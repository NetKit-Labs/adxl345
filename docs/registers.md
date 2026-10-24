# Registers

Summary of the ADXL345 map this driver uses (Analog Devices Rev. G). Reserved `0x01`–`0x1C` are not accessed.

| Address | Name | Access | Reset | What the driver does with it |
| --- | --- | --- | --- | --- |
| `0x00` | DEVID | R | `0xE5` | `verify()` |
| `0x1D` | THRESH_TAP | R/W | `0x00` | 62.5 mg/LSB, unsigned |
| `0x1E`–`0x20` | OFSX/Y/Z | R/W | `0x00` | 15.6 mg/LSB, signed. One 3-byte write. |
| `0x21` | DUR | R/W | `0x00` | Tap duration, 625 µs/LSB. `0` disables tap. |
| `0x22` | Latent | R/W | `0x00` | Double-tap wait, 1.25 ms/LSB |
| `0x23` | Window | R/W | `0x00` | Double-tap window, 1.25 ms/LSB |
| `0x24` | THRESH_ACT | R/W | `0x00` | 62.5 mg/LSB |
| `0x25` | THRESH_INACT | R/W | `0x00` | 62.5 mg/LSB |
| `0x26` | TIME_INACT | R/W | `0x00` | 1 s/LSB |
| `0x27` | ACT_INACT_CTL | R/W | `0x00` | AC/DC and per-axis enables |
| `0x28` | THRESH_FF | R/W | `0x00` | Free-fall threshold, 62.5 mg/LSB. 300–600 mg is the suggested range. |
| `0x29` | TIME_FF | R/W | `0x00` | 5 ms/LSB. 100–350 ms is the suggested range. |
| `0x2A` | TAP_AXES | R/W | `0x00` | Suppress bit and axis enables |
| `0x2B` | ACT_TAP_STATUS | R | `0x00` | First axis of a tap or activity event, plus asleep |
| `0x2C` | BW_RATE | R/W | `0x0A` | Low-power bit (D4) and rate code (D3:D0). `0x0A` is 100 Hz. |
| `0x2D` | POWER_CTL | R/W | `0x00` | Link, auto-sleep, measure, sleep, wakeup rate |
| `0x2E` | INT_ENABLE | R/W | `0x00` | Same bit order as INT_SOURCE |
| `0x2F` | INT_MAP | R/W | `0x00` | `0` routes that event to INT1, `1` routes it to INT2 |
| `0x30` | INT_SOURCE | R | `0x02` | Reading clears tap, activity, inactivity, and free-fall. Data-ready, watermark, and overrun clear when the data registers are read. |
| `0x31` | DATA_FORMAT | R/W | `0x00` | Self-test, SPI 3-wire, INT invert, full resolution, justify, range |
| `0x32`–`0x37` | DATAX/Y/Z | R | `0x00` | Little-endian int16. Always read as one 6-byte burst. |
| `0x38` | FIFO_CTL | R/W | `0x00` | Mode, trigger pin, 5-bit sample field (0–31) |
| `0x39` | FIFO_STATUS | R | `0x00` | Trigger flag and entry count (0–32) |

## DATA_FORMAT

| Bit | Name | Meaning |
| --- | --- | --- |
| D7 | SELF_TEST | Electrostatic force on the sensor |
| D6 | SPI | `1` selects 3-wire SPI. Leave it clear on I2C. |
| D5 | INT_INVERT | `1` makes the interrupt pins active low |
| D3 | FULL_RES | Keep 3.9 mg/LSB and grow the word as the range grows |
| D2 | Justify | `1` is left-justified. The driver shifts it back to counts. |
| D1:D0 | Range | `00` ±2 g, `01` ±4 g, `10` ±8 g, `11` ±16 g |

Full resolution is 10, 11, 12, or 13 bits at ±2/4/8/16 g, always 256 LSB/g. Fixed 10-bit mode is 256, 128, 64, or 32 LSB/g at those ranges.

## BW_RATE codes

| Code | ODR | Bandwidth |
| --- | --- | --- |
| `0x0` | 0.10 Hz | 0.05 Hz |
| `0x1` | 0.20 Hz | 0.10 Hz |
| `0x2` | 0.39 Hz | 0.20 Hz |
| `0x3` | 0.78 Hz | 0.39 Hz |
| `0x4` | 1.56 Hz | 0.78 Hz |
| `0x5` | 3.13 Hz | 1.56 Hz |
| `0x6` | 6.25 Hz | 3.13 Hz |
| `0x7` | 12.5 Hz | 6.25 Hz |
| `0x8` | 25 Hz | 12.5 Hz |
| `0x9` | 50 Hz | 25 Hz |
| `0xA` | 100 Hz | 50 Hz |
| `0xB` | 200 Hz | 100 Hz |
| `0xC` | 400 Hz | 200 Hz |
| `0xD` | 800 Hz | 400 Hz |
| `0xE` | 1600 Hz | 800 Hz |
| `0xF` | 3200 Hz | 1600 Hz |

D4 set is low-power mode. It is useful from 12.5 Hz through 400 Hz. On I2C the datasheet stops at 800 Hz when the bus is 400 kHz, and the limit scales down with the clock. 1600 Hz and 3200 Hz also change which bit is the LSB; this driver refuses those two rates unless `HighRatePolicy::allow` is passed.

## POWER_CTL

| Bit | Name | Meaning |
| --- | --- | --- |
| D5 | Link | Activity detection waits until inactivity has been seen |
| D4 | AUTO_SLEEP | With link set, inactivity puts the part to sleep |
| D3 | Measure | `0` standby, `1` measure |
| D2 | Sleep | Sampling moves to the wakeup rate. Data-ready is suppressed. |
| D1:D0 | Wakeup | Sleep-mode rate: 8, 4, 2, or 1 Hz |

Clearing sleep, link, or auto-sleep is done as two writes when measurement should continue: standby with the new bits, then measure. That is the sequence the datasheet asks for so the part is re-biased.

## Interrupts

Same positions in `INT_ENABLE`, `INT_MAP`, and `INT_SOURCE`:

| Bit | Event |
| --- | --- |
| D7 | DATA_READY |
| D6 | SINGLE_TAP |
| D5 | DOUBLE_TAP |
| D4 | Activity |
| D3 | Inactivity |
| D2 | FREE_FALL |
| D1 | Watermark |
| D0 | Overrun |

The kernel module enables DATA_READY and OVERRUN together (`INT_ENABLE` `0x81`). `INT_MAP` `0x00` sends both to INT1. `INT_MAP` `0x81` sends both to INT2. Reading `DATAX0` through `DATAZ1` clears those two bits. The other latched events clear when `INT_SOURCE` is read. `kernel/adxl34x-overlay.dts` describes that pin to the image's `adxl34x` driver as GPIO17, level-high (`IRQ_TYPE_LEVEL_HIGH`, value 4).

## FIFO

`FIFO_CTL` D7:D6 is the mode: bypass `00`, FIFO `01`, stream `10`, trigger `11`. D5 selects INT2 as the trigger pin. D4:D0 is the watermark or the number of samples kept after a trigger, 0 through 31.

In bypass mode the data registers hold the latest sample. In the other modes a 6-byte read pops the oldest sample. A short read drops the rest of that sample, so the driver always reads all six bytes.

## I2C framing

A write is start, slave address + W, register address, payload, stop.

A read is start, slave address + W, register address, repeated start, slave address + R, payload, stop. The register pointer increments for each extra byte. The SPI multiple-byte bit (bit 6 of the command byte) is not used on I2C. The driver reads `0x32` for six bytes, which is the transaction the regression suite locks in.
