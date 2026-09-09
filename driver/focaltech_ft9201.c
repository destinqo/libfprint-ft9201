/*
 * FocalTech FT9201 driver for libfprint
 * Copyright (C) 2026 Miroslav Baranko <miroslav.baranko@upjs.sk>
 *
 * The knowledge of the protocol comes from two sources: the reverse
 * engineering of banianitc
 * (https://github.com/banianitc/ft9201-fingerprint-driver), and USB
 * captures of the vendor Windows driver. This driver is an independent
 * implementation for libfprint.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Notes on the protocol
 * ---------------------
 * The FT9201 is an image sensor of the press type. It is not a swipe
 * sensor, and it does not do the match operation in the chip. The bulk IN
 * endpoint gives one full raw grayscale frame for each capture. Vendor
 * control transfers do all the other work. They read and write the
 * registers. They arm the capture engine. They also give the length of
 * the next bulk transfer.
 *
 * The sensor also keeps no firmware. Its 8051 core starts with an empty
 * code RAM after each loss of VBUS. The host must send an image to the
 * bulk OUT endpoint before an AFE register gives an answer. The table
 * ft9201_firmware_script below does this.
 *
 * This is the full explanation of a known fault: users had to connect the
 * sensor to a Windows machine to make it operate again. The Windows
 * driver sent the firmware. It did not repair a bad state. Refer to
 * FT9201_FIRMWARE_PATH in the header file.
 *
 * The source of the sequence is a USBPcap capture. It shows the Windows
 * vendor driver during a complete enrolment. From it come the request
 * numbers, the register indices and their order. Each step was then sent again to
 * the device until the device gave frames. Some steps below have the note
 * "purpose unknown". Each of those steps is necessary, because the
 * download fails without it, but its effect on the chip is not known.
 */

#define FP_COMPONENT "focaltech_ft9201"

#include <errno.h>
#include <math.h>

#include "drivers_api.h"
#include "focaltech_ft9201.h"

struct _FpiDeviceFocaltechFt9201
{
  FpDevice parent;

  /* Device identity, read once during activation. */
  guint16 chip_id;
  guint8  sensor_width;
  guint8  sensor_height;
  guint8  sensor_type;          /* from the SFR 0x00f3, 0xff = not read */

  /* Scratch space holding the response of the last register read. Only
   * read again once the corresponding *_run_state case has issued the
   * read that fills it; treat as write-once/read-once. */
  guint8 reg_buf[4];

  /* Bounded retry counter, reused (and reset to 0) by each of the
   * "poll until ready" loops during activation. */
  guint retry_count;

  /* MCU firmware image, read from disk on demand and released as soon as
   * the activation that needed it finishes. NULL whenever the MCU was
   * already running and no download was required. */
  GBytes *fw_data;
  /* Index into ft9201_firmware_script while the download runs. */
  guint   fw_step;

  /* Capture bookkeeping. */
  FpiSsm  *task_ssm;             /* the running action, or NULL when idle */
  guint    spurious_polls;       /* finger reported but frame not usable */
  guint    idle_polls;           /* polls since the last finger or renewal */
  guint    rearm_retries;        /* re-arms that did not put the MCU back to work */
  gboolean fw_reloaded;          /* the recovery download ran in this activation */
  guint8  *frame;                /* last captured frame, w*h bytes */

  /* Which action the shared capture machine is serving, and its state. */
  FpiDeviceAction action;
  GPtrArray      *enroll_frames; /* GBytes, one per completed enroll stage */
};
G_DEFINE_TYPE (FpiDeviceFocaltechFt9201, fpi_device_focaltech_ft9201,
               FP_TYPE_DEVICE);

/****** LOW-LEVEL USB HELPERS ******/

static void
ft9201_ctrl_done (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* IN transfers (register reads) have a non-zero requested length; OUT
   * transfers (register writes, action requests) do not carry a response
   * to store. */
  if (transfer->length > 0)
    {
      memset (self->reg_buf, 0, sizeof (self->reg_buf));
      memcpy (self->reg_buf, transfer->buffer,
              MIN ((gsize) transfer->actual_length, sizeof (self->reg_buf)));
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
ft9201_ctrl_in (FpiSsm *ssm, FpDevice *dev, guint8 request, guint16 value,
                guint16 index, gsize length)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 request, value, index, length);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, FT9201_CTRL_TIMEOUT_MS,
                           fpi_device_get_cancellable (dev),
                           ft9201_ctrl_done, NULL);
}

static void
ft9201_ctrl_out (FpiSsm *ssm, FpDevice *dev, guint8 request, guint16 value,
                 guint16 index)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_control (transfer,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 request, value, index, 0);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, FT9201_CTRL_TIMEOUT_MS,
                           fpi_device_get_cancellable (dev),
                           ft9201_ctrl_done, NULL);
}

/* Reads an AFE register. The answer has 4 bytes: the registers [n] and
 * [n+1], and then two zero bytes. For a usual register the driver reads
 * only reg_buf[0]. For the MCU status register it also reads reg_buf[1].
 * Refer to ft9201_is_mcu_ready(). */
static void
ft9201_read_register (FpiSsm *ssm, FpDevice *dev, guint8 reg)
{
  ft9201_ctrl_in (ssm, dev, FT9201_REQ_READ_REGISTER, 0, reg, 4);
}

static void
ft9201_write_register (FpiSsm *ssm, FpDevice *dev, guint8 reg, guint8 value)
{
  ft9201_ctrl_out (ssm, dev, FT9201_REQ_WRITE_REGISTER, value, reg);
}

static gboolean
ft9201_is_mcu_ready (FpiDeviceFocaltechFt9201 *self)
{
  return self->reg_buf[0] == FT9201_MCU_STATE_MAGIC_A5 &&
         self->reg_buf[1] == FT9201_MCU_STATE_MAGIC_5A;
}

/* Gives TRUE while the MCU gives an answer, in the idle state ("a5 5a")
 * or in the busy state ("01 01"). An MCU without firmware gives zeroes
 * for each register read. */
static gboolean
ft9201_is_mcu_alive (FpiDeviceFocaltechFt9201 *self)
{
  return ft9201_is_mcu_ready (self) ||
         (self->reg_buf[0] == FT9201_MCU_STATE_BUSY &&
          self->reg_buf[1] == FT9201_MCU_STATE_BUSY);
}

/*
 * The last recovery step. The activation now includes the firmware
 * download. Thus the driver corrects the initial fault, when each
 * register gives zero, before this function. This function operates only
 * when the sensor has a different fault. A bus reset has a low cost and
 * can correct a temporary fault of the enumeration. It does no more than
 * that.
 *
 * The driver uses this function only in the failure paths of the
 * activation. It does not use it for the usual transfer errors of the
 * capture loop. A temporary error of one bulk read is not equal to a
 * device that does not start. A bus reset during a capture session causes
 * more trouble than help.
 */
static void
ft9201_recover_and_fail (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  GError *reset_error = NULL;

  fp_warn ("FT9201 activation failed (%s); attempting a USB reset before "
           "giving up.", error->message);

  if (!g_usb_device_reset (usb_dev, &reset_error))
    {
      fp_warn ("FT9201 USB reset attempt itself failed: %s", reset_error->message);
      g_clear_error (&reset_error);
    }

  fpi_ssm_mark_failed (ssm, error);
}

/****** MCU FIRMWARE DOWNLOAD ******/

/*
 * The download is a table and not one state for each request. The
 * download is a fixed sequence: the driver sends the steps in their
 * order, and it makes no decisions. A table keeps the sequence easy to
 * compare with the USB capture that is its source.
 */
typedef enum {
  FT9201_OP_END = 0,
  FT9201_OP_OUT,        /* vendor control OUT: request, value, index */
  FT9201_OP_IN,         /* vendor control IN: request, value, index, length */
  FT9201_OP_DELAY,      /* value = milliseconds */
  FT9201_OP_SEND_PROBE, /* bulk-out the 4-byte probe word */
  FT9201_OP_SEND_IMAGE, /* bulk-out the whole firmware image */
} Ft9201OpKind;

typedef struct
{
  guint8  kind;
  guint8  request;
  guint16 value;
  guint16 index;
  guint8  length;
} Ft9201Op;

#define FW_OUT(req, val, idx)     { FT9201_OP_OUT, (req), (val), (idx), 0 }
#define FW_IN(req, val, idx, len) { FT9201_OP_IN, (req), (val), (idx), (len) }
#define FW_DELAY(ms)              { FT9201_OP_DELAY, 0, (ms), 0, 0 }
#define FW_SEND_PROBE { FT9201_OP_SEND_PROBE, 0, 0, 0, 0 }
#define FW_SEND_IMAGE { FT9201_OP_SEND_IMAGE, 0, 0, 0, 0 }
#define FW_END { FT9201_OP_END, 0, 0, 0, 0 }

/* The handshake of two requests that prepares the MCU to give answers.
 * It occurs more than one time, with different delays between the two
 * requests. */
#define FW_PREPARE_MCU(gap_ms)                                    \
  FW_OUT (FT9201_REQ_PREPARE_MCU_CHECK, 0x0070, 0x0070),          \
  FW_DELAY (gap_ms),                                              \
  FW_OUT (FT9201_REQ_PREPARE_MCU_CHECK, 0x0070, 0x0070),          \
  FW_DELAY (32)

/* One of the five equal MCU write and read pairs at the start of the
 * sequence. */
#define FW_MCU_PAIR                                               \
  FW_OUT (FT9201_REQ_MCU_WRITE, 0x9001, 0x0000),                  \
  FW_IN (FT9201_REQ_MCU_READ, 0, 0x0000, 4)

/* One of the three groups of three requests that unlock the code RAM.
 * Each group writes 0x55 to the location 0xc2. */
#define FW_CODE_RAM_UNLOCK                                        \
  FW_OUT (FT9201_REQ_MCU_SYNC, 0, 0),                             \
  FW_OUT (FT9201_REQ_CODE_RAM_WRITE, 0x0055, 0x00c2),             \
  FW_IN (FT9201_REQ_CODE_RAM_READ, 0, 0x00c2, 4)

