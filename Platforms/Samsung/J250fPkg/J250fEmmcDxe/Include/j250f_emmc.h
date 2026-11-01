/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 J250F eMMC port contributors.
 * Derived from Qualcomm/Samsung Linux SDHCI MSM and Linux MMC/SDHCI code;
 * original copyright notices remain in original/ of this distribution.
 *
 * Minimal dependency-free, polled, PIO SDCC1 eMMC transport for MSM8917.
 * Used by a Project Mu / EDK II DXE firmware BlockIo producer.
 * Firmware BlockIo stops at ExitBootServices: not an OS kernel driver.
 */
#ifndef J250F_EMMC_H
#define J250F_EMMC_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J250F_HC_PHYS       UINT32_C(0x07824900)
#define J250F_HC_SIZE       UINT32_C(0x00000500)
#define J250F_CORE_PHYS     UINT32_C(0x07824000)
#define J250F_CORE_SIZE     UINT32_C(0x00000800)
#define J250F_CMDQ_PHYS     UINT32_C(0x07824E00)
#define J250F_CMDQ_SIZE     UINT32_C(0x00000200)
#define J250F_GCC_PHYS      UINT32_C(0x01800000)
#define J250F_GCC_SIZE      UINT32_C(0x00080000)
#define J250F_TLMM_PHYS     UINT32_C(0x01000000)
#define J250F_TLMM_SIZE     UINT32_C(0x00300000)
#define J250F_VDD_UV       UINT32_C(2900000)
#define J250F_VQMMC_UV     UINT32_C(1800000)
#define J250F_BLOCK_SIZE    512u

/* All callbacks mandatory. MMIO offsets are relative to a mapped HC or CORE
 * aperture, and implementations MUST use uncached device mappings and a DMB
 * around ordering-sensitive accesses. On Windows, READ_REGISTER/WRITE_REGISTER
 * functions can implement MMIO callbacks after MmMapIoSpace. */
typedef enum j250f_region {
    J250F_REGION_HC = 0,
    J250F_REGION_CORE = 1
} j250f_region;

typedef struct j250f_ops {
    uint8_t  (*read8)(void *ctx, j250f_region region, uint32_t offset);
    uint16_t (*read16)(void *ctx, j250f_region region, uint32_t offset);
    uint32_t (*read32)(void *ctx, j250f_region region, uint32_t offset);
    void (*write8)(void *ctx, j250f_region region, uint32_t offset, uint8_t value);
    void (*write16)(void *ctx, j250f_region region, uint32_t offset, uint16_t value);
    void (*write32)(void *ctx, j250f_region region, uint32_t offset, uint32_t value);
    void (*barrier)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
    /* Must program SDCC1 GCC root clock + branch clocks, not an SDHCI divider.
     * Return nonzero on failure. 0 Hz is disable. */
    int (*set_clock_hz)(void *ctx, uint32_t hz);
    /* Must operate RPM PM8917 L8 (VDD) and L5 (VQMMC); maintain always-on
     * semantics. Return success ONLY after regulators reach requested state. */
    int (*set_rails)(void *ctx, int bus_on, uint32_t vdd_uv, uint32_t vqmmc_uv);
    /* Must program TLMM SDC1 clk/cmd/data/rclk active/sleep states. */
    int (*set_pins)(void *ctx, int active);
} j250f_ops;

typedef enum j250f_result {
    J250F_OK = 0,
    J250F_E_ARGUMENT = -1,
    J250F_E_TIMEOUT = -2,
    J250F_E_IO = -3,
    J250F_E_PROTOCOL = -4,
    J250F_E_UNSUPPORTED = -5,
    J250F_E_RANGE = -6,
    J250F_E_READ_ONLY = -7,
    J250F_E_NOT_READY = -8,
    J250F_E_POWER = -9
} j250f_result;

typedef struct j250f_emmc {
    j250f_ops io;
    void *context;
    uint32_t capacity_sectors; /* SEC_COUNT, user area only */
    uint32_t ocr;
    uint32_t cid[4];
    uint32_t csd[4];
    uint32_t rca;
    uint32_t frequency_hz;
    uint32_t last_interrupt;
    uint32_t last_present;
    uint32_t last_vendor;
    uint32_t last_dll;
    uint16_t last_clock;
    uint8_t last_command;
    uint32_t last_r1;
    uint8_t ext_csd[512];
    uint8_t high_capacity;
    uint8_t host_ready;
    uint8_t card_ready;
    uint8_t bus_8bit;
    uint8_t high_speed;
    uint8_t rails_on;
    uint8_t pins_active;
    uint8_t writes_enabled;
    uint8_t reserved;
} j250f_emmc;

/* Single-threaded operations; external caller serializes access. */
int j250f_init_host(j250f_emmc *h, const j250f_ops *ops, void *context);
int j250f_init_card(j250f_emmc *h);
int j250f_read_blocks(j250f_emmc *h, uint32_t sector, uint32_t count, void *buf);
/* Explicitly opt in to writing: zero by default to prevent accidents. */
void j250f_allow_writes(j250f_emmc *h, int allow);
int j250f_write_blocks(j250f_emmc *h, uint32_t sector, uint32_t count, const void *buf);
/* Safe in polling context, and callable from the platform's power IRQ DPC
 * only if the regulator and pin callbacks are safe in that context. */
int j250f_service_power(j250f_emmc *h);
/* Controller/core reset state, and power acknowledgement are exposed for tests. */
int j250f_set_frequency(j250f_emmc *h, uint32_t hz);
const char *j250f_error_string(int status);

#ifdef __cplusplus
}
#endif
#endif
