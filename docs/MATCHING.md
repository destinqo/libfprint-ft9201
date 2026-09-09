# The verification of the FT9201

This file tells you how the driver decides that two fingerprints are the
same. The file [`../README.md`](../README.md) tells you how to build and
use the driver, and [`PROTOCOL.md`](PROTOCOL.md) holds the notes about the
hardware.

## How it operates

**The verification operates.** A measurement used two different fingers of
one person:

| | result | score range |
| --- | --- | --- |
| the enrolled finger | **11 of 11 matched** | 0.136 to 0.388 |
| a different finger | **0 of 18 accepted** | 0.000 to 0.045 |

The threshold is 0.06. The enrolment completed 8 stages of 8, and the driver
discarded no frame.

**Read the margin with care.** The first group of presses with the different
finger gave a maximum of 0.036. A later group gave **0.045**. Thus the
distance to 0.06 is only a factor of 1.33 on that side, against 2.3 on the
side of the correct finger. The two groups did not overlap in 29
comparisons. But the maximum of the different finger increased with more
samples, which is the usual behaviour. This is the reason why these numbers
are not a security measurement. The 18 comparisons use one pair of fingers
of one person. They give an upper limit of approximately 15 % on the rate of
incorrect accept operations, at 95 % confidence.

The correct description is "the driver separates two fingers clearly", and
not "the driver is secure". Before you use this sensor as the only
authentication factor, measure it with more fingers and more persons. If the
maximum of the different finger continues to increase, increase
`FT9201_MATCH_THRESHOLD` and write the new numbers in this file.

### The enrolment technique changes the result

A measurement through `fprintd` used the same finger and three enrolment
techniques:

| enrolment technique | matched | scores |
| --- | --- | --- |
| eight quick presses | 3 of 4 | 0.198, 0.124, 0.045, 0.068 |
| the finger moved between presses | 1 of 4 | 0.042, 0.025, 0.110, 0.044 |
| **the same position, firm, 1 s each** | **4 of 4** | 0.278, 0.333, 0.281, 0.244 |

Thus **consistency is better than coverage** for this matcher. A verify
operation compares one press against the template. The image area is only
4.5 mm. Thus a template from eight different parts of the finger gives one
press only one frame for a comparison. Tell the users to press the same
position eight times.

### Why the driver does not use minutiae

The image device path of libfprint uses NBIS bozorth3. That path is closed
for a sensor of this size:

- A 96x96 frame gives **1 or 2 minutiae**. A measurement used 23 frames and
  each combination of `FPI_IMAGE_PARTIAL`, `FPI_IMAGE_COLORS_INVERTED`, a
  `ppmm` from 0 to 25, and a scale of 1x to 4x.
- Image enhancement does not help. Histogram equalisation, tiled
  equalisation of the CLAHE type, unsharp masking and a ridge band-pass
  filter increase the mean only from 1.1 to 1.5. No frame gives 6.
- A combination of frames does not help. The drivers `elan` and `elanspi` do
  this, but consecutive presses give almost the same position. Thus there is
  no new area. The best gain in an enrolment of 18 frames was a factor of
  1.00.
- The frames have a good quality. The maps of NBIS give the highest quality
  to 19 of 144 blocks. A square of 4.5 mm on a fingertip does not contain
  ten minutiae.
- Ten is a hard limit. `bozorth3` gives `ZERO_MATCH_SCORE` and does no
  calculation when one side has fewer than `MIN_COMPUTABLE_BOZORTH_MINUTIAE`
  minutiae. Each score is 0, thus **no `bz3_threshold` can give a match**.
  Therefore the driver is a plain `FpDevice` with its own comparison.

### What the driver does

It uses keypoints, descriptors and a check of the geometry. The vendor
software of the sensor uses the same method. This is not an assumption.
Their Linux library has symbol names. Their Windows engine adapter
(`ftWbioEngineAdapter.dll`) exports `FtBuildGaussPyr`, `FtBuildDogPyr`,
`FtCreateInitImg`, `FtAdjustForImgDbl`, `FtCalcFeatureScales`, `FtDeriv3D`,
`FtHistToDescr`, `FtComputeDescriptors` and `FtCalcBriskFeatureOris`. Their
matcher calls `FtRansacNew`, `FtEstimateRotParms` and `FtHmatrixInv`. These
names give scale-space keypoints with descriptors of the SIFT type and the
BRISK type, a match operation, and a check with RANSAC. This driver uses the
same method, but no vendor code. The steps below are the usual formulation
from the literature.