static const Ft9201Op ft9201_firmware_script[] = {
  /* The vendor driver does the wake handshake and the read of the
   * register 0x20 at this point. This driver does them in the
   * ACTIVATE_WAKE_* states, because the activation must make a decision
   * from their result. The order on the bus stays the same. */
  FW_IN (FT9201_REQ_READ_REGISTER, 0, FT9201_REG_CHIP_ID_HIGH, 4),
  FW_IN (FT9201_REQ_READ_REGISTER, 0, FT9201_REG_CHIP_ID_LOW, 4),

  /* Five write and read pairs in the register space of the MCU, and then
   * one different write. Purpose unknown. */
  FW_MCU_PAIR,
  FW_MCU_PAIR,
  FW_MCU_PAIR,
  FW_MCU_PAIR,
  FW_MCU_PAIR,
  FW_OUT (FT9201_REQ_MCU_WRITE, 0x0603, 0x00f9),

  /* A probe write of four bytes, and then a check that gives
   * 0x50 0x2b. Purpose unknown. The download fails without this step. */
  FW_OUT (FT9201_REQ_SET_BULK_MODE, FT9201_BULK_MODE_OUT, 0),
  FW_OUT (FT9201_REQ_SET_BULK_TRANSFER, FT9201_FW_PROBE_SIZE,
          FT9201_FW_PROBE_ADDR),
  FW_SEND_PROBE,
  FW_DELAY (9),
  FW_OUT (FT9201_REQ_UNKNOWN_03, 0x0001, 0x00a4),
  FW_IN (FT9201_REQ_BULK_CHECK, 0, FT9201_FW_PROBE_ADDR, 4),

  /* Writes to five special function registers of the 8051 core. Purpose
   * unknown. */
  FW_OUT (FT9201_REQ_MCU_SYNC, 0, 0),
  FW_IN (FT9201_REQ_SFR_READ, 0, 0x00c8, 4),
  FW_OUT (FT9201_REQ_SFR_WRITE, 0x00df, 0x00c8),
  FW_OUT (FT9201_REQ_SFR_WRITE, 0x001d, 0x00f1),
  FW_IN (FT9201_REQ_SFR_READ, 0, 0x00f4, 4),
  FW_OUT (FT9201_REQ_SFR_WRITE, 0x0001, 0x00f4),
  FW_IN (FT9201_REQ_SFR_READ, 0, 0x00f3, 4),
  FW_IN (FT9201_REQ_GET_SUI_VERSION, 0, 0, 4),

  FW_PREPARE_MCU (15),
  FW_IN (FT9201_REQ_READ_REGISTER, 0, FT9201_REG_MCU_SENSOR_STATUS, 4),

  /* Unlocks the code RAM for write operations. */
  FW_CODE_RAM_UNLOCK,
  FW_CODE_RAM_UNLOCK,
  FW_CODE_RAM_UNLOCK,

  /* The download. The driver gives the length in units of 64 bytes, and
   * sends the image in one bulk transfer. The maximum packet size of the
   * endpoint is 16 bytes, and the length of the image is a multiple of
   * 16. Thus the packets on the bus are equal to the packets of the
   * vendor driver, which writes 64 bytes at one time. */
  FW_OUT (FT9201_REQ_SET_BULK_MODE, FT9201_BULK_MODE_RESET, 0),
  FW_DELAY (22),
  FW_OUT (FT9201_REQ_SET_BULK_MODE, FT9201_BULK_MODE_OUT, 0),
  FW_OUT (FT9201_REQ_SET_BULK_TRANSFER, FT9201_FW_CHUNK_SIZE,
          FT9201_FW_LOAD_ADDR),
  FW_SEND_IMAGE,
  FW_DELAY (15),

  /* Gives control to the new code, and then waits. The code needs this
   * time to start before it can answer a request. */
  FW_OUT (FT9201_REQ_FIRMWARE_COMMIT, 0, 0),
  FW_DELAY (31),
  FW_OUT (FT9201_REQ_FIRMWARE_COMMIT, 0, 0),
  FW_DELAY (200),
  FW_PREPARE_MCU (3),

  FW_END,
};

/* The probe word for the address FT9201_FW_PROBE_ADDR. It is not const,
 * because fpi_usb_transfer_fill_bulk_full() takes a buffer that is not
 * const. That function marks the buffer as read only, and this driver
 * does not change the word. */
static guint8 ft9201_fw_probe_word[FT9201_FW_PROBE_SIZE] = {
  0x11, 0xee, 0x02, 0x00
};

static gboolean
ft9201_load_firmware (FpiDeviceFocaltechFt9201 *self, GError **error)
{
  g_autofree gchar *data = NULL;
  gsize len = 0;

  if (self->fw_data != NULL)
    return TRUE;

  if (!g_file_get_contents (FT9201_FIRMWARE_PATH, &data, &len, NULL))
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                   "the sensor has no firmware loaded and "
                                                   "%s could not be read. This sensor keeps no "
                                                   "persistent firmware, so the image has to be "
                                                   "extracted from the vendor driver and installed "
                                                   "at that path -- see the driver README.",
                                                   FT9201_FIRMWARE_PATH));
      return FALSE;
    }

  /* The file is an input that the driver cannot trust. Thus the driver
   * puts limits on its length, and makes sure that the length agrees with
   * the value that it gives to the device. It does not send an unknown
   * quantity of data to the code RAM of the sensor. */
  if (len < FT9201_FW_MIN_SIZE || len > FT9201_FW_MAX_SIZE ||
      len % FT9201_FW_CHUNK_SIZE != 0)
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                   "%s is not a usable firmware image: %"
                                                   G_GSIZE_FORMAT " bytes, expected a multiple of %d "
                                                   "between %d and %d.",
                                                   FT9201_FIRMWARE_PATH, len,
                                                   FT9201_FW_CHUNK_SIZE,
                                                   FT9201_FW_MIN_SIZE, FT9201_FW_MAX_SIZE));
      return FALSE;
    }

  fp_dbg ("FT9201 firmware %s: %" G_GSIZE_FORMAT " bytes", FT9201_FIRMWARE_PATH, len);
  self->fw_data = g_bytes_new_take (g_steal_pointer (&data), len);
  return TRUE;
}

static void
ft9201_bulk_out_done (FpiUsbTransfer *transfer, FpDevice *dev,
                      gpointer user_data, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (transfer->ssm, error);
  else
    fpi_ssm_next_state (transfer->ssm);
}

static void
ft9201_bulk_out (FpiSsm *ssm, FpDevice *dev, guint8 *buffer, gsize length,
                 guint timeout_ms)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, FT9201_EP_FW_OUT, buffer,
                                   length, NULL);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, timeout_ms,
                           fpi_device_get_cancellable (dev),
                           ft9201_bulk_out_done, NULL);
}


/*
 * Reads the answer of one SFR read of the download script.
 *
 * The download script sends a group of reads and writes to the SFR space
 * of the 8051 core. It comes from a capture of the vendor driver, and the
 * driver sends it without a change. Two of those reads carry data:
 *
 * - The location FT9201_SFR_CHIP_TYPE gives the type of the sensor.
 * - The location FT9201_SFR_TYPE_LATCH gives a latch. The next step writes
 *   the constant 0x0001 to it. The vendor writes the value that it read,
 *   with the bit 0 set. The two operations differ when the latch is not
 *   0x00, thus the driver writes a warning in that condition.
 *
 * The driver does not use the type. It only puts the type in the log,
 * because the correct firmware image for this sensor is an open question.
 */
static void
ft9201_note_sfr_read (FpiDeviceFocaltechFt9201 *self, guint16 index)
{
  guint8 raw = self->reg_buf[0];

  if (index == FT9201_SFR_CHIP_TYPE)
    {
      self->sensor_type = (raw >> 1) & 0x0f;
      fp_dbg ("FT9201 SFR 0x%04x reads 0x%02x, thus the sensor type is %u",
              index, raw, self->sensor_type);
    }
  else if (index == FT9201_SFR_TYPE_LATCH)
    {
      fp_dbg ("FT9201 SFR 0x%04x reads 0x%02x", index, raw);
      if ((raw | 0x01) != 0x01)
        {
          fp_warn ("FT9201 SFR 0x%04x reads 0x%02x. The driver writes 0x01 to "
                   "it, and the vendor writes 0x%02x. Refer to the open "
                   "questions in the documentation.",
                   index, raw, raw | 0x01);
        }
    }
}

/****** ACTIVATION (firmware, device identification, arming) ******/

enum activate_states {
  ACTIVATE_READ_MCU_STATE,
  ACTIVATE_CHECK_MCU_STATE,

  /* The driver uses these states when the first read gives an empty
   * answer. An idle AFE also gives zeroes. Thus the driver sends the wake
   * handshake and reads again before it decides that the sensor has no
   * firmware. */
  ACTIVATE_WAKE_1,
  ACTIVATE_WAKE_1_DELAY,
  ACTIVATE_WAKE_2,
  ACTIVATE_WAKE_2_DELAY,
  ACTIVATE_REREAD_MCU_STATE,
  ACTIVATE_RECHECK_MCU_STATE,

  /* The driver uses these states only when the MCU still gives no
   * answer. */
  ACTIVATE_FW_STEP,
  ACTIVATE_FW_NEXT,

  ACTIVATE_WAIT_READY_READ,
  ACTIVATE_WAIT_READY_CHECK,
  /* On some firmware revisions the MCU comes back only when the driver
   * sends the wake handshake again between two polls. Refer to
   * ACTIVATE_WAIT_READY_CHECK. */
  ACTIVATE_WAIT_READY_NUDGE_1,
  ACTIVATE_WAIT_READY_NUDGE_DELAY_1,
  ACTIVATE_WAIT_READY_NUDGE_2,
  ACTIVATE_WAIT_READY_NUDGE_DELAY_2,

  ACTIVATE_READ_CHIP_ID_HIGH,
  ACTIVATE_STORE_CHIP_ID_HIGH,
  ACTIVATE_READ_CHIP_ID_LOW,
  ACTIVATE_STORE_CHIP_ID_LOW,
  ACTIVATE_READ_DIM_WIDTH,
  ACTIVATE_STORE_DIM_WIDTH,
  ACTIVATE_READ_DIM_HEIGHT,
  ACTIVATE_STORE_DIM_HEIGHT,

  ACTIVATE_CONFIG_WRITE_22,
  ACTIVATE_CONFIG_WRITE_23,

  ACTIVATE_READ_CAPTURE_READY,
  ACTIVATE_CHECK_CAPTURE_READY,
  ACTIVATE_ARM_WRITE_01,
  ACTIVATE_ARM_WRITE_41,
  ACTIVATE_ARM_WRITE_30,
  ACTIVATE_ARM_VERIFY_READ,
  ACTIVATE_ARM_VERIFY_CHECK,

