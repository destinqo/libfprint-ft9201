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

#pragma once

#include <glib.h>

/* The USB vendor ID and product ID of the sensor. The tool "lsusb" shows
 * these IDs. The device reports bcdDevice 1.00. */
#define FT9201_VENDOR_ID 0x2808
#define FT9201_PRODUCT_ID 0x93a9

/* The two bulk endpoints. The IN endpoint sends one raw frame for each
 * capture, with a maximum packet size of 32 bytes. The host sends the MCU
 * firmware to the OUT endpoint, which has a maximum packet size of 16
 * bytes. Refer to FT9201_FIRMWARE_PATH. */
#define FT9201_EP_IMG_IN (0x03 | FPI_USB_ENDPOINT_IN)
#define FT9201_EP_FW_OUT (0x02 | FPI_USB_ENDPOINT_OUT)

#define FT9201_CTRL_TIMEOUT_MS 2000
#define FT9201_BULK_TIMEOUT_MS 2000
/* The firmware image has approximately 10 KB. The full-speed OUT endpoint
 * sends 16 bytes in each packet. Thus the download needs much more time
 * than a register write. */
#define FT9201_FW_TIMEOUT_MS 10000

/* The vendor control requests (bRequest).
 *
 * The reverse engineering project supplied the names of the requests
 * 0x1a, 0x22, 0x34, 0x35, 0x3a, 0x3b and 0x43. The other requests occur
 * only in the firmware download. Their names show their known behaviour.
 * Each IN request in this list gives an answer of 4 bytes with this
 * structure: "<value> <bRequest> 00 00". This structure lets you identify
 * a true answer. */
#define FT9201_REQ_UNKNOWN_03 0x03
#define FT9201_REQ_GET_SUI_VERSION 0x1a
#define FT9201_REQ_PREPARE_MCU_CHECK 0x22
#define FT9201_REQ_BULK_CHECK 0x30
#define FT9201_REQ_SET_BULK_MODE 0x34
#define FT9201_REQ_SET_BULK_TRANSFER 0x35
#define FT9201_REQ_READ_REGISTER 0x3a
#define FT9201_REQ_WRITE_REGISTER 0x3b
#define FT9201_REQ_FIRMWARE_COMMIT 0x40
#define FT9201_REQ_GET_SENSOR_INT_PORT_STATES 0x43
#define FT9201_REQ_MCU_SYNC 0x57
#define FT9201_REQ_MCU_READ 0x60
#define FT9201_REQ_MCU_WRITE 0x64
#define FT9201_REQ_SFR_READ 0x65
#define FT9201_REQ_SFR_WRITE 0x66
#define FT9201_REQ_CODE_RAM_READ 0x68
#define FT9201_REQ_CODE_RAM_WRITE 0x69

/* The wValue values for FT9201_REQ_SET_BULK_MODE. This request selects
 * the direction of the next bulk transfer. FT9201_REQ_SET_BULK_TRANSFER
 * then gives the length in wValue and the target address in wIndex. The
 * bulk transfer follows immediately. */
#define FT9201_BULK_MODE_RESET 0x00ff
#define FT9201_BULK_MODE_OUT 0x0002
#define FT9201_BULK_MODE_IN 0x0003

/* The indices of the AFE (analog front end) registers. The driver reads
 * and writes them with FT9201_REQ_READ_REGISTER and
 * FT9201_REQ_WRITE_REGISTER.
 *
 * A register read gives 4 bytes. The first two bytes hold the registers
 * [n] and [n+1], and the last two bytes are zero. A read of the register
 * 0x16 thus gives the full chip ID "95 a8". The driver uses only byte 0,
 * which keeps the meaning of each read clear. */
#define FT9201_REG_UNKNOWN_01 0x01
#define FT9201_REG_SENSOR_WIDTH 0x14
#define FT9201_REG_SENSOR_HEIGHT 0x15
#define FT9201_REG_CHIP_ID_HIGH 0x16
#define FT9201_REG_CHIP_ID_LOW 0x17
#define FT9201_REG_FINGER_PRESENT 0x1d
#define FT9201_REG_AUTO_POWER_2 0x1e
#define FT9201_REG_AUTO_POWER_1 0x1f
#define FT9201_REG_MCU_SENSOR_STATUS 0x20
#define FT9201_REG_SENSOR_CONFIG_22 0x22
#define FT9201_REG_SENSOR_CONFIG_23 0x23
#define FT9201_REG_CAPTURE_READY 0x30
#define FT9201_REG_UNKNOWN_41 0x41