1. **Local contrast normalisation.** The driver subtracts the local mean and
   divides by the local standard deviation. This removes the pressure
   gradient. Without this step, a strong press and a light press of one
   finger differ more than two different fingers with an equal press.
2. **A frame two times larger**, as the function `FtAdjustForImgDbl` of the
   vendor also makes. At 96x96 there are too few pixels for each ridge, and
   the keypoints are not stable.
3. **Keypoints** from the minimum and maximum values of a difference-of-
   Gaussians for the measured ridge period of 10.7 pixels. The driver uses
   no scale space with more than one octave. The sensor resolution is
   constant, thus an independence from the scale gives no advantage and
   causes more incorrect matches.
4. **Descriptors**: a grid of 4x4 histograms of the gradient direction, each
   with 8 bins. The driver takes the samples in the coordinates of the
   keypoint. Thus a turn of the finger turns the sample positions and not
   the descriptor.
5. **A match operation with the ratio test of Lowe.** This test stops the
   pairs that come from the repeated ridge texture.
6. **RANSAC** on the pairs, with a model of one turn and one movement. The
   score is the number of agreeing pairs in relation to the available
   keypoints. Thus the score of a full press and the score of a partial
   press are comparable.
7. A template is the group of 8 frames from the enrolment. It needs
   approximately 74 KB for each finger, and the driver keeps it in the
   `FpPrint` on the host. The score is the **second best** value of the 8
   frames. The maximum gives an accidental match eight independent
   opportunities.

The driver keeps the raw frames and not a group of features. Thus a better
matcher can use the existing enrolments. This gave an advantage one time
already, when the algorithm changed completely.

### Methods that the tests rejected, with numbers

- **A normalised cross correlation of the full frames.** The scores of a
  different finger reached 0.52, and the scores of the correct finger went
  as low as 0.33. The two groups overlap, thus no threshold is safe. A press
  on 4.5 mm gives a different part of the skin each time, and a correlation
  of the full frames cannot correct this.
- **A Gabor filter before the correlation.** This is the correct method in
  theory, and the function `FtImageGaborU16` of the vendor does it. But on
  live presses the margin between the correct finger and a different finger
  decreased to a factor of 1.3. That code is not in this
  repository. It can help together with the keypoints.
- **A condition that the two best frames of the template agree on the
  position.** This gave scores from 0.07 to 0.33 only, and it decreased the
  margin from a factor of 4 to a factor of 1.3. The condition is true only
  in a few comparisons, thus most scores fell to a penalty value.

### The proprietary alternative