  ACTIVATE_AUTO_POWER_POLL_READ,
  ACTIVATE_AUTO_POWER_POLL_CHECK,
  ACTIVATE_AUTO_POWER_SET_1,
  ACTIVATE_AUTO_POWER_SET_2,
  ACTIVATE_AUTO_POWER_DELAY,
  ACTIVATE_AUTO_POWER_POLL_2_READ,
  ACTIVATE_AUTO_POWER_POLL_2_CHECK,
  /* The last three reads of an activation of the vendor driver. Their
   * capture ends with the value 01 01 in the register 0x20, which shows
   * that the MCU looks for a finger. If an activation ends with an idle
   * MCU, the sensor reports no press. */
  ACTIVATE_AUTO_POWER_PRESENT_READ,
  ACTIVATE_AUTO_POWER_HUNTING_READ,
  ACTIVATE_AUTO_POWER_HUNTING_CHECK,
  ACTIVATE_NUM_STATES,
};

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_MCU_STATE:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_CHECK_MCU_STATE:
      /* An MCU that gives an answer still holds its firmware in RAM from
       * an earlier activation. Thus the driver does not do the download.
       * The vendor driver uses the same test. */
      if (ft9201_is_mcu_alive (self))
        fpi_ssm_jump_to_state (ssm, ACTIVATE_WAIT_READY_READ);
      else
        fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_WAKE_1:
    case ACTIVATE_WAKE_2:
      ft9201_ctrl_out (ssm, dev, FT9201_REQ_PREPARE_MCU_CHECK, 0x0070, 0x0070);
      break;

    case ACTIVATE_WAKE_1_DELAY:
      fpi_ssm_next_state_delayed (ssm, 17);
      break;

    case ACTIVATE_WAKE_2_DELAY:
      fpi_ssm_next_state_delayed (ssm, 32);
      break;

    case ACTIVATE_REREAD_MCU_STATE:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_RECHECK_MCU_STATE:
      /* Zeroes are not proof of missing firmware. The AFE removes its own
       * power when no step arms it again. It then gives zeroes for each
       * register read until the wake handshake above starts it again. Only
       * a device that stays silent after the handshake has an empty code
       * RAM. The vendor driver uses the same test before a download. */
      if (ft9201_is_mcu_alive (self))
        {
          fpi_ssm_jump_to_state (ssm, ACTIVATE_WAIT_READY_READ);
        }
      else
        {
          GError *error = NULL;

          fp_dbg ("FT9201 MCU is not answering after a wake handshake "
                  "(register 0x%02x reads %02x %02x); downloading firmware",
                  FT9201_REG_MCU_SENSOR_STATUS,
                  self->reg_buf[0], self->reg_buf[1]);

          if (!ft9201_load_firmware (self, &error))
            {
              fpi_ssm_mark_failed (ssm, error);
              return;
            }

          self->fw_step = 0;
          fpi_ssm_next_state (ssm);
        }
      break;

    case ACTIVATE_FW_STEP:
      {
        const Ft9201Op *op = &ft9201_firmware_script[self->fw_step];

        switch (op->kind)
          {
          case FT9201_OP_END:
            fpi_ssm_jump_to_state (ssm, ACTIVATE_WAIT_READY_READ);
            break;

          case FT9201_OP_OUT:
            ft9201_ctrl_out (ssm, dev, op->request, op->value, op->index);
            break;

          case FT9201_OP_IN:
            ft9201_ctrl_in (ssm, dev, op->request, op->value, op->index,
                            op->length);
            break;

          case FT9201_OP_DELAY:
            fpi_ssm_next_state_delayed (ssm, op->value);
            break;

          case FT9201_OP_SEND_PROBE:
            ft9201_bulk_out (ssm, dev, ft9201_fw_probe_word,
                             sizeof (ft9201_fw_probe_word),
                             FT9201_BULK_TIMEOUT_MS);
            break;

          case FT9201_OP_SEND_IMAGE:
            {
              gsize len = 0;
              const guint8 *data = g_bytes_get_data (self->fw_data, &len);

              /* The cast is safe. fill_bulk_full() marks this argument as
               * read only, and self->fw_data stays valid for more time
               * than the transfer. */
              ft9201_bulk_out (ssm, dev, (guint8 *) data, len,
                               FT9201_FW_TIMEOUT_MS);
            }
            break;

          default:
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "bad firmware script opcode %u at step %u",
                                                           op->kind, self->fw_step));
            break;
          }
      }
      break;

    case ACTIVATE_FW_NEXT:
      {
        const Ft9201Op *done = &ft9201_firmware_script[self->fw_step];

        if (done->kind == FT9201_OP_IN &&
            done->request == FT9201_REQ_SFR_READ)
          ft9201_note_sfr_read (self, done->index);
      }
      self->fw_step++;
      fpi_ssm_jump_to_state (ssm, ACTIVATE_FW_STEP);
      break;

    case ACTIVATE_WAIT_READY_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_WAIT_READY_CHECK:
      /* The driver cannot trust a value that it reads before this point.
       * A busy MCU gives its status bytes for each register read. Thus an
       * early read of the sensor dimensions gives "01 01", and a correct
       * 96x96 sensor has the appearance of a 1x1 sensor. */
      if (ft9201_is_mcu_ready (self))
        {
          self->retry_count = 0;
          /* The driver jumps and does not go to the next state. The
          * wake states are between this state and the identity reads. A
          * move into them after a success makes an endless loop. */
          fpi_ssm_jump_to_state (ssm, ACTIVATE_READ_CHIP_ID_HIGH);
        }
      else if (self->retry_count >= FT9201_MCU_READY_MAX_POLLS)
        {
          self->retry_count = 0;
          ft9201_recover_and_fail (ssm, dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Sensor MCU never became ready: register 0x%02x "
                                                             "reads %02x %02x, expected %02x %02x",
                                                             FT9201_REG_MCU_SENSOR_STATUS,
                                                             self->reg_buf[0], self->reg_buf[1],
                                                             FT9201_MCU_STATE_MAGIC_A5,
                                                             FT9201_MCU_STATE_MAGIC_5A));
        }
      else
        {
          /* A second read alone is not sufficient. On the firmware
           * revision that reports 64x80, the register 0x20 stays at
           * "01 01" for all time. It gives "a5 5a" only immediately after
           * a wake handshake. Thus the driver sends the handshake again
           * between the reads. */
          self->retry_count++;
          fpi_ssm_next_state (ssm);
        }
      break;

    case ACTIVATE_WAIT_READY_NUDGE_1:
    case ACTIVATE_WAIT_READY_NUDGE_2:
      ft9201_ctrl_out (ssm, dev, FT9201_REQ_PREPARE_MCU_CHECK, 0x0070, 0x0070);
      break;

    case ACTIVATE_WAIT_READY_NUDGE_DELAY_1:
      fpi_ssm_next_state_delayed (ssm, 16);
      break;

    case ACTIVATE_WAIT_READY_NUDGE_DELAY_2:
      fpi_ssm_jump_to_state_delayed (ssm, ACTIVATE_WAIT_READY_READ, 32);
      break;

    case ACTIVATE_READ_CHIP_ID_HIGH:
      ft9201_read_register (ssm, dev, FT9201_REG_CHIP_ID_HIGH);
      break;

    case ACTIVATE_STORE_CHIP_ID_HIGH:
      self->chip_id = ((guint16) self->reg_buf[0]) << 8;
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_READ_CHIP_ID_LOW:
      ft9201_read_register (ssm, dev, FT9201_REG_CHIP_ID_LOW);
      break;

    case ACTIVATE_STORE_CHIP_ID_LOW:
      self->chip_id |= self->reg_buf[0];
      fp_dbg ("FT9201 AFE chip id: 0x%04x", self->chip_id);
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_READ_DIM_WIDTH:
      ft9201_read_register (ssm, dev, FT9201_REG_SENSOR_WIDTH);
      break;

    case ACTIVATE_STORE_DIM_WIDTH:
      self->sensor_width = self->reg_buf[0];
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_READ_DIM_HEIGHT:
      ft9201_read_register (ssm, dev, FT9201_REG_SENSOR_HEIGHT);
      break;

    case ACTIVATE_STORE_DIM_HEIGHT:
      self->sensor_height = self->reg_buf[0];
      fp_dbg ("FT9201 sensor dimensions: %ux%u, chip ID 0x%04x, type %u",
              self->sensor_width, self->sensor_height, self->chip_id,
              self->sensor_type);
      if (self->sensor_width == 0 || self->sensor_height == 0)
        {
          ft9201_recover_and_fail (ssm, dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Sensor reported zero dimensions"));
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_CONFIG_WRITE_22:
      /* The driver writes these two registers during each activation,
       * warm or cold. An earlier version wrote them only when the sensor
       * reported dimensions different from the default 0x60 x 0x60. Thus
       * it never wrote them. The vendor driver always writes them. */
      ft9201_write_register (ssm, dev, FT9201_REG_SENSOR_CONFIG_22, 0x00);
      break;

    case ACTIVATE_CONFIG_WRITE_23:
      ft9201_write_register (ssm, dev, FT9201_REG_SENSOR_CONFIG_23, 0x0e);
      break;

    case ACTIVATE_READ_CAPTURE_READY:
      ft9201_read_register (ssm, dev, FT9201_REG_CAPTURE_READY);
      break;

    case ACTIVATE_CHECK_CAPTURE_READY:
      /*
       * The driver always arms the sensor, also when the register 0x30
       * already reads 0xbb.
       *
       * To omit the arm writes on an armed sensor has the appearance of
       * a correct decision. The warm enumerations of the vendor driver
       * also omit them. But a measurement shows that this stops the finger
       * detection. Without the writes, the request 0x43 gives 00 for each
       * poll. A simple poll program that does no arm operation sees
       * presses in the same minute, with the same finger. That program saw
       * 49 detections in 130 polls. This driver saw 0 in 85 polls.
       *
       * Only one vendor session continued to the polls of the request
       * 0x43, and that session was a cold one with the full sequence. The
       * warm sessions stopped after the enumeration. Thus they do not show
       * if their short sequence keeps the detection. It does not.
       *
       * The driver keeps the register read below. Then the value is in the
       * log when a fault occurs.
       */
      fp_dbg ("FT9201 register 0x%02x reads 0x%02x before arming",
              FT9201_REG_CAPTURE_READY, self->reg_buf[0]);
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_ARM_WRITE_01:
      ft9201_write_register (ssm, dev, FT9201_REG_UNKNOWN_01, 0x01);
      break;

    case ACTIVATE_ARM_WRITE_41:
      ft9201_write_register (ssm, dev, FT9201_REG_UNKNOWN_41, 0x0f);
      break;

    case ACTIVATE_ARM_WRITE_30:
      /*
       * The true arm step. The sensor does not set the value 0xbb. Thus a
       * driver that only polls this register sees 0x00 for all time, and
       * makes the incorrect conclusion that the hardware has a fault.
       *
       * This write has two effects, and only one of them is known. A test
       * removed the write, because a search through the recovery actions
       * showed live finger detection while the register 0x30 read 0x00.
       * That search gave 15, 50, 39 and 17 detections in four sequential
       * windows, and no detection in each window after a write of 0xbb.
       *
       * Without the write the detection did come back, and an enrolment
       * completed 8 stages of 8 with no discarded frame. But the frames no
       * longer agreed with each other. The correct finger then scored
       * 0.029 to 0.032, which is the noise level of a different finger.
       * With the write the same finger scored 0.167 to 0.293.
       *
       * Thus the write arms the imaging path, and it can also stop the
       * finger interrupt in some state. The interaction is not known. The
       * driver keeps the write, because tests measured a correct
       * enrolment, an accepted finger and a rejected finger only in this
       * configuration. Refer to "Known issues" in the README.
       */
      ft9201_write_register (ssm, dev, FT9201_REG_CAPTURE_READY,
                             FT9201_CAPTURE_READY_MAGIC);
      break;

    case ACTIVATE_ARM_VERIFY_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_CAPTURE_READY);
      break;

    case ACTIVATE_ARM_VERIFY_CHECK:
      if (self->reg_buf[0] != FT9201_CAPTURE_READY_MAGIC)
        {
          ft9201_recover_and_fail (ssm, dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Capture engine did not arm: register 0x%02x "
                                                             "reads 0x%02x after being written 0x%02x",
                                                             FT9201_REG_CAPTURE_READY,
                                                             self->reg_buf[0],
                                                             FT9201_CAPTURE_READY_MAGIC));
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_AUTO_POWER_POLL_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_AUTO_POWER_POLL_CHECK:
      if (ft9201_is_mcu_ready (self))
        {
          self->retry_count = 0;
          fpi_ssm_next_state (ssm);
        }
      else if (self->retry_count >= FT9201_AUTO_POWER_MAX_RETRY)
        {
          self->retry_count = 0;
          ft9201_recover_and_fail (ssm, dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Sensor MCU did not power up after %d attempts",
                                                             FT9201_AUTO_POWER_MAX_RETRY));
        }
      else
        {
          self->retry_count++;
          fpi_ssm_jump_to_state_delayed (ssm, ACTIVATE_AUTO_POWER_POLL_READ,
                                         10);
        }
      break;

    case ACTIVATE_AUTO_POWER_SET_1:
      ft9201_write_register (ssm, dev, FT9201_REG_AUTO_POWER_1, 1);
      break;

    case ACTIVATE_AUTO_POWER_SET_2:
      ft9201_write_register (ssm, dev, FT9201_REG_AUTO_POWER_2, 1);
      break;

    case ACTIVATE_AUTO_POWER_DELAY:
      fpi_ssm_next_state_delayed (ssm, 20);
      break;

    case ACTIVATE_AUTO_POWER_POLL_2_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_AUTO_POWER_POLL_2_CHECK:
      if (ft9201_is_mcu_ready (self))
        {
          self->retry_count = 0;
          fpi_ssm_next_state (ssm);
        }
      else if (self->retry_count >= FT9201_AUTO_POWER_MAX_RETRY)
        {
          self->retry_count = 0;
          ft9201_recover_and_fail (ssm, dev,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Sensor MCU did not confirm auto-power after %d attempts",
                                                             FT9201_AUTO_POWER_MAX_RETRY));
        }
      else
        {
          self->retry_count++;
          fpi_ssm_jump_to_state_delayed (ssm, ACTIVATE_AUTO_POWER_POLL_2_READ,
                                         1);
        }
      break;

    case ACTIVATE_AUTO_POWER_PRESENT_READ:
      /* The driver reads this register for its effect on the sensor, as
       * the vendor driver does. With no finger on the sensor the value has
       * no use. */
      ft9201_read_register (ssm, dev, FT9201_REG_FINGER_PRESENT);
      break;

    case ACTIVATE_AUTO_POWER_HUNTING_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case ACTIVATE_AUTO_POWER_HUNTING_CHECK:
      /* The value 01 01 shows that the MCU is busy. The driver armed the
       * automatic finger detection immediately before this read. Thus the
       * MCU is busy with the search for a finger. The activation of the
       * vendor driver also ends in this state. If the activation ends with
       * an idle MCU, the sensor gives 0x00 for each finger poll for the
       * remainder of the session. Therefore the driver arms the sensor
       * again and does not continue. */
      if (self->reg_buf[0] == FT9201_MCU_STATE_BUSY)
        {
          self->rearm_retries = 0;
          fp_dbg ("FT9201 activated, MCU hunting for a finger");
          fpi_ssm_next_state (ssm);
        }
      else if (self->rearm_retries < FT9201_REARM_MAX_RETRY)
        {
          self->rearm_retries++;
          fp_dbg ("FT9201 activation left the MCU idle (register 0x%02x "
                  "reads 0x%02x 0x%02x); re-arming (%u of %d)",
                  FT9201_REG_MCU_SENSOR_STATUS, self->reg_buf[0],
                  self->reg_buf[1], self->rearm_retries,
                  FT9201_REARM_MAX_RETRY);
          fpi_ssm_jump_to_state_delayed (ssm, ACTIVATE_AUTO_POWER_SET_1, 14);
        }
      else if (!self->fw_reloaded)
        {
          /*
           * The arm writes did not start the search. The driver now sends
           * the firmware again, and then it arms the sensor again.
           *
           * The sensor answers each register in this condition, thus it
           * has its firmware. But the search for a finger does not start,
           * and each poll of the request 0x43 gives 00. Before this step
           * the only correct operation was a power cycle of the USB port,
           * which the user cannot always do.
           *
           * A test sent the image to a sensor that already had it. The
           * download gave the status a5 5a, and the sensor then captured
           * frames. Thus the operation is safe on a sensor that operates.
           * A test on a sensor in this condition is not possible on
           * demand, because the condition is not deterministic.
           */
          GError *error = NULL;

          self->rearm_retries = 0;
          self->fw_reloaded = TRUE;
          fp_warn ("FT9201 sensor did not start hunting for a finger after "
                   "%d attempts (register 0x%02x reads 0x%02x 0x%02x); "
                   "sending the firmware again", FT9201_REARM_MAX_RETRY,
                   FT9201_REG_MCU_SENSOR_STATUS, self->reg_buf[0],
                   self->reg_buf[1]);

          if (!ft9201_load_firmware (self, &error))
            {
              fpi_ssm_mark_failed (ssm, error);
              return;
            }

          self->fw_step = 0;
          fpi_ssm_jump_to_state (ssm, ACTIVATE_FW_STEP);
        }
      else
        {
          /* The second attempt also did not start the search. This is not
           * a fatal fault. The sensor is correct in all other conditions,
           * and the caller can still get a frame if a finger arrives at
           * the correct moment. Thus the driver gives a warning and does
           * not stop the enrolment. */
          self->rearm_retries = 0;
          fp_warn ("FT9201 sensor did not start hunting for a finger, also "
                   "after a second firmware download (register 0x%02x "
                   "reads 0x%02x 0x%02x); presses may go unnoticed until "
                   "the device is power-cycled",
                   FT9201_REG_MCU_SENSOR_STATUS, self->reg_buf[0],
                   self->reg_buf[1]);
          fpi_ssm_next_state (ssm);
        }
      break;
    }
}


