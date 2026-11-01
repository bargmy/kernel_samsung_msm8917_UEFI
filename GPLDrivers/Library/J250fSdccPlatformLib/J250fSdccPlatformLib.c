// SPDX-License-Identifier: GPL-2.0-only
/*
 * Derived from Samsung's Qualcomm kernel at
 * d8e6e0d79bf4e26a11d5f69c843a6ad096b2b181:
 * drivers/clk/msm/clock-local2.c, clock-gcc-8952.c,
 * include/dt-bindings/clock/msm-clocks-hwio-8952.h,
 * drivers/pinctrl/qcom/pinctrl-msm.c and pinctrl-msm8917.c.
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 * Distributed WITHOUT ANY WARRANTY. See COPYING.
 *
 * Adaptation: EDK2 MMIO/delay primitives replace Linux clock/pinctrl APIs.
 * Only the J250F's 400kHz XO rate and active SDCC1 pad configuration are
 * ported here. RPM voltage and bus votes are separate dependencies.
 */
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/J250fSdccPlatformLib.h>

#define GCC 0x01800000U
#define CMD (GCC + 0x42004U)
#define CFG (CMD + 4U)
#define M (CMD + 8U)
#define N (CMD + 12U)
#define D (CMD + 16U)
#define APPS (GCC + 0x42018U)
#define AHB (GCC + 0x4201cU)
#define PADS 0x0110a000U

STATIC EFI_STATUS WaitClear (UINTN Address, UINT32 Mask)
{
  UINTN I;
  for (I = 0; I < 1000; ++I) {
    if ((MmioRead32 (Address) & Mask) == 0) return EFI_SUCCESS;
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS EnableBranch (UINTN Address)
{
  MmioWrite32 (Address, MmioRead32 (Address) | 1U);
  return WaitClear (Address, 1U << 31);
}

EFI_STATUS EFIAPI J250fSdccSetIdentificationClock (VOID)
{
  EFI_STATUS Status;
  UINT32 Config;
  // Same frequency entry as ftbl_gcc_sdcc1_apps_clk:
  // F(400000, xo, 12, 1, 4). XO is source select 0.
  // HID divider = 2*12-1, M=1, N=~(4-1), D=~4, dual edge mode=2.
  Status = EnableBranch (AHB);
  if (EFI_ERROR (Status)) return Status;
  MmioWrite32 (M, 1U);
  MmioWrite32 (N, ~3U);
  MmioWrite32 (D, ~4U);
  Config = MmioRead32 (CFG);
  Config &= ~(0x1fU | 0x700U | 0x3000U);
  Config |= 23U | 0x2000U;
  MmioWrite32 (CFG, Config);
  MmioWrite32 (CMD, MmioRead32 (CMD) | 1U);
  Status = WaitClear (CMD, 1U);
  if (EFI_ERROR (Status)) return Status;
  // Confirm the selected source, divider and MND mode were accepted.
  if ((MmioRead32 (CFG) & 0x371fU) != (23U | 0x2000U))
    return EFI_DEVICE_ERROR;
  return EnableBranch (APPS);
}

EFI_STATUS EFIAPI J250fSdccSetActivePads (VOID)
{
  UINT32 Value, Mask, Active;
  // J250F rev04 DTS: CLK 16mA/no pull; CMD/DATA 10mA/pull-up;
  // RCLK pull-down. pinctrl-msm8917.c maps all four to TLMM+0x10a000.
  // pinctrl-msm.c: strength encoding=(mA/2)-1, pull-down=1, pull-up=3.
  Mask = (3U << 13) | (3U << 11) | (3U << 9) | (3U << 15) |
         (7U << 6) | (7U << 3) | 7U;
  Active = (3U << 11) | (3U << 9) | (1U << 15) |
           (7U << 6) | (4U << 3) | 4U;
  Value = (MmioRead32 (PADS) & ~Mask) | Active;
  MmioWrite32 (PADS, Value);
  return (MmioRead32 (PADS) & Mask) == Active ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
