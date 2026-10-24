# Raspberry Pi Zero 2 W

This is the same board the netkit MPU benchmarks reach over Wi-Fi. Host scripts live in `netkit/tools/pi_ssh.sh` and default to:

| Variable | Value |
| --- | --- |
| `NETKIT_PI_HOST` | `pi.local` |
| `NETKIT_PI_USER` | `pi` |
| `NETKIT_PI_PASS` | set in the environment, not in this tree |
| Remote directory | `/home/pi/adxl345` |

`pi.local` is the name to use. The address moves when DHCP renews; an older static note of `192.168.0.176` is not this board.

Observed on the board while this driver was written:

- Raspberry Pi OS, Debian 13 (trixie), kernel `6.18` aarch64
- `g++` 14.2.0, which is enough for this C++23 code
- `sudo` without a password for user `pi`
- user `pi` already in group `i2c`
- `/boot/firmware/config.txt` had `#dtparam=i2c_arm=on` until `sudo raspi-config nonint do_i2c 0` created `/dev/i2c-1`

The breakout on this board responds at `0x53`. A hardware run after that change measured about 0.985 g while the board was still, and the self-test shift at 3.3 V was +1.219 g, −1.266 g, +2.664 g.

## Deploy from the Mac

```bash
export NETKIT_PI_HOST=pi.local
export NETKIT_PI_USER=pi
export NETKIT_PI_PASS='your-pi-password'
./tools/deploy.sh
```

`tools/deploy.sh` calls the netkit expect helper. It does not contain a password. The remote steps are:

1. `sudo raspi-config nonint do_i2c 0`
2. Unpack the source into `/home/pi/adxl345`
3. `make clean all test`
4. `./build/adxl345 scan`
5. `./build/adxl345_hw`
6. `./tools/run_kernel_compare.sh` (builds and loads the kernel modules, compares them, checks GPIO17 data-ready, then compares a polled reload with that interrupt)

A missing part prints a wiring hint and exits 2. A failed gravity or self-test check exits 1. Unit and regression failures stop the script before the hardware run.

## Run a single command

```bash
export NETKIT_PI_PASS='your-pi-password'
~/workspace/netkit/tools/pi_ssh.sh ssh './adxl345/build/adxl345 read --count 5'
```

## Clock and address

`/dev/i2c-1` is GPIO2 (SDA, pin 3) and GPIO3 (SCL, pin 5). The breakout INT pin is GPIO17 (physical pin 11). At 100 kHz, stay at or below 200 Hz output data rate. The CLI prints a note if you ask for faster.

The Adafruit board's default address is `0x53`. `scan` prints whichever address answered with `0xE5`.