/****** MATCHER ******/

/*
 * The reason for a match by correlation and not by minutiae.
 *
 * The image device path of libfprint uses NBIS bozorth3 for the match
 * operation. That algorithm gives no score when one side has fewer than
 * MIN_COMPUTABLE_BOZORTH_MINUTIAE (10) minutiae. A 96x96 frame from this
 * sensor gives 1 or 2 minutiae.
 *
 * A test measured this on 23 frames. It used each combination of the
 * image flags, the ppmm value and a scale of 1x to 4x. It also used
 * histogram equalisation, CLAHE, unsharp masking and a band-pass filter.
 * The count stayed at 2 or fewer. NBIS also reports that the frames have
 * a good quality. Thus the image quality is not the cause. A square of
 * 4.5 mm on a fingertip does not contain ten minutiae.
 *
 * On an area of this size, a comparison of the ridge pattern operates
 * correctly. Therefore this driver is a usual FpDevice with its own match
 * operation, and not an FpImageDevice. The driver applies a band-pass
 * filter at the ridge frequency, masks the area that the finger does not
 * touch. It then finds the best normalised cross correlation. The search
 * moves and turns one frame against the other.
 *
 * No part of this comes from a vendor binary. It is the usual correlation
 * method for sensors with a small area.
 */

/* A measurement on this sensor gives a primary ridge wavelength of 9.6 to
 * 13.7 pixels in 23 frames, with a median of 10.7. The band-pass filter
 * uses this median as its centre. */
#define FT9201_RIDGE_PERIOD 10.7f

/* On a sensor of this size, the presses occur within a few pixels of each
 * other. A larger search area gives only more cost and a higher risk of an
 * incorrect match. */
#define FT9201_MATCH_MAX_SHIFT 12
#define FT9201_MATCH_SHIFT_STEP 2
#define FT9201_MATCH_MAX_ANGLE 9
#define FT9201_MATCH_ANGLE_STEP 3
#define FT9201_MATCH_N_ANGLES \
  (2 * (FT9201_MATCH_MAX_ANGLE / FT9201_MATCH_ANGLE_STEP) + 1)

/* The function gives a score to a position only when the two finger masks
 * overlap. The overlap must be more than this fraction of the smaller
 * mask. Without this limit, a small corner with a good correlation can get
 * a high score. */
#define FT9201_MATCH_MIN_OVERLAP 0.35f

/* How closely two stored frames must agree on the winning pose to count as
 * confirming each other. One search step in each dimension. */
#define FT9201_MATCH_POSE_TOL_PX 4
/* A separable box blur that holds the edge values. It gives a local mean
 * at a low cost. */
static void
ft9201_box (const gfloat *src, gfloat *dst, gfloat *scratch,
            gint w, gint h, gint r)
{
  for (gint y = 0; y < h; y++)
    {
      gdouble run = 0;
      for (gint i = -r; i <= r; i++)
        run += src[y * w + CLAMP (i, 0, w - 1)];
      for (gint x = 0; x < w; x++)
        {
          scratch[y * w + x] = run / (2 * r + 1);
          run += src[y * w + CLAMP (x + r + 1, 0, w - 1)]
                 - src[y * w + CLAMP (x - r, 0, w - 1)];
        }
    }
  for (gint x = 0; x < w; x++)
    {
      gdouble run = 0;
      for (gint i = -r; i <= r; i++)
        run += scratch[CLAMP (i, 0, h - 1) * w + x];
      for (gint y = 0; y < h; y++)
        {
          dst[y * w + x] = run / (2 * r + 1);
          run += scratch[CLAMP (y + r + 1, 0, h - 1) * w + x]
                 - scratch[CLAMP (y - r, 0, h - 1) * w + x];
        }
    }
}

/* A separable Gaussian blur that holds the edge values. */
static void
ft9201_gauss (const gfloat *src, gfloat *dst, gfloat *scratch,
              gint w, gint h, gfloat sigma)
{
  gint r = MIN ((gint) (3.0f * sigma + 0.5f), 24);
  gfloat k[2 * 24 + 1];
  gfloat sum = 0.0f;

  if (r < 1)
    {
      memcpy (dst, src, sizeof (gfloat) * w * h);
      return;
    }
  for (gint i = -r; i <= r; i++)
    {
      k[i + r] = expf (-(gfloat) (i * i) / (2.0f * sigma * sigma));
      sum += k[i + r];
    }
  for (gint i = 0; i <= 2 * r; i++)
    k[i] /= sum;

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gfloat a = 0.0f;
        for (gint i = -r; i <= r; i++)
          a += k[i + r] * src[y * w + CLAMP (x + i, 0, w - 1)];
        scratch[y * w + x] = a;
      }
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gfloat a = 0.0f;
        for (gint i = -r; i <= r; i++)
          a += k[i + r] * scratch[CLAMP (y + i, 0, h - 1) * w + x];
        dst[y * w + x] = a;
      }
}

/*
 * Makes the local contrast equal in the full frame. The function subtracts
 * the local mean, and then divides by the local standard deviation. This
 * removes the gradient of the pressure and of the illumination. Without
 * this step the gradient controls the result. A strong press and a light
 * press of one finger then differ more than two different fingers with an
 * equal press.
 */
static void
ft9201_normalize_local (gfloat *img, gfloat *s1, gfloat *s2, gfloat *s3,
                        gint w, gint h)
{
  gint r = (gint) (FT9201_RIDGE_PERIOD * 1.5f);
  gsize n = (gsize) w * h;

  ft9201_box (img, s1, s3, w, h, r);
  for (gsize i = 0; i < n; i++)
    s2[i] = (img[i] - s1[i]) * (img[i] - s1[i]);
  ft9201_box (s2, s2, s3, w, h, r);
  for (gsize i = 0; i < n; i++)
    img[i] = (img[i] - s1[i]) / sqrtf (MAX (s2[i], 1.0f));
}

/****** KEYPOINT FEATURES ******/

/*
 * Keypoints and descriptors, and then a check of the geometry.
 *
 * A correlation of the full frames was the first method, and it is not
 * sufficient here. A measurement on live presses gave scores to 0.52 for a
 * different finger, and scores as low as 0.33 for the correct finger.
 * Thus the two groups of scores overlap, and no threshold is safe. The
 * cause is the small area. A press on a square of 4.5 mm gives a different
 * part of the skin each time, and the two parts overlap only partially. A correlation
 * of the full frames cannot correct this.
 *
 * The method below is the usual solution for this problem. The vendor
 * software of the sensor also uses it. Their engine exports
 * FtScaleSpaceExtrema, FtComputeDescriptors, FtHistToDescr, FtRansacNew
 * and FtEstimateRotParms. These names show scale-space keypoints with
 * descriptors of the SIFT type, a match operation, and then a check with
 * RANSAC. This driver uses the same method, but no part of their code.
 * Nobody read their code line by line or made a copy of it. The code below
 * is the usual formulation from the literature.
 *
 * The driver makes the method more simple in one condition: it uses no
 * scale space with more than one octave. The resolution of the sensor is
 * constant, and the measured ridge period is 10.7 pixels. Thus an
 * independence from the scale gives no advantage. It gives only more code
 * and more incorrect matches. The keypoints come from one
 * difference-of-Gaussians for that period.
 */

/* The driver makes the frame two times larger before the detection. The
 * function FtAdjustForImgDbl of the vendor does the same. At 96x96 there
 * are too few pixels for each ridge, and the keypoints are not stable. */
