/* Shared by the kernel module and the C++23 programs.
 * Plain C. The kernel side is compiled with -std=gnu11.
 */
#ifndef ADXL345_CUSTOM_MOD_ABI_H
#define ADXL345_CUSTOM_MOD_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __s16 adxl345_s16;
typedef __u16 adxl345_u16;
typedef __u32 adxl345_u32;
typedef __s64 adxl345_s64;
#else
#include <stdint.h>
typedef int16_t adxl345_s16;
typedef uint16_t adxl345_u16;
typedef uint32_t adxl345_u32;
typedef int64_t adxl345_s64;
#endif

/* Full-resolution scale. 3.9 mg/LSB, about 256 counts per g. */
#define ADXL345_CUSTOM_LSB_PER_G 256

/* INT_ENABLE / INT_MAP / INT_SOURCE. DATA_READY is bit 7, OVERRUN is bit 0. */
#define ADXL345_CUSTOM_INT_OVERRUN 0x01u
#define ADXL345_CUSTOM_INT_DATA_READY 0x80u
#define ADXL345_CUSTOM_INT_ENABLE_BITS \
	(ADXL345_CUSTOM_INT_DATA_READY | ADXL345_CUSTOM_INT_OVERRUN)

/* Linux IRQ_TYPE_LEVEL_HIGH. The pin stays high until the data registers are read. */
#define ADXL345_CUSTOM_IRQ_TYPE_LEVEL_HIGH 4

/* INT_MAP bit set sends that event to INT2. Clear sends it to INT1. */
static inline unsigned adxl345_custom_int_map(int line)
{
	return line == 2 ? ADXL345_CUSTOM_INT_ENABLE_BITS : 0u;
}

/* sequence 0 means nothing has been published, so the first sample is not a drop.
 * A later publish counts a drop when userspace has not taken the previous one.
 * Returns the sequence stored in the new sample.
 */
static inline adxl345_u32 adxl345_custom_note_publish(adxl345_u32 *sequence,
						      adxl345_u32 read_seq,
						      adxl345_u32 *dropped)
{
	if (*sequence != 0 && *sequence != read_seq)
		*dropped = *dropped + 1u;
	*sequence = *sequence + 1u;
	return *sequence;
}

/* One sample captured by the driver. Host endian.
 * read() returns exactly one of these and ignores the file offset.
 * In interrupt mode, read() blocks until data-ready captures a new sample.
 * sequence increases once per captured sample. overruns counts chip OVERRUN
 * events seen by the interrupt handler. timestamp_ns is CLOCK_MONOTONIC.
 */
struct adxl345_custom_sample {
	adxl345_s16 x;
	adxl345_s16 y;
	adxl345_s16 z;
	adxl345_u16 lsb_per_g;
	adxl345_u32 sequence;
	adxl345_u32 overruns;
	adxl345_s64 timestamp_ns;
};

#if defined(__cplusplus)
static_assert(sizeof(adxl345_custom_sample) == 24, "sample is 24 bytes");
#else
_Static_assert(sizeof(struct adxl345_custom_sample) == 24, "sample is 24 bytes");
#endif

#endif
