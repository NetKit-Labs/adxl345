#pragma once

#include <cstdint>

// Register map for the Analog Devices ADXL345, Rev. G.
// Addresses 0x01 through 0x1C are reserved; the datasheet says not to touch them.

namespace adxl345::reg {

inline constexpr std::uint8_t devid = 0x00;
inline constexpr std::uint8_t thresh_tap = 0x1D;
inline constexpr std::uint8_t ofsx = 0x1E;
inline constexpr std::uint8_t ofsy = 0x1F;
inline constexpr std::uint8_t ofsz = 0x20;
inline constexpr std::uint8_t dur = 0x21;
inline constexpr std::uint8_t latent = 0x22;
inline constexpr std::uint8_t window = 0x23;
inline constexpr std::uint8_t thresh_act = 0x24;
inline constexpr std::uint8_t thresh_inact = 0x25;
inline constexpr std::uint8_t time_inact = 0x26;
inline constexpr std::uint8_t act_inact_ctl = 0x27;
inline constexpr std::uint8_t thresh_ff = 0x28;
inline constexpr std::uint8_t time_ff = 0x29;
inline constexpr std::uint8_t tap_axes = 0x2A;
inline constexpr std::uint8_t act_tap_status = 0x2B;
inline constexpr std::uint8_t bw_rate = 0x2C;
inline constexpr std::uint8_t power_ctl = 0x2D;
inline constexpr std::uint8_t int_enable = 0x2E;
inline constexpr std::uint8_t int_map = 0x2F;
inline constexpr std::uint8_t int_source = 0x30;
inline constexpr std::uint8_t data_format = 0x31;
inline constexpr std::uint8_t data_x0 = 0x32;
inline constexpr std::uint8_t data_x1 = 0x33;
inline constexpr std::uint8_t data_y0 = 0x34;
inline constexpr std::uint8_t data_y1 = 0x35;
inline constexpr std::uint8_t data_z0 = 0x36;
inline constexpr std::uint8_t data_z1 = 0x37;
inline constexpr std::uint8_t fifo_ctl = 0x38;
inline constexpr std::uint8_t fifo_status = 0x39;

inline constexpr std::uint8_t device_id = 0xE5;

// Adafruit breakout pulls SDO/ALT ADDRESS low, so the part answers at 0x53.
// Tie SDO high and the address becomes 0x1D.
inline constexpr std::uint8_t address_sdo_low = 0x53;
inline constexpr std::uint8_t address_sdo_high = 0x1D;

inline constexpr std::uint8_t power_link = 1u << 5;
inline constexpr std::uint8_t power_auto_sleep = 1u << 4;
inline constexpr std::uint8_t power_measure = 1u << 3;
inline constexpr std::uint8_t power_sleep = 1u << 2;
inline constexpr std::uint8_t power_wakeup_mask = 0x03;

inline constexpr std::uint8_t bw_low_power = 1u << 4;
inline constexpr std::uint8_t bw_rate_mask = 0x0F;

inline constexpr std::uint8_t fmt_self_test = 1u << 7;
inline constexpr std::uint8_t fmt_spi_3wire = 1u << 6;
inline constexpr std::uint8_t fmt_int_invert = 1u << 5;
inline constexpr std::uint8_t fmt_full_res = 1u << 3;
inline constexpr std::uint8_t fmt_justify = 1u << 2;
inline constexpr std::uint8_t fmt_range_mask = 0x03;

inline constexpr std::uint8_t fifo_mode_shift = 6;
inline constexpr std::uint8_t fifo_trigger = 1u << 5;
inline constexpr std::uint8_t fifo_samples_mask = 0x1F;
inline constexpr std::uint8_t fifo_trig_status = 1u << 7;
inline constexpr std::uint8_t fifo_entries_mask = 0x3F;

inline constexpr std::uint8_t tap_suppress = 1u << 3;
inline constexpr std::uint8_t tap_x = 1u << 2;
inline constexpr std::uint8_t tap_y = 1u << 1;
inline constexpr std::uint8_t tap_z = 1u << 0;

inline constexpr std::uint8_t act_ac_dc = 1u << 7;
inline constexpr std::uint8_t act_x = 1u << 6;
inline constexpr std::uint8_t act_y = 1u << 5;
inline constexpr std::uint8_t act_z = 1u << 4;
inline constexpr std::uint8_t inact_ac_dc = 1u << 3;
inline constexpr std::uint8_t inact_x = 1u << 2;
inline constexpr std::uint8_t inact_y = 1u << 1;
inline constexpr std::uint8_t inact_z = 1u << 0;

inline constexpr std::uint8_t status_act_x = 1u << 6;
inline constexpr std::uint8_t status_act_y = 1u << 5;
inline constexpr std::uint8_t status_act_z = 1u << 4;
inline constexpr std::uint8_t status_asleep = 1u << 3;
inline constexpr std::uint8_t status_tap_x = 1u << 2;
inline constexpr std::uint8_t status_tap_y = 1u << 1;
inline constexpr std::uint8_t status_tap_z = 1u << 0;

// Cleared by reading INT_SOURCE. DATA_READY, watermark, and overrun stay
// until the data registers are read.
inline constexpr std::uint8_t int_source_clear_on_read = 0x7C;

namespace irq {
inline constexpr std::uint8_t overrun = 1u << 0;
inline constexpr std::uint8_t watermark = 1u << 1;
inline constexpr std::uint8_t free_fall = 1u << 2;
inline constexpr std::uint8_t inactivity = 1u << 3;
inline constexpr std::uint8_t activity = 1u << 4;
inline constexpr std::uint8_t double_tap = 1u << 5;
inline constexpr std::uint8_t single_tap = 1u << 6;
inline constexpr std::uint8_t data_ready = 1u << 7;
inline constexpr std::uint8_t all = 0xFF;
}  // namespace irq

}  // namespace adxl345::reg