#define FT9201_KP_UPSCALE 2
#define FT9201_KP_MAX 256
/* 4x4 bins for the position and 8 bins for the direction, as in the usual
 * descriptor. */
#define FT9201_DESC_BINS 4
#define FT9201_DESC_ORIS 8
#define FT9201_DESC_LEN (FT9201_DESC_BINS * FT9201_DESC_BINS * FT9201_DESC_ORIS)
/* The width of the descriptor window, in pixels of the larger frame. It is
 * approximately two ridge periods. */
#define FT9201_DESC_RADIUS ((gint) (FT9201_RIDGE_PERIOD * FT9201_KP_UPSCALE))
/* A keypoint is valid only when its |DoG| answer is more than this value,
 * after the normalisation. */
#define FT9201_KP_MIN_CONTRAST 0.04f

typedef struct
{
  gfloat x, y;
  gfloat ori;
  gfloat desc[FT9201_DESC_LEN];
} Ft9201Keypoint;

typedef struct
{
  Ft9201Keypoint *kp;
  guint           n;
} Ft9201Features;

static void
ft9201_features_free (Ft9201Features *f)
{
  if (f == NULL)
    return;
  g_free (f->kp);
  g_free (f);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Ft9201Features, ft9201_features_free)

/* Makes the frame two times larger with the nearest neighbour method. The
 * Gaussian blur that follows makes the result smooth. */
static void
ft9201_upscale (const gfloat *src, gfloat *dst, gint w, gint h)
{
  gint dw = w * FT9201_KP_UPSCALE;

  for (gint y = 0; y < h * FT9201_KP_UPSCALE; y++)
    for (gint x = 0; x < dw; x++)
      dst[y * dw + x] = src[(y / FT9201_KP_UPSCALE) * w + (x / FT9201_KP_UPSCALE)];
}

/* Calculates the magnitude and the direction of the gradient of the work
 * image. */
static void
ft9201_gradients (const gfloat *img, gfloat *mag, gfloat *ori, gint w, gint h)
{
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gfloat gx = img[y * w + MIN (x + 1, w - 1)] - img[y * w + MAX (x - 1, 0)];
        gfloat gy = img[MIN (y + 1, h - 1) * w + x] - img[MAX (y - 1, 0) * w + x];

        mag[y * w + x] = sqrtf (gx * gx + gy * gy);
        ori[y * w + x] = atan2f (gy, gx);
      }
}

/*
 * A descriptor that stays equal when the finger turns. It is a grid of 4x4
 * histograms of the gradient direction, and each histogram has 8 bins. The
 * function takes the samples in the coordinates of the keypoint. Thus a
 * turn of the finger turns the sample positions, and not the descriptor.
 */
static void
ft9201_describe (Ft9201Keypoint *k, const gfloat *mag, const gfloat *ori,
                 gint w, gint h)
{
  gint r = FT9201_DESC_RADIUS;
  gfloat ca = cosf (k->ori), sa = sinf (k->ori);
  gfloat norm = 0.0f;

  memset (k->desc, 0, sizeof k->desc);

  for (gint dy = -r; dy <= r; dy++)
    for (gint dx = -r; dx <= r; dx++)
      {
        gint px = (gint) (k->x + dx), py = (gint) (k->y + dy);
        gfloat rx, ry, bx, by, a;
        gint bxi, byi, obin;

        if (px < 1 || py < 1 || px >= w - 1 || py >= h - 1)
          continue;

        /* turns the sample offset into the coordinates of the keypoint */
        rx = (dx * ca + dy * sa) / r;      /* -1 .. 1 */
        ry = (-dx * sa + dy * ca) / r;
        if (rx <= -1.0f || rx >= 1.0f || ry <= -1.0f || ry >= 1.0f)
          continue;

        bx = (rx + 1.0f) * 0.5f * FT9201_DESC_BINS;
        by = (ry + 1.0f) * 0.5f * FT9201_DESC_BINS;
        bxi = CLAMP ((gint) bx, 0, FT9201_DESC_BINS - 1);
        byi = CLAMP ((gint) by, 0, FT9201_DESC_BINS - 1);

        /* the gradient direction, also in relation to the keypoint */
        a = ori[py * w + px] - k->ori;
        while (a < 0)
          a += 2.0f * (gfloat) G_PI;
        while (a >= 2.0f * (gfloat) G_PI)
          a -= 2.0f * (gfloat) G_PI;
        obin = (gint) (a / (2.0f * (gfloat) G_PI) * FT9201_DESC_ORIS) % FT9201_DESC_ORIS;

        k->desc[(byi * FT9201_DESC_BINS + bxi) * FT9201_DESC_ORIS + obin]
          += mag[py * w + px];
      }

  /* Makes the length equal to 1, then limits the large values and makes
   * the length equal to 1 again. This is the usual protection against a
   * small number of large gradients that control the descriptor. */
  for (gint i = 0; i < FT9201_DESC_LEN; i++)
    norm += k->desc[i] * k->desc[i];
  norm = sqrtf (norm);
  if (norm < 1e-6f)
    return;
  for (gint i = 0; i < FT9201_DESC_LEN; i++)
    k->desc[i] = MIN (k->desc[i] / norm, 0.2f);
  norm = 0.0f;
  for (gint i = 0; i < FT9201_DESC_LEN; i++)
    norm += k->desc[i] * k->desc[i];
  norm = sqrtf (norm);
  if (norm > 1e-6f)
    for (gint i = 0; i < FT9201_DESC_LEN; i++)
      k->desc[i] /= norm;
}

static Ft9201Features *
ft9201_features_new (const guint8 *img, gint w, gint h)
{
  Ft9201Features *f = g_new0 (Ft9201Features, 1);
  gint uw = w * FT9201_KP_UPSCALE, uh = h * FT9201_KP_UPSCALE;
  gsize n = (gsize) w * h, un = (gsize) uw * uh;
  g_autofree gfloat *in = g_new (gfloat, n);
  g_autofree gfloat *s1 = g_new (gfloat, n);
  g_autofree gfloat *s2 = g_new (gfloat, n);
  g_autofree gfloat *s3 = g_new (gfloat, n);
  g_autofree gfloat *up = g_new (gfloat, un);
  g_autofree gfloat *g1 = g_new (gfloat, un);
  g_autofree gfloat *g2 = g_new (gfloat, un);
  g_autofree gfloat *dog = g_new (gfloat, un);
  g_autofree gfloat *mag = g_new (gfloat, un);
  g_autofree gfloat *ori = g_new (gfloat, un);
  g_autofree gfloat *scratch = g_new (gfloat, un);
  gfloat period = FT9201_RIDGE_PERIOD * FT9201_KP_UPSCALE;

  f->kp = g_new0 (Ft9201Keypoint, FT9201_KP_MAX);
  f->n = 0;

  for (gsize i = 0; i < n; i++)
    in[i] = img[i];
  ft9201_normalize_local (in, s1, s2, s3, w, h);
  ft9201_upscale (in, up, w, h);

  /* the difference of two Gaussians on the two sides of the ridge period */
  ft9201_gauss (up, g1, scratch, uw, uh, period / 5.0f);
  ft9201_gauss (up, g2, scratch, uw, uh, period / 3.2f);
  for (gsize i = 0; i < un; i++)
    dog[i] = g1[i] - g2[i];

  ft9201_gradients (g1, mag, ori, uw, uh);

  /*
   * The keypoints are the local minimum and maximum values of the DoG
   * answer. On a fingerprint they occur at the ends of the ridges, at the
   * points where a ridge divides, and at the pores. A minutiae extractor
   * uses the same positions, but it needs ten of them for bozorth3. This
   * method has no such limit.
   */
  {
    gint step = MAX ((gint) (period / 4.0f), 2);
    gint border = FT9201_DESC_RADIUS + 2;

    for (gint y = border; y < uh - border && f->n < FT9201_KP_MAX; y++)
      for (gint x = border; x < uw - border && f->n < FT9201_KP_MAX; x++)
        {
          gfloat v = dog[y * uw + x];
          gboolean is_max = TRUE, is_min = TRUE;

          if (ABS (v) < FT9201_KP_MIN_CONTRAST)
            continue;
          for (gint dy = -step; dy <= step && (is_max || is_min); dy++)
            for (gint dx = -step; dx <= step; dx++)
              {
                gfloat o;

                if (dx == 0 && dy == 0)
                  continue;
                o = dog[(y + dy) * uw + (x + dx)];
                if (o >= v)
                  is_max = FALSE;
                if (o <= v)
                  is_min = FALSE;
              }
          if (!is_max && !is_min)
            continue;

          Ft9201Keypoint *k = &f->kp[f->n];
          gfloat hist[36] = { 0 };
          gint best = 0;

          k->x = x;
          k->y = y;

          /* the primary gradient direction in a ring around the keypoint */
          for (gint dy = -FT9201_DESC_RADIUS; dy <= FT9201_DESC_RADIUS; dy++)
            for (gint dx = -FT9201_DESC_RADIUS; dx <= FT9201_DESC_RADIUS; dx++)
              {
                gint px = x + dx, py = y + dy;
                gfloat a;
                gint b;

                if (px < 0 || py < 0 || px >= uw || py >= uh)
                  continue;
                a = ori[py * uw + px];
                if (a < 0)
                  a += 2.0f * (gfloat) G_PI;
                b = (gint) (a / (2.0f * (gfloat) G_PI) * 36) % 36;
                hist[b] += mag[py * uw + px];
              }
          for (gint b = 1; b < 36; b++)
            if (hist[b] > hist[best])
              best = b;
          k->ori = (best + 0.5f) / 36.0f * 2.0f * (gfloat) G_PI;

          ft9201_describe (k, mag, ori, uw, uh);
          f->n++;
        }
  }

  return f;
}

/****** MATCHING ******/

/* The square of the Euclidean distance between two descriptors. */
static gfloat
ft9201_desc_dist2 (const Ft9201Keypoint *a, const Ft9201Keypoint *b)
{
  gfloat d = 0.0f;

  for (gint i = 0; i < FT9201_DESC_LEN; i++)
    {
      gfloat t = a->desc[i] - b->desc[i];
      d += t * t;
    }
  return d;
}

/* The ratio test of Lowe. A pair of keypoints is valid only when the best
 * descriptor match is much better than the second best. This prevents pairs
 * at all positions, because the ridge texture repeats. */
#define FT9201_MATCH_RATIO 0.8f

/* RANSAC with a model of one turn and one movement. Two pairs of keypoints
 * define such a model. Thus each hypothesis has a low cost, and the driver
 * can test all combinations of two pairs. */
#define FT9201_RANSAC_TOL_PX (3.0f * FT9201_KP_UPSCALE)
#define FT9201_RANSAC_MAX_PAIRS 400

/*
 * Calculates the score of one new frame against one stored frame. The
 * function matches the descriptors, and then finds the largest group of
 * pairs that agree on one motion. The motion has one turn and one
 * movement. The score is the size of that group in relation to the number
 * of available pairs. Thus a frame with more keypoints does not get a
 * higher score for that reason.
 */
