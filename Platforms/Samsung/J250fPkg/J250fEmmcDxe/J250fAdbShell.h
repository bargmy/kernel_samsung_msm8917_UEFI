/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J250F_ADB_SHELL_H
#define J250F_ADB_SHELL_H
#include <Uefi.h>

#include "Core/j250f_adb.h"
VOID J250fAdbShellInit(EFI_HANDLE Parent);
VOID J250fAdbShellOps(jadb_ops *Ops);
BOOLEAN J250fAdbShellExitRequested(VOID);
VOID J250fAdbShellFinish(VOID);
#endif
