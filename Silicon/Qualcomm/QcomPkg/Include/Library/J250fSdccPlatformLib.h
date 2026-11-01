// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J250F_SDCC_PLATFORM_LIB_H
#define J250F_SDCC_PLATFORM_LIB_H
#include <Uefi.h>
// Programs only local GCC SDCC1 clock and TLMM pads; does not manage rails.
// Caller must stop SDCLK before changing the source.
EFI_STATUS EFIAPI J250fSdccSetIdentificationClock (VOID);
EFI_STATUS EFIAPI J250fSdccSetActivePads (VOID);
#endif