static gfloat
ft9201_match_pair (const Ft9201Features *a, const Ft9201Features *b)
{
  guint pairs = 0;
  guint16 ai[FT9201_KP_MAX], bi[FT9201_KP_MAX];
  gint best_inliers = 0;

  if (a->n < 4 || b->n < 4)
    return 0.0f;

  for (guint i = 0; i < a->n && pairs < FT9201_KP_MAX; i++)
    {
      gfloat d1 = G_MAXFLOAT, d2 = G_MAXFLOAT;
      guint bestj = 0;

      for (guint j = 0; j < b->n; j++)
        {
          gfloat d = ft9201_desc_dist2 (&a->kp[i], &b->kp[j]);

          if (d < d1)
            {
              d2 = d1;
              d1 = d;
              bestj = j;
            }
          else if (d < d2)
            {
              d2 = d;
            }
        }
      if (d2 > 0.0f && d1 < FT9201_MATCH_RATIO * FT9201_MATCH_RATIO * d2)
        {
          ai[pairs] = i;
          bi[pairs] = bestj;
          pairs++;
        }
    }

  if (pairs < 3)
    return 0.0f;

  /* each two pairs of keypoints give one motion */
  for (guint p = 0; p < pairs; p++)
    for (guint q = p + 1; q < pairs; q++)
      {
        const Ft9201Keypoint *a1 = &a->kp[ai[p]], *a2 = &a->kp[ai[q]];
        const Ft9201Keypoint *b1 = &b->kp[bi[p]], *b2 = &b->kp[bi[q]];
        gfloat adx = a2->x - a1->x, ady = a2->y - a1->y;
        gfloat bdx = b2->x - b1->x, bdy = b2->y - b1->y;
        gfloat alen = sqrtf (adx * adx + ady * ady);
        gfloat blen = sqrtf (bdx * bdx + bdy * bdy);
        gfloat ang, ca, sa, tx, ty;
        gint inliers = 0;

        if (alen < 4.0f || blen < 4.0f)
          continue;
        /* a motion of this type keeps the distance; if not, discard the pair */
        if (ABS (alen - blen) > FT9201_RANSAC_TOL_PX)
          continue;

        ang = atan2f (ady, adx) - atan2f (bdy, bdx);
        ca = cosf (ang);
        sa = sinf (ang);
        tx = a1->x - (b1->x * ca - b1->y * sa);
        ty = a1->y - (b1->x * sa + b1->y * ca);

        for (guint r = 0; r < pairs; r++)
          {
            const Ft9201Keypoint *ka = &a->kp[ai[r]], *kb = &b->kp[bi[r]];
            gfloat px = kb->x * ca - kb->y * sa + tx;
            gfloat py = kb->x * sa + kb->y * ca + ty;
            gfloat ex = px - ka->x, ey = py - ka->y;

            if (ex * ex + ey * ey <= FT9201_RANSAC_TOL_PX * FT9201_RANSAC_TOL_PX)
              inliers++;
          }
        if (inliers > best_inliers)
          best_inliers = inliers;
      }

  /* The function divides by the number of keypoints of the smaller side.
   * Thus the score of a full press and the score of a partial press are
   * comparable. */
  return (gfloat) best_inliers / (gfloat) MIN (a->n, b->n);
}

/*
 * Calculates the best score of one new frame against a full template. The
 * template is the group of frames from the enrolment. The function
 * calculates the keypoints of the new frame one time and then uses them
 * again, because they are the largest part of the cost.
 */
static gfloat
ft9201_match_template (GPtrArray *template_frames, const guint8 *probe,
                       gint w, gint h)
{
  g_autoptr(Ft9201Features) pf = ft9201_features_new (probe, w, h);
  gfloat first = -1.0f, second = -1.0f;
  guint scored = 0;

  for (guint t = 0; t < template_frames->len; t++)
    {
      GBytes *b = g_ptr_array_index (template_frames, t);
      gsize len = 0;
      const guint8 *data = g_bytes_get_data (b, &len);
      g_autoptr(Ft9201Features) gf = NULL;
      gfloat s;

      if (len != (gsize) w * h)
        continue;
      gf = ft9201_features_new (data, w, h);
      s = ft9201_match_pair (gf, pf);
      scored++;

      if (s > first)
        {
          second = first;
          first = s;
        }
      else if (s > second)
        {
          second = s;
        }
    }

  if (scored == 0)
    return -1.0f;
  if (scored == 1)
    return first;

  /* The function takes the second best score and not the best score. Each
   * stored frame gives an accidental match one more opportunity, and a
   * template holds FT9201_ENROLL_STAGES frames. The correct finger matches
   * more than one frame. */
  return second;
}

/****** TEMPLATE STORAGE ******/

/*
 * A template is the group of frames from the enrolment. The driver keeps
 * the frames in the data of the FpPrint, on the host. This sensor has no
 * storage, and the driver does not make a small feature vector from a
 * frame. At 96x96 one frame has 9 KB, thus one finger needs approximately
 * 74 KB. The driver keeps the frames for one more reason: a better match
 * algorithm can then use the existing enrolments.
 */
#define FT9201_TEMPLATE_FORMAT "(yy@ay)"

static GVariant *
ft9201_template_pack (GPtrArray *frames, guint8 w, guint8 h)
{
  g_autofree guint8 *blob = NULL;
  gsize frame_size = (gsize) w * h;
  gsize total = frame_size * frames->len;
  gsize off = 0;

  blob = g_malloc (total);
  for (guint i = 0; i < frames->len; i++)
    {
      GBytes *b = g_ptr_array_index (frames, i);
      gsize len = 0;
      const guint8 *data = g_bytes_get_data (b, &len);

      if (len != frame_size)
        continue;
      memcpy (blob + off, data, frame_size);
      off += frame_size;
    }

  return g_variant_new (FT9201_TEMPLATE_FORMAT, w, h,
                        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                   blob, off, 1));
}

/* Gives a GPtrArray of GBytes, or NULL and an error. The function
 * examines each field, because this data comes from the disk and can hold
 * any value. */
static GPtrArray *
ft9201_template_unpack (FpPrint *print, guint8 *out_w, guint8 *out_h,
                        GError **error)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) blob_var = NULL;
  GPtrArray *frames = NULL;
  const guint8 *blob = NULL;
  gsize blob_len = 0, frame_size;
  guint8 w = 0, h = 0;

  g_object_get (print, "fpi-data", &data, NULL);
  if (data == NULL ||
      !g_variant_check_format_string (data, FT9201_TEMPLATE_FORMAT, FALSE))
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                   "stored print is not in this driver's format"));
      return NULL;
    }

  g_variant_get (data, FT9201_TEMPLATE_FORMAT, &w, &h, &blob_var);
  blob = g_variant_get_fixed_array (blob_var, &blob_len, 1);
  frame_size = (gsize) w * h;

  if (w == 0 || h == 0 || frame_size == 0 || blob_len == 0 ||
      blob_len % frame_size != 0)
    {
      g_propagate_error (error,
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                   "stored print is malformed: %ux%u with %"
                                                   G_GSIZE_FORMAT " bytes of frame data",
                                                   w, h, blob_len));
      return NULL;
    }

  frames = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  for (gsize off = 0; off + frame_size <= blob_len; off += frame_size)
    g_ptr_array_add (frames, g_bytes_new (blob + off, frame_size));

  *out_w = w;
  *out_h = h;
  return frames;
}

/*
 * The threshold for a match. It applies to the fraction of agreeing pairs
 * from ft9201_match_pair().
 *
 * A measurement used live presses of two different fingers of one person.
 * The 14 presses gave 14 comparisons with the correct finger and 14
 * comparisons with the other finger:
 *
 *   the other finger, all 14 comparisons      0.000 to 0.036
 *   the correct finger, good presses          0.079 to 0.271
 *   the correct finger, weak or partial       0.024 to 0.037
 *
 * Thus the value 0.06 is in a true space between the two groups. No
 * comparison with the other finger came nearer than a factor of 1.6. Each
 * good press of the correct finger was more than the threshold by 30 % or
 * more. The measured error rates are 0 incorrect accept operations in 14,
 * and approximately 36 % incorrect reject operations. The user must press
 * a second time after a weak press, which is the correct behaviour for an
 * authentication path.
 *
 * The limits of this measurement: 14 comparisons with one pair of fingers
 * of one person cannot give a rate of incorrect accept operations. For 0
 * of 14, the upper limit at 95 % confidence is still near 20 %. Thus this
 * threshold shows a clear difference between two fingers, but it is not a
 * measurement of security. Do the test again with more fingers and more
 * persons before you use this sensor as the only authentication factor.
 *
 * If the sensor accepts an incorrect finger, increase this value and write
 * the change in the README.
 */
#define FT9201_MATCH_THRESHOLD 0.06f

/*
 * The limits of the threshold. A value outside these limits is a fault,
 * and the driver then uses FT9201_MATCH_THRESHOLD. The lower limit keeps
 * a threshold that no measured impostor score has reached.
 */
#define FT9201_MATCH_THRESHOLD_MIN 0.01f
#define FT9201_MATCH_THRESHOLD_MAX 1.00f

/* The name of the variable that changes the threshold for a measurement. */
#define FT9201_MATCH_THRESHOLD_ENV "FT9201_MATCH_THRESHOLD"

/*
 * Gives the threshold that the verify and identify operations use.
 *
 * The threshold is a security parameter. A measurement of the error rates
 * must change it many times, and a rebuild for each value is expensive.
 * Thus the variable FT9201_MATCH_THRESHOLD can change it at run time.
 *
 * The driver treats the content of that variable as unknown data. It
 * accepts the value only when the full text is one number, and when that
 * number is inside the limits above. Each other content is a fault, and
 * the driver then keeps the compiled default.
 *
 * The driver writes a warning for each accepted value. An operator must
 * see in the log that the authentication does not use the default.
 */
static gfloat
ft9201_match_threshold (void)
{
  static gfloat value = FT9201_MATCH_THRESHOLD;
  static gboolean known = FALSE;
  const gchar *text;
  gchar *end = NULL;
  gdouble parsed;

  if (known)
    return value;

  known = TRUE;

  text = g_getenv (FT9201_MATCH_THRESHOLD_ENV);
  if (text == NULL || *text == '\0')
    return value;

  errno = 0;
  parsed = g_ascii_strtod (text, &end);

  /* g_ascii_strtod ignores a space before the number. The driver does not,
   * because it accepts one number and no other character. */
  if (g_ascii_isspace (text[0]) ||
      errno != 0 || end == NULL || *end != '\0' || !isfinite (parsed) ||
      parsed < (gdouble) FT9201_MATCH_THRESHOLD_MIN ||
      parsed > (gdouble) FT9201_MATCH_THRESHOLD_MAX)
    {
      fp_warn ("FT9201 ignores %s: the value is not a number between "
               "%.2f and %.2f. The driver uses the threshold %.2f.",
               FT9201_MATCH_THRESHOLD_ENV,
               (gdouble) FT9201_MATCH_THRESHOLD_MIN,
               (gdouble) FT9201_MATCH_THRESHOLD_MAX,
               (gdouble) value);
      return value;
    }

  value = (gfloat) parsed;

  if (value < FT9201_MATCH_THRESHOLD)
    {
      fp_warn ("FT9201 uses the match threshold %.3f from %s. The value is "
               "less than the default %.2f, thus it accepts a different "
               "finger more often. Use it for a measurement only.",
               (gdouble) value, FT9201_MATCH_THRESHOLD_ENV,
               (gdouble) FT9201_MATCH_THRESHOLD);
    }
  else
    {
      fp_warn ("FT9201 uses the match threshold %.3f from %s. The default "
               "is %.2f.", (gdouble) value, FT9201_MATCH_THRESHOLD_ENV,
               (gdouble) FT9201_MATCH_THRESHOLD);
    }

  return value;
}

/****** CAPTURE ******/

/*
 * The wait for a finger polls only the request 0x43. The vendor driver does
 * the same. In the Windows capture its wait loop sends the request 0x43
 * each 80 ms. It does not read the register 0x20.
 *
 * An earlier version of this loop first tested the ready state of the MCU.
 * That test was an invention of this driver, and it stopped the detection.
 * Between two presses the AFE removes its own power, and the register 0x20
 * stays at 01 01. Thus the test never permitted the poll, and the driver
 * never sent the request 0x43. A measurement compared this loop with a
 * simple poll program. The simple program saw four presses in thirteen
 * seconds. The loop with the test saw none.
 */
