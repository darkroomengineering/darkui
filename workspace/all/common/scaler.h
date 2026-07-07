#ifndef __SCALER_H__
#define __SCALER_H__
#include <stdint.h>

//
//	arm NEON / C integer scalers for rg35xx
//	args/	src :	src offset		address of top left corner
//		dst :	dst offset		address	of top left corner
//		sw  :	src width		pixels
//		sh  :	src height		pixels
//		sp  :	src pitch (stride)	bytes	if 0, (src width * [2|4]) is used
//		dw  :	dst width		pixels
//		dh  :	dst height		pixels
//		dp  :	dst pitch (stride)	bytes	if 0, (src width * [2|4] * multiplier) is used
//
//	** NOTE **
//	since 32bit aligned addresses need to be processed for NEON scalers,
//	x-offset and stride pixels must be even# in the case of 16bpp,
//	if odd#, then handled by the C scaler
//

typedef void (*scaler_t)(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);

#ifdef HAS_NEON
//	NEON scalers (diagonal xmul==ymul only; off-diagonal and 32bpp variants are unused and were removed)
void scale1x1_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale2x2_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale3x3_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale4x4_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale5x5_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale6x6_n16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
#endif

//	C scalers (unaligned fallback for NEON path; only the diagonal xmul==ymul==1 variant is used externally)
void scale1x1_c16(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);

void scale1x_line(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale2x_line(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale3x_line(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale4x_line(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);

void scale2x_grid(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);
void scale3x_grid(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);

#endif
