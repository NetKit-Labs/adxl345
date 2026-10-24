/* Shared by the kernel module and the C++23 comparison program.
 * Plain C. The kernel side is compiled with -std=gnu11.
 */
#ifndef ADXL345_CUSTOM_MOD_ABI_H
#define ADXL345_CUSTOM_MOD_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __s16 adxl345_s16;
typedef __u16 adxl345_u16;
#else
#include <stdint.h>
typedef int16_t adxl345_s16;
typedef uint16_t adxl345_u16;
#endif

/* Full-resolution scale. 3.9 mg/LSB, about 256 counts per g. */
#define ADXL345_CUSTOM_LSB_PER_G 256

/* One coherent sample. Host endian. Each read() of /dev/adxl345_custom
 * returns exactly one of these and ignores the file offset.
 */
struct adxl345_custom_sample {
	adxl345_s16 x;
	adxl345_s16 y;
	adxl345_s16 z;
	adxl345_u16 lsb_per_g;
};

#if defined(__cplusplus)
static_assert(sizeof(adxl345_custom_sample) == 8, "sample is 8 bytes");
#else
_Static_assert(sizeof(struct adxl345_custom_sample) == 8, "sample is 8 bytes");
#endif

#endif