/*
 * The registers 0x1f and 0x1e start the scan for a finger. The vendor
 * calls this the automatic sensor mode. Their SPI code has the same write
 * in focal_fp_sensor_set_autosensormode_flag. A test on this hardware
 * applied the arm writes one at a time. The detection started exactly at
 * the write of the register 0x1e, and at no earlier write.
 *
 * The driver writes these two registers again after each frame, and also
 * after each 1000 polls that report no finger. The vendor driver does the
 * same. Refer to FT9201_KEEPALIVE_POLLS for the measurement.
 *
 * An earlier version of this comment said that the vendor driver does not
 * renew the mode during a wait. That conclusion came from a capture of 26
 * seconds. That capture is shorter than one renewal interval, thus it
 * contains no renewal. A capture of 14 minutes shows the renewals clearly.
 */
enum capture_states {
  CAP_POLL_FINGER,
  CAP_POLL_FINGER_CHECK,
  CAP_READ_READY,
  CAP_CHECK_READY,
  CAP_READ_PRESENT,
  CAP_CHECK_PRESENT,
  CAP_MODE_RESET,
  CAP_MODE_RESET_DELAY,
  CAP_MODE_IN,
  CAP_SET_TRANSFER,
  CAP_READ_IMAGE,
  CAP_REARM_READ,
  CAP_REARM_1,
  CAP_REARM_2,
  CAP_REARM_DELAY,
  CAP_FRAME_DONE,
  /* These states renew the automatic sensor mode during a long wait. The
   * poll loop goes to them after each FT9201_KEEPALIVE_POLLS polls. They
   * then continue through the CAP_REARM_* states, which do the same
   * renewal as after a frame. */
  CAP_KEEPALIVE_WAKE_1,
  CAP_KEEPALIVE_WAKE_1_DELAY,
  CAP_KEEPALIVE_WAKE_2,
  CAP_KEEPALIVE_SETTLE,
  /* The three reads that the vendor driver does after an arm operation.
   * The last read is the only known method to see if the sensor still
   * looks for a finger, when no finger touches the sensor. */
  CAP_KEEPALIVE_VERIFY_STATUS,
  CAP_KEEPALIVE_VERIFY_PRESENT,
  CAP_KEEPALIVE_VERIFY_RESUMED,
  CAP_KEEPALIVE_VERIFY_CHECK,
  CAP_NUM_STATES,
};

static void
ft9201_frame_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                      gpointer user_data, GError *error)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);
  gsize img_size = (gsize) self->sensor_width * self->sensor_height;
  gsize expected = img_size + FT9201_IMG_HEADER_SIZE;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if ((gsize) transfer->actual_length != expected)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Short frame read: got %" G_GSIZE_FORMAT
                                                     " bytes, expected %" G_GSIZE_FORMAT,
                                                     (gsize) transfer->actual_length,
                                                     expected));
      return;
    }

  /* Each true frame starts with the bytes "dd dd". A different start shows
   * that the MCU gave its status bytes, because the driver asked for an
   * image during the busy state. The data after such a start is a field of
   * 0x00 and 0x01 bytes, and not an image. */
  if (transfer->buffer[0] != FT9201_IMG_HEADER_MAGIC ||
      transfer->buffer[1] != FT9201_IMG_HEADER_MAGIC)
    {
      fp_dbg ("FT9201 discarding frame with header %02x %02x",
              transfer->buffer[0], transfer->buffer[1]);
      /* This is not an error. The driver waits again for a true frame. */
      fpi_ssm_jump_to_state (transfer->ssm, CAP_REARM_READ);
      return;
    }

  g_free (self->frame);
  self->frame = g_memdup2 (transfer->buffer + FT9201_IMG_HEADER_SIZE, img_size);

  /* A frame with one value in all pixels holds no ridge data. It is an
   * incorrect read, and not a fingerprint. */
  {
    gboolean flat = TRUE;

    for (gsize i = 1; i < img_size; i++)
      if (self->frame[i] != self->frame[0])
        {
          flat = FALSE;
          break;
        }
    if (flat)
      {
        fp_dbg ("FT9201 discarding flat frame (all 0x%02x)", self->frame[0]);
        g_clear_pointer (&self->frame, g_free);
        fpi_ssm_jump_to_state (transfer->ssm, CAP_REARM_READ);
        return;
      }
  }

  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAP_POLL_FINGER:
      /* This vendor request reports a finger. The driver does not poll the
       * register 0x1d for this purpose. */
      ft9201_ctrl_in (ssm, dev, FT9201_REQ_GET_SENSOR_INT_PORT_STATES,
                      0, 0, 1);
      break;

    case CAP_POLL_FINGER_CHECK:
      /* The wait has no time limit. The user can cancel the action, which
       * is the behaviour that libfprint expects. */
      if (self->reg_buf[0] == FT9201_INT_PORT_FINGER_PRESENT)
        {
          self->idle_polls = 0;
          fpi_ssm_next_state (ssm);
        }
      else if (++self->idle_polls >= FT9201_KEEPALIVE_POLLS)
        {
          /* The automatic sensor mode is at its time limit. The driver
           * must renew it, or the sensor stops the report of each finger.
           * Refer to FT9201_KEEPALIVE_POLLS. */
          self->idle_polls = 0;
          fp_dbg ("FT9201 renewing auto-sensor mode after %d idle polls",
                  FT9201_KEEPALIVE_POLLS);
          fpi_ssm_jump_to_state_delayed (ssm, CAP_KEEPALIVE_WAKE_1,
                                         FT9201_POLL_INTERVAL_MS);
        }
      else
        {
          fpi_ssm_jump_to_state_delayed (ssm, CAP_POLL_FINGER,
                                         FT9201_POLL_INTERVAL_MS);
        }
      break;

    case CAP_READ_READY:
      ft9201_read_register (ssm, dev, FT9201_REG_CAPTURE_READY);
      break;

    case CAP_CHECK_READY:
      /*
       * The driver keeps this test, but the test has a known weakness.
       *
       * A press makes the MCU busy. A register read during the busy state
       * gives the status pattern 01 01 and not the register value. Thus
       * this read can give 0x0101 at the moment when the finger arrives,
       * and the driver then discards a true press.
       *
       * A test removed this step for that reason, and the result did not
       * improve. The measurements of 11 correct matches in 11, and 0
       * incorrect accept operations in 18, used the configuration with the
       * test. Therefore the driver keeps the test until a measurement
       * shows a better method.
       *
       * The register 0x1d, which the driver reads next, is the value that
       * identifies a true frame.
       */
      if (self->reg_buf[0] == FT9201_CAPTURE_READY_MAGIC)
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fp_dbg ("FT9201 register 0x%02x reads 0x%02x, not 0x%02x -- press discarded",
                  FT9201_REG_CAPTURE_READY, self->reg_buf[0],
                  FT9201_CAPTURE_READY_MAGIC);
          fpi_ssm_jump_to_state_delayed (ssm, CAP_POLL_FINGER,
                                         FT9201_POLL_INTERVAL_MS);
        }
      break;

    case CAP_READ_PRESENT:
      ft9201_read_register (ssm, dev, FT9201_REG_FINGER_PRESENT);
      break;

    case CAP_CHECK_PRESENT:
      /* Only the value 0xa0 occurs with a true frame. The value 0x01 is
       * the busy pattern of the MCU. One such poll for each capture is
       * usual, immediately after an arm operation. */
      if (self->reg_buf[0] == FT9201_FINGER_PRESENT_A0)
        {
          self->spurious_polls = 0;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
          fpi_ssm_next_state (ssm);
        }
      else if (++self->spurious_polls > FT9201_SPURIOUS_MAX_POLLS)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO,
                                 "a finger was reported %d times without the sensor "
                                 "being ready to image it (register 0x%02x reads 0x%02x, "
                                 "expected 0x%02x)",
                                 FT9201_SPURIOUS_MAX_POLLS,
                                 FT9201_REG_FINGER_PRESENT, self->reg_buf[0],
                                 FT9201_FINGER_PRESENT_A0));
        }
      else
        {
          fpi_ssm_jump_to_state_delayed (ssm, CAP_POLL_FINGER,
                                         FT9201_POLL_INTERVAL_MS);
        }
      break;

    case CAP_MODE_RESET:
      ft9201_ctrl_out (ssm, dev, FT9201_REQ_SET_BULK_MODE,
                       FT9201_BULK_MODE_RESET, 0);
      break;

    case CAP_MODE_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, 15);
      break;

    case CAP_MODE_IN:
      ft9201_ctrl_out (ssm, dev, FT9201_REQ_SET_BULK_MODE,
                       FT9201_BULK_MODE_IN, 0);
      break;

    case CAP_SET_TRANSFER:
      {
        guint16 size = (guint16) ((guint) self->sensor_width *
                                  self->sensor_height +
                                  FT9201_IMG_HEADER_SIZE);

        ft9201_ctrl_out (ssm, dev, FT9201_REQ_SET_BULK_TRANSFER, size, 0x3400);
      }
      break;

    case CAP_READ_IMAGE:
      {
        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        gsize size = (gsize) self->sensor_width * self->sensor_height +
                     FT9201_IMG_HEADER_SIZE;

        transfer->short_is_error = TRUE;
        fpi_usb_transfer_fill_bulk (transfer, FT9201_EP_IMG_IN, size);
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, FT9201_BULK_TIMEOUT_MS,
                                 fpi_device_get_cancellable (dev),
                                 ft9201_frame_read_cb, NULL);
      }
      break;

    case CAP_REARM_READ:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case CAP_REARM_1:
      ft9201_write_register (ssm, dev, FT9201_REG_AUTO_POWER_1, 1);
      break;

    case CAP_REARM_2:
      ft9201_write_register (ssm, dev, FT9201_REG_AUTO_POWER_2, 1);
      break;

    case CAP_REARM_DELAY:
      /* The driver comes to this state after a frame, or from the wait
       * loop that renews the automatic sensor mode.
       *
       * The vendor driver does three reads at this point, after each arm
       * operation. The last read shows that the MCU looks for a finger
       * again. This is the only method to find an arm operation that had
       * no effect. Refer to CAP_KEEPALIVE_VERIFY_CHECK. */
      fpi_ssm_jump_to_state_delayed (ssm, CAP_KEEPALIVE_VERIFY_STATUS, 14);
      break;

    case CAP_FRAME_DONE:
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

      if (self->action == FPI_DEVICE_ACTION_ENROLL)
        {
          gsize img_size = (gsize) self->sensor_width * self->sensor_height;

          g_ptr_array_add (self->enroll_frames,
                           g_bytes_new (self->frame, img_size));
          g_clear_pointer (&self->frame, g_free);

          fpi_device_enroll_progress (dev, self->enroll_frames->len, NULL, NULL);

          if (self->enroll_frames->len < FT9201_ENROLL_STAGES)
            {
              fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
              fpi_ssm_jump_to_state (ssm, CAP_POLL_FINGER);
              return;
            }
        }

      fpi_ssm_mark_completed (ssm);
      break;

    case CAP_KEEPALIVE_WAKE_1:
    case CAP_KEEPALIVE_WAKE_2:
      /* The same wake handshake of two requests that the activation uses,
       * with the same delay between the two requests as in the vendor
       * driver. */
      ft9201_ctrl_out (ssm, dev, FT9201_REQ_PREPARE_MCU_CHECK, 0x0070, 0x0070);
      break;

    case CAP_KEEPALIVE_WAKE_1_DELAY:
      fpi_ssm_next_state_delayed (ssm, 16);
      break;

    case CAP_KEEPALIVE_SETTLE:
      /* The CAP_REARM_* states write the registers 0x1f and 0x1e again.
       * CAP_REARM_DELAY then goes to the verify reads below. */
      fpi_ssm_jump_to_state_delayed (ssm, CAP_REARM_READ, 48);
      break;

    case CAP_KEEPALIVE_VERIFY_STATUS:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case CAP_KEEPALIVE_VERIFY_PRESENT:
      /* The vendor driver reads the register 0x20 and then the register
       * 0x1d at this point. This driver also reads the register 0x1d,
       * because the read can clear the finger latch. The driver does not
       * use the value. */
      ft9201_read_register (ssm, dev, FT9201_REG_FINGER_PRESENT);
      break;

    case CAP_KEEPALIVE_VERIFY_RESUMED:
      ft9201_read_register (ssm, dev, FT9201_REG_MCU_SENSOR_STATUS);
      break;

    case CAP_KEEPALIVE_VERIFY_CHECK:
      /* A sensor that looks for a finger again gives 01 01 at this point,
       * because its MCU is busy with that operation. The capture of the
       * vendor driver shows 01 01 after each of their arm operations.
       *
       * A different value shows that the arm operation had no effect. An
       * idle sensor then reports no press until a new arm operation. This
       * condition is the stall that made this sensor difficult to use.
       * Therefore the driver does the full arm operation again, with the
       * wake handshake, and does not poll a sensor that cannot answer. */
      if (self->reg_buf[0] == FT9201_MCU_STATE_BUSY)
        {
          self->rearm_retries = 0;
          fp_dbg ("FT9201 auto-sensor mode armed, MCU hunting");
        }
      else if (self->rearm_retries < FT9201_REARM_MAX_RETRY)
        {
          self->rearm_retries++;
          fp_dbg ("FT9201 re-arm did not take (register 0x%02x reads "
                  "0x%02x 0x%02x); retrying (%u of %d)",
                  FT9201_REG_MCU_SENSOR_STATUS, self->reg_buf[0],
                  self->reg_buf[1], self->rearm_retries,
                  FT9201_REARM_MAX_RETRY);
          fpi_ssm_jump_to_state (ssm, CAP_KEEPALIVE_WAKE_1);
          return;
        }
      else
        {
          self->rearm_retries = 0;
          fp_warn ("FT9201 sensor will not go back to hunting for a finger "
                   "after %d attempts (register 0x%02x reads 0x%02x 0x%02x, "
                   "expected 0x%02x 0x%02x); presses may go unnoticed until "
                   "the device is power-cycled",
                   FT9201_REARM_MAX_RETRY, FT9201_REG_MCU_SENSOR_STATUS,
                   self->reg_buf[0], self->reg_buf[1],
                   FT9201_MCU_STATE_BUSY, FT9201_MCU_STATE_BUSY);
        }

      /* If the driver holds a frame, an action waits for that frame. */
      if (self->frame != NULL)
        fpi_ssm_jump_to_state_delayed (ssm, CAP_FRAME_DONE, 20);
      else
        fpi_ssm_jump_to_state_delayed (ssm, CAP_POLL_FINGER,
                                       FT9201_POLL_INTERVAL_MS);
      break;
    }
}