The driver of FocalTech also verifies, and the community guides use it:
[Romk-a/ft9201-linux-setup](https://github.com/Romk-a/ft9201-linux-setup)
for Astra Linux, and
[ryenyuku/libfprint-ft9201](https://github.com/ryenyuku/libfprint-ft9201)
for Arch, also in the
[AUR](https://aur.archlinux.org/packages/libfprint-ft9201). It is not a
TOD plugin, although its name has that appearance. It is a complete
`libfprint` build with their driver in it, and it replaces the system
library. You cannot call their matcher alone. The library exports 94
symbols. 87 of them are the standard `fp_*` API, and not one is a `Ft*`
entry point. Thus there was never a function to use, and the only method is
a new implementation of the algorithm. This driver is that implementation.

An examination of their driver gave two facts. It holds a firmware image,
which is the reason why its users never had the "needs a Windows machine"
fault. And it has one `FpIdEntry` with the PID `0x9338`, which is the
structure that those guides change to `0x93a9`.

The kernel module that gave the protocol
([banianitc](https://github.com/banianitc/ft9201-fingerprint-driver))
never verified a finger. Its README says: *"It won't work with User login
settings as libfprint integration is not done yet"*.


## How to measure it yourself

The tool `tools/ft9201-matcher` measures the matcher of the driver on saved
frames. It needs no hardware. It holds no copy of the matcher: the script
`tools/extract-matcher.sh` takes the code out of the driver, thus the tool
cannot measure an algorithm that the driver no longer uses.

```bash
cd tools
./extract-matcher.sh
gcc -O2 -Wall -Wextra -o ft9201-matcher ft9201-matcher.c \
    $(pkg-config --cflags --libs glib-2.0) -lm
./ft9201-matcher  fingerA/frame*.pgm  fingerB/frame*.pgm
```

Put the frames of one finger in one directory. The name of the parent
directory gives the group. The tool prints four results:

1. the score of each pair,
2. the statistics of the two groups,
3. the error rates against the threshold,
4. the error rate against the number of the frames in the template.

**Collect the frames in the same way that a person uses the sensor.** Put
the finger down the same way each time. Do not change the position on
purpose.

### The threshold at run time

The threshold `FT9201_MATCH_THRESHOLD` has the value 0.06 in the driver.
The variable of the same name changes it for one measurement, thus you do
not build the driver again for each value:

```bash
FT9201_MATCH_THRESHOLD=0.12 fprintd-verify
```

The driver accepts the value only when the full text is one number between
0.01 and 1.00. Each other content is a fault, and the driver then keeps
0.06. The driver writes a warning for each accepted value, and a second
warning when the value is less than the default. **Use it for a measurement
only.** A smaller threshold accepts a different finger more often.

### What the measurement gave

The measurement used the 18 frames of one Windows enrolment session. Of
those frames, 3 are one finger and 15 are a second finger.

| | |
|---|---|
| different finger, largest score | **0.040** |
| same finger, mean score | 0.048 |
| same finger, largest score | 0.401 |
| at the threshold 0.06 | 33 % incorrect reject, 0 of 18 incorrect accept |

The scores of the same finger are **bimodal**. Two frames that share an
area of the skin score 0.10 to 0.40. Two frames that do not share an area
score 0.00 to 0.03, which is the level of a different finger. The small
sensor causes this. It is also the reason for the size of the template.

The rate of the incorrect reject operations falls with the number of the
frames in the template, and it does not stop:

| frames in template | 2 | 4 | 6 | 8 | 10 | 12 | 14 |
|---|---|---|---|---|---|---|---|
| incorrect reject | 93.3 % | 86.7 % | 73.3 % | 60.0 % | 46.7 % | 40.0 % | 40.0 % |

`FT9201_ENROLL_STAGES` was 8 and is now **15** for this reason. That
session changed the position of the finger at each press, thus these rates
are worse than the rates of normal use. The shape of the curve is the
result that matters.

### The measurement on the hardware after the change

A test measured the change from 8 to 15 frames on the sensor. The test made
one enrolment of 15 stages. It then made 4 presses of the enrolled finger,
and 5 presses of a different finger. These are the scores:

| | 8 frames | 15 frames |
|---|---|---|
| correct finger | 0.136 to 0.388 | **0.239 to 0.357** |
| different finger | 0.000 to 0.045 | **0.024 to 0.027** |
| distance between the groups | 3.0 x | **8.9 x** |

The driver accepted the enrolled finger at the first press each time. It
rejected the different finger each time. The enrolment needed 19 seconds.

**These numbers are not a measurement of security.** The test used two
fingers of one person. It made only 4 comparisons of the correct finger and
5 comparisons of a different finger. The numbers show only that the larger
template increases the distance between the two groups. Do the test again
with more fingers and more persons before you use this sensor as the only
authentication factor.

### The measurement of 2026-09-09, on 13 fingers

A second measurement used a real collection instead of frames from a
Windows session. Five persons of one house gave 13 different fingers, and
each finger gave 9 or 10 presses. That is 128 frames, all with natural
placement. The tool `tools/collect-finger.sh` collected them.

One frame against one frame:

| | n | mean | max |
|---|---|---|---|
| correct finger | 567 | 0.146 | 0.797 |
| different finger | 7561 | 0.007 | 0.047 |

The operation of the driver, which scores one frame against a template and
takes the second best score:

| | n | mean | sd | median | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| correct finger | 128 | 0.260 | 0.145 | 0.260 | 0.512 | 0.593 | 0.593 |
| different finger | 1536 | 0.017 | 0.012 | 0.023 | 0.027 | 0.031 | **0.038** |

| | |
|---|---|
| equal error rate | **4.69 %** at the threshold 0.027 |
| separation `d'` | **2.36** |
| at the threshold 0.08 | 10.9 % incorrect reject, **0 incorrect accept of 1536** |

**0 incorrect accept operations in 1536 comparisons gives an upper limit of
approximately 0.2 % at 95 % confidence.** The earlier measurement used 5
comparisons and gave approximately 45 %.

**The rule of the second best score does work.** The largest score of a
different finger is 0.047 for one frame against one frame, but only 0.038
for the operation of the driver. A single lucky pair does not accept a
finger, because the driver needs two frames of the template to agree.

The threshold moved from 0.06 to 0.08 for this reason. Both values give 0
incorrect accept operations, and 0.08 costs one more incorrect reject
operation in 128 attempts. But 0.08 keeps a distance of 2.1 times to the
largest score of a different finger, where 0.06 keeps 1.6 times. An
estimate from 5 persons is optimistic, thus the distance is worth more.

### The difference is between persons, and not between an adult and a child

The mean score of each finger against its own group:

| finger | mean | | finger | mean |
|---|---|---|---|---|
| person B, right index | 0.345 | | person D, right index | 0.113 |
| person A, right thumb | 0.227 | | **child, left index** | **0.106** |
| person B, left index | 0.212 | | person A, right middle | 0.103 |
| person D, right index | 0.176 | | person C, right index | 0.090 |
| person A, right index | 0.146 | | person C, left index | 0.083 |
| person A, left middle | 0.123 | | **child, right index** | **0.082** |
| | | | person A, left thumb | **0.056** |

The two fingers of the child are in the middle of that list, and both are
better than the left thumb of an adult. `FT9201_RIDGE_PERIOD` is 10.7
pixels, and a measurement of 23 frames of adult fingers gave that value. A
child has finer ridges, thus the value could be incorrect for a child. This
measurement shows that it is not.

The difference between the best finger and the worst is 6 times, and it
follows the person and the finger, not the age.

### The rate holds for one physical position of the sensor

The driver handles a turn of the finger. The descriptor turns its sample
grid by the orientation of the keypoint, and it keeps each gradient
direction relative to that orientation. The RANSAC model then estimates a
turn of any angle together with a movement, and it puts no limit on the
angle. Thus a turn alone does not stop a match.

A change of the **position of the sensor** is a different thing. The person
who uses it then puts the finger down at a new angle, and the sensor sees a
different area of the skin through the same 96 x 96 window. That is a loss
of the common area, and not a turn.

A test showed this. The sensor moved to a longer cable, which changed the
angle of a natural press. The template came from the earlier position:

| | result |
|---|---|
| the template of the earlier position | no match, 3 times of 3 |
| a new template, after the move | match, 4 times of 4 |

Both tests used the threshold 0.08.

**Thus the rate of the incorrect reject operations above holds for one
physical position.** The collection of the frames used one session and one
position, and the enrolment of a user must use the position of normal use.
Enrol again after a change of the position of the sensor.

### Why the incorrect reject rate is an upper limit

Six frames of 128 score less than 0.05 against every other frame of their
own finger. A frame like that is a bad press, a finger that is not on the
centre of the sensor, or a frame of the wrong finger. The score alone does
not separate those causes.

Only one frame came out of the set: the person who collected the frames saw
the first press of one finger go to a different finger. The other five stay
in, because a bad press and a press that is not on the centre are normal
use and belong in the rate. A collection that removes each frame with a low
score measures its own selection, and not the sensor.

## What other projects measured

- **`Dgmtnz/ft9201-fingerprint-linux`** recalibrated a correlation matcher
  on a `2808:9338` unit. It reports an equal error rate near 0.07 %. Its
  matcher accepted 8 correct fingers of 8, and it rejected 10 different
  fingers of 10. Three values gave that result: the search radius 3 -> 16
  pixels, the enrolment stages 5 -> 15, and the threshold 0.30 -> 0.55.

  Its measurement of the stage count agrees with the measurement of this
  driver, on different hardware and with a different matcher. At the search
  radius 16 it gives these equal error rates: 7.4 % for 5 templates, 1.6 %
  for 10, and 0.07 % for 15. That project also sets its threshold above the
  value that gives the fewest incorrect reject operations, for the same
  reason as this driver: an estimate of the incorrect accept rate from few
  fingers is optimistic.
  That project also gives the warning about the position of the finger. Its
  numbers are the reason to believe that warning. A dataset with a changed
  position gives an equal error rate near 45 % for each matcher.
- **`NBN-PATRIC/ft9201-libfprint`** measured that NBIS gives at most 3
  minutiae on a 64 x 80 frame, and that it never matches. It also measured
  that a correlation over subtemplates separates the two groups only when
  the finger is in almost the same position. In the string table of the
  vendor library it found that the vendor keeps **more than one subtemplate
  for each finger**. The limit has the name
  `MAX_SUBTEMPLATES_PER_FINGER`.
- **`narkomart/focaltech-ft9348-linux`** calls the matcher of the vendor
  from the Linux library. Those functions are in the ELF `.symtab` and not
  in the dynamic symbols, thus `nm -D` does not show them. The function
  `focal_VerifyTwoTemplate` takes the **threshold as a parameter**, and it
  gives a homography matrix and an **overlap area** as separate results.
  The homography shows that the vendor also uses keypoints and a geometric
  model, which is the method of this driver.

The overlap area is the useful idea. This driver has no such value. A small
overlap can give a large ratio of inliers by accident. That is the probable
cause of the small distance between the two groups.