/* Two locations in the SFR space of the 8051 core. The driver reads them
 * through the request FT9201_REQ_SFR_READ during the download.
 *
 * The location 0x00f3 gives the type of the sensor. The type is in the
 * bits 1 to 4: type = (value >> 1) & 0x0f. The project
 * OMGrant/ft9201-libfprint found this, and the type 3 there uses a
 * geometry of 64 x 80.
 *
 * The location 0x00f4 holds a latch. That project reads the location and
 * writes the value again with the bit 0 set. This driver writes the
 * constant 0x0001. The two operations give the same result only when the
 * location reads 0x00. */
#define FT9201_SFR_CHIP_TYPE 0x00f3
#define FT9201_SFR_TYPE_LATCH 0x00f4

/* The driver puts this value in the type when no read gave a type. */
#define FT9201_SENSOR_TYPE_UNKNOWN 0xff

/* The constant values from the USB captures. Tests on the hardware confirm
 * all of them. */

/* The register 0x20 gives "a5 5a" when the MCU is idle, and "01 01" when
 * the MCU is busy. This is more than a status. A read of any register
 * during the busy state gives "01 01" and not the register data.
 * Therefore the driver waits for the idle state before each read that has
 * a meaning. An early read of the sensor dimensions makes a correct 96x96
 * sensor report a size of 1x1. */
#define FT9201_MCU_STATE_MAGIC_A5 0xa5
#define FT9201_MCU_STATE_MAGIC_5A 0x5a
#define FT9201_MCU_STATE_BUSY 0x01

/* The register 0x30 holds 0xbb when the capture engine is armed. The
 * vendor driver writes this value, and the sensor does not set it. A
 * driver that only reads this register sees 0x00 for all time. */
#define FT9201_CAPTURE_READY_MAGIC 0xbb

/* The register 0x1d gives 0xa0 when a finger touches the sensor. The
 * value 0x01 is not a finger. It is the low byte of the busy pattern of
 * the MCU. A driver that accepts 0x01 gets a frame of 0x00 and 0x01 bytes
 * and not an image. */
#define FT9201_FINGER_PRESENT_A0 0xa0

/* FT9201_REQ_GET_SENSOR_INT_PORT_STATES gives one byte. The value is 1
 * when a finger touches the sensor, or 0 when no finger touches it. The
 * vendor driver polls this request and not the register 0x1d. */
#define FT9201_INT_PORT_FINGER_PRESENT 0x01

/* Each image transfer starts with these two bytes. A frame with a
 * different start is not an image. It holds status bytes, because the MCU
 * was busy when the driver asked for an image. */
#define FT9201_IMG_HEADER_SIZE 2
#define FT9201_IMG_HEADER_MAGIC 0xdd

/* The number of pixels for each millimetre. This value comes from the
 * measured wavelength of the ridges. Refer to the .c file for its use.
 * NBIS reads this value when it makes its quality map. libfprint keeps
 * the value at 0 if a driver does not set it. */
#define FT9201_PPMM 19.685

/* The known IDs of the AFE chip. The test unit has the ID 0x95a8. The
 * reverse engineering project gives a full initialisation sequence only
 * for this ID. It found the other two IDs, but not their additional
 * steps. For those two IDs this driver goes directly to the auto-power
 * step, as the original driver does. */
#define FT9201_CHIP_ID_9338 0x9338
#define FT9201_CHIP_ID_9536 0x9536
#define FT9201_CHIP_ID_95A8 0x95a8

/* The MCU firmware.
 *
 * The FT9201 keeps no firmware. Its 8051 core starts with an empty code
 * RAM after each loss of VBUS. Before the host sends an image to
 * FT9201_EP_FW_OUT, each AFE register read gives 00 00 00 00.
 *
 * This fact explains the known fault. Users had to connect the sensor to
 * a Windows machine to make it operate again. The Windows driver did not
 * repair a bad state. It sent the firmware that the Linux driver did not
 * send. A bus reset, a new connection or a power cycle cannot replace the
 * download.
 *
 * The image is the property of the vendor. Therefore this driver does not
 * supply it. Extract the image from the vendor driver or from a USB
 * capture, then install it at the path below. Refer to the README. */
#define FT9201_FIRMWARE_PATH "/lib/firmware/focaltech/ft9201.bin"

/* The download goes to the code RAM offset 0. The driver gives the device
 * the length in units of 64 bytes. The limits below are a check on a file
 * from the disk, and not a limit of the protocol. The known image has
 * 10368 bytes. */
#define FT9201_FW_LOAD_ADDR 0x0000
#define FT9201_FW_CHUNK_SIZE 64
#define FT9201_FW_MIN_SIZE 1024
#define FT9201_FW_MAX_SIZE (64 * 1024)