/****** ACTION PLUMBING ******/

static void
ft9201_reset_task (FpiDeviceFocaltechFt9201 *self)
{
  self->task_ssm = NULL;
  self->spurious_polls = 0;
  self->idle_polls = 0;
  self->rearm_retries = 0;
  g_clear_pointer (&self->frame, g_free);
  g_clear_pointer (&self->enroll_frames, g_ptr_array_unref);
}

static void
capture_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);
  FpiDeviceAction action = self->action;

  self->task_ssm = NULL;

  if (error)
    {
      ft9201_reset_task (self);
      fpi_device_action_error (dev, error);
      return;
    }

  switch (action)
    {
    case FPI_DEVICE_ACTION_ENROLL:
      {
        FpPrint *print = NULL;

        fpi_device_get_enroll_data (dev, &print);
        fpi_print_set_type (print, FPI_PRINT_RAW);
        fpi_print_set_device_stored (print, FALSE);
        g_object_set (print, "fpi-data",
                      ft9201_template_pack (self->enroll_frames,
                                            self->sensor_width,
                                            self->sensor_height),
                      NULL);
        fp_dbg ("FT9201 enrolled %u frames of %ux%u",
                self->enroll_frames->len, self->sensor_width,
                self->sensor_height);
        ft9201_reset_task (self);
        fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
        break;
      }

    case FPI_DEVICE_ACTION_VERIFY:
      {
        FpPrint *template_print = NULL;
        g_autoptr(GPtrArray) frames = NULL;
        g_autoptr(GError) local = NULL;
        guint8 w = 0, h = 0;
        gfloat score;

        fpi_device_get_verify_data (dev, &template_print);
        frames = ft9201_template_unpack (template_print, &w, &h, &local);
        if (frames == NULL)
          {
            ft9201_reset_task (self);
            fpi_device_verify_complete (dev, g_steal_pointer (&local));
            return;
          }
        if (w != self->sensor_width || h != self->sensor_height)
          {
            ft9201_reset_task (self);
            fpi_device_verify_complete (dev,
                                        fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                                  "print was enrolled at %ux%u but the sensor "
                                                                  "now reports %ux%u -- the firmware revision "
                                                                  "changed, so it has to be re-enrolled",
                                                                  w, h, self->sensor_width,
                                                                  self->sensor_height));
            return;
          }

        score = ft9201_match_template (frames, self->frame, w, h);
        fp_dbg ("FT9201 verify score %.3f (threshold %.2f)",
                (gdouble) score, (gdouble) ft9201_match_threshold ());
        ft9201_reset_task (self);

        fpi_device_verify_report (dev,
                                  score >= ft9201_match_threshold () ?
                                  FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                  NULL, NULL);
        fpi_device_verify_complete (dev, NULL);
        break;
      }

    case FPI_DEVICE_ACTION_IDENTIFY:
      {
        GPtrArray *prints = NULL;
        FpPrint *best_print = NULL;
        gfloat best = -1.0f;

        fpi_device_get_identify_data (dev, &prints);
        for (guint i = 0; prints != NULL && i < prints->len; i++)
          {
            FpPrint *candidate = g_ptr_array_index (prints, i);
            g_autoptr(GPtrArray) frames = NULL;
            guint8 w = 0, h = 0;
            gfloat score;

            frames = ft9201_template_unpack (candidate, &w, &h, NULL);
            if (frames == NULL || w != self->sensor_width ||
                h != self->sensor_height)
              continue;

            score = ft9201_match_template (frames, self->frame, w, h);
            /* The driver writes each candidate to the log, and not only
             * the best one. A comparison of one press against more than
             * one enrolled finger gives the distribution of the scores.
             * The user does not have to remember the finger. */
            fp_dbg ("FT9201 identify candidate %u (%s) score %.3f", i,
                    fp_print_get_finger (candidate) == FP_FINGER_LEFT_INDEX ?
                    "left index" : fp_print_get_finger (candidate) == FP_FINGER_RIGHT_INDEX ?
                    "right index" : "other", (gdouble) score);
            if (score > best)
              {
                best = score;
                best_print = candidate;
              }
          }

        fp_dbg ("FT9201 identify best score %.3f (threshold %.2f)",
                (gdouble) best, (gdouble) ft9201_match_threshold ());
        ft9201_reset_task (self);

        fpi_device_identify_report (dev,
                                    best >= ft9201_match_threshold () ? best_print : NULL,
                                    NULL, NULL);
        fpi_device_identify_complete (dev, NULL);
        break;
      }

    case FPI_DEVICE_ACTION_CAPTURE:
      {
        FpImage *img = fp_image_new (self->sensor_width, self->sensor_height);

        memcpy (img->data, self->frame,
                (gsize) self->sensor_width * self->sensor_height);
        img->ppmm = FT9201_PPMM;
        ft9201_reset_task (self);
        fpi_device_capture_complete (dev, img, NULL);
        break;
      }

    /* The driver starts the capture machine only for the four actions
     * above. The other actions are in the list for one reason: a new
     * action in libfprint then causes a build warning at this point. */
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      ft9201_reset_task (self);
      fpi_device_action_error (dev,
                               fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      break;
    }
}

static void
ft9201_start_capture (FpDevice *dev, FpiDeviceAction action)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  self->action = action;
  self->spurious_polls = 0;
  self->idle_polls = 0;
  self->rearm_retries = 0;
  g_clear_pointer (&self->frame, g_free);

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
  self->task_ssm = fpi_ssm_new (dev, capture_run_state, CAP_NUM_STATES);
  fpi_ssm_start (self->task_ssm, capture_ssm_done);
}

/****** LIBFPRINT DEVICE VFUNCS ******/

static void
activate_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  self->task_ssm = NULL;
  /* The driver needs the firmware image only during the download. A warm
   * open operation does not read the image. */
  g_clear_pointer (&self->fw_data, g_bytes_unref);

  if (error)
    {
      g_autoptr(GError) release_error = NULL;

      g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0,
                                      &release_error);
      fpi_device_open_complete (dev, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0,
                                     &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->retry_count = 0;
  self->fw_step = 0;
  self->fw_reloaded = FALSE;
  self->task_ssm = fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES);
  fpi_ssm_start (self->task_ssm, activate_ssm_done);
}

static void
dev_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (dev);

  self->enroll_frames = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  ft9201_start_capture (dev, FPI_DEVICE_ACTION_ENROLL);
}

static void
dev_verify (FpDevice *dev)
{
  ft9201_start_capture (dev, FPI_DEVICE_ACTION_VERIFY);
}

static void
dev_identify (FpDevice *dev)
{
  ft9201_start_capture (dev, FPI_DEVICE_ACTION_IDENTIFY);
}

static void
dev_capture (FpDevice *dev)
{
  ft9201_start_capture (dev, FPI_DEVICE_ACTION_CAPTURE);
}

static const FpIdEntry id_table[] = {
  { .vid = FT9201_VENDOR_ID, .pid = FT9201_PRODUCT_ID, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_focaltech_ft9201_finalize (GObject *object)
{
  FpiDeviceFocaltechFt9201 *self = FPI_DEVICE_FOCALTECH_FT9201 (object);

  g_clear_pointer (&self->fw_data, g_bytes_unref);
  g_clear_pointer (&self->frame, g_free);
  g_clear_pointer (&self->enroll_frames, g_ptr_array_unref);

  G_OBJECT_CLASS (fpi_device_focaltech_ft9201_parent_class)->finalize (object);
}

static void
fpi_device_focaltech_ft9201_init (FpiDeviceFocaltechFt9201 *self)
{
  /* The value 0xff shows that no read gave the type. A warm start does no
   * download, thus it reads no type. */
  self->sensor_type = FT9201_SENSOR_TYPE_UNKNOWN;
}

static void
fpi_device_focaltech_ft9201_class_init (FpiDeviceFocaltechFt9201Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  object_class->finalize = fpi_device_focaltech_ft9201_finalize;

  dev_class->id = "focaltech_ft9201";
  dev_class->full_name = "FocalTech FT9201 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = FT9201_ENROLL_STAGES;

  /* The match operation occurs on the host and not in the sensor. Thus the
   * driver reports no storage feature. fprintd keeps the templates. */
  dev_class->features = FP_DEVICE_FEATURE_VERIFY |
                        FP_DEVICE_FEATURE_IDENTIFY |
                        FP_DEVICE_FEATURE_CAPTURE;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->capture = dev_capture;
}
