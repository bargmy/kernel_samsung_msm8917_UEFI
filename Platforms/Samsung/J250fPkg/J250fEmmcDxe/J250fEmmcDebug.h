/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J250F_EMMC_DEBUG_H
#define J250F_EMMC_DEBUG_H
#include <Uefi.h>

#include "Include/j250f_emmc.h"
VOID J250fEmmcDebugAttach(j250f_emmc *Host);
BOOLEAN J250fEmmcDebugCommand(CONST CHAR8 *Command, CHAR8 *Output,
                              UINTN Capacity, UINTN *Length);
EFI_STATUS J250fEmmcTry(BOOLEAN IdentifyOnly);
EFI_STATUS J250fEmmcSetClock(UINT32 Hz);
UINT32 J250fEmmcAttempts(VOID);
#endif