/* The driver writes a word of 4 bytes to the address 0x85c0 before the
 * download. FT9201_REQ_BULK_CHECK then gives 0x50 0x2b. The function of
 * this step is not known, but the download fails without it. */
#define FT9201_FW_PROBE_ADDR 0x85c0
#define FT9201_FW_PROBE_SIZE 4

/* The maximum number of times the driver does the arm operation again. It
 * does the operation again only when the verify read shows that the MCU
 * does not look for a finger. This count has a limit for two reasons. A
 * sensor that does not come back is a true state, and only a power cycle
 * repairs it. Also, the poll loop must stay ready for a cancel
 * operation. */
#define FT9201_REARM_MAX_RETRY 3

/* The limits for the two loops that wait for the ready state of the MCU. */
#define FT9201_AUTO_POWER_MAX_RETRY 5
#define FT9201_MCU_READY_MAX_POLLS 50

/* The interval between two FT9201_REQ_GET_SENSOR_INT_PORT_STATES
 * requests. The vendor driver uses 80 ms. */
#define FT9201_POLL_INTERVAL_MS 80

/* The automatic finger detection of the sensor has a time limit. The
 * vendor driver renews it after each 1000 polls that report no finger,
 * which is approximately 80 seconds. It renews the mode also when no
 * finger touches the sensor. The renewal is the wake handshake, and then
 * a write of the registers 0x1f and 0x1e. This is the same operation that
 * comes after a frame.
 *
 * A Windows capture of 14 minutes shows 1001 polls between one renewal
 * and the next. This occurred four times in two device sessions. The
 * sensor kept the detection through idle periods of 400 seconds, and
 * detected each press at the first poll after the press.
 *
 * A driver that does not renew the mode loses the finger detection after
 * the first idle period. This gave the sensor the appearance of an
 * accidental failure. An earlier capture of 26 seconds was too short. It
 * contained no renewal, and gave the incorrect conclusion that the vendor
 * driver does not renew the mode.
 *
 * The driver counts polls and not milliseconds, because the vendor driver
 * also counts polls. The measured interval moves between 79.2 s and
 * 80.7 s, but the count stays at 1001 polls. */
#define FT9201_KEEPALIVE_POLLS 1000

/* The number of frames in an enrolment. More frames give a template with
 * more area of the finger, which is important for a small sensor. The
 * costs are the time of the enrolment, and approximately 9 KB of stored
 * data for each frame.
 *
 * A measurement gives the value. The tool tools/ft9201-matcher scored each
 * saved frame against a template of the other frames of the same finger.
 * The rate of the incorrect reject operations falls with the size of the
 * template, and it does not stop:
 *
 * 2 frames rejected 93.3%, 4 frames 86.7%, 6 frames 73.3%, 8 frames 60.0%,
 * 10 frames 46.7%, 12 frames 40.0%, 14 frames 40.0%.
 *
 * Thus 8 frames was too few. The value 15 keeps the enrolment near 40
 * seconds. The vendor also stores more than one part of the finger; its
 * library gives the name MAX_SUBTEMPLATES_PER_FINGER to that limit.
 *
 * A template that an earlier version wrote holds 8 frames. It stays valid.
 * A verify operation reads the frames that the template holds.
 *
 * Keep the frames on the heap. GObject permits an instance of 64 KB, and
 * 15 frames of 96 x 96 are 138240 bytes. A driver that holds them in the
 * instance structure stops with the message
 * "g_type_register_static_simple: assertion 'instance_size <=
 * G_MAXUINT16' failed", which does not name the cause. This driver keeps
 * them in a GPtrArray of GBytes, thus the instance stays small. The
 * project Dgmtnz/ft9201-fingerprint-linux found this limit.
 */
#define FT9201_ENROLL_STAGES 15

/* The driver waits for a finger without a time limit. This is the
 * function of the libfprint state AWAIT_FINGER_ON, and the user can
 * cancel the operation. But the driver puts a limit on a different case:
 * the sensor reports a finger, and the frame behind it is not usable.
 * This occurs when the capture engine loses its armed state, or when the
 * MCU is busy and answers a register read with its status bytes. One such
 * poll after an arm operation is usual. Many such polls show a fault. */
#define FT9201_SPURIOUS_MAX_POLLS 25

G_DECLARE_FINAL_TYPE (FpiDeviceFocaltechFt9201, fpi_device_focaltech_ft9201,
                      FPI, DEVICE_FOCALTECH_FT9201, FpDevice);
