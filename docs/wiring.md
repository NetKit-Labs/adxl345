# Wiring

Adafruit ADXL345 breakout to a Raspberry Pi Zero 2 W, I2C mode. Five wires: power, ground, SDA, SCL, and the interrupt output. The breakout pulls CS high, which selects I2C, and pulls SDO low, which selects address `0x53`.

| Breakout | Pi Zero 2 W | Notes |
| --- | --- | --- |
| VIN | Pin 1, 3.3 V | The board accepts 3 V to 5 V on VIN. Prefer the Pi's 3.3 V pin so SDA and SCL stay at 3.3 V. |
| GND | Pin 6 | Any ground pin is fine. |
| SDA | Pin 3, GPIO2 | I2C1 data. The Pi already has the pull-ups. |
| SCL | Pin 5, GPIO3 | I2C1 clock. |
| INT | Pin 11, GPIO17 | The breakout's interrupt output. Data-ready is routed here. |

Leave CS and SDO unconnected. The board's silkscreen may say INT rather than INT1 or INT2. The module tries INT1, then INT2, and keeps the line that produces data-ready.

- Tie SDO to 3.3 V before power-up and the address becomes `0x1D`. The scanner tries `0x53`, then `0x1D`.
- Pull CS low and the part enters SPI. This driver will not see it. The Adafruit pull-up keeps CS high if you leave the pin alone.
- INT is push-pull and active high. The kernel module requests GPIO17 as a level-high interrupt. A weak pull-down on the Pi keeps a disconnected wire from floating.

## Enable the bus

Raspberry Pi OS leaves I2C off until you ask for it. On current images the line in `/boot/firmware/config.txt` is commented out:

```
#dtparam=i2c_arm=on
```

Enable it and load the device node:

```bash
sudo raspi-config nonint do_i2c 0
ls -l /dev/i2c-1
```

The `pi` user is already in the `i2c` group on Raspberry Pi OS, so a new login can open `/dev/i2c-1` without root. If the node is missing after `raspi-config`, reboot once.

The default I2C clock is 100 kHz. The ADXL345 datasheet allows an output data rate up to 800 Hz at 400 kHz, scaling linearly, so 100 kHz is comfortable through 200 Hz. For 400 Hz or 800 Hz output, add this and reboot:

```
dtparam=i2c_arm_baudrate=400000
```

## Power-up state

After VS and VDD I/O are both up, the part sits in standby. `DEVID` (`0x00`) reads `0xE5` even in standby. Acceleration registers read as zero until `POWER_CTL` bit 3 (measure) is set. Offset registers reset to zero on every power cycle.
