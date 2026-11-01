/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J250F_BOARD_H
#define J250F_BOARD_H
#include <stdint.h>

/* Register-level clock/pin translation from Qualcomm GCC-8952, clock-local2,
 * pinctrl-msm8917 sources; see original/Linux source references in docs. */

typedef struct j250f_board {
    void *ctx;
    uint32_t (*reg_read32)(void *ctx, uint32_t phys);
    void (*reg_write32)(void *ctx, uint32_t phys, uint32_t value);
    void (*barrier)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
    /* Invoked before enabling GPLL0. Must validate GCC digital voltage and
     * upstream source state against active firmware; 0 means ready. */
    int (*gpll0_power_ready)(void *ctx);
} j250f_board;

/* Implement j250f_ops.set_clock_hz with these (no Linux clock framework).
 * Return -1 on invalid request or hardware timeouts. Code assumes exclusive
 * serialized access to SDCC1 GCC RCG, not exclusive ownership of GPLL0. */
int j250f_gcc_sdcc1_clock(j250f_board *b, uint32_t hz);
/* Implement j250f_ops.set_pins; preserve other TLMM registers and fields. */
int j250f_tlmm_sdcc1_pins(j250f_board *b, int active);
#endif
