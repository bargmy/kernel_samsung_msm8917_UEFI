// SPDX-License-Identifier: GPL-2.0-only
#ifndef J250F_RPM_SMD_H
#define J250F_RPM_SMD_H
#include <Uefi.h>
// Polling transport for an already allocated APPS/RPM channel; no SMEM alloc.
EFI_STATUS J250fRpmConfigureSmem (UINTN Base, UINT32 Size);
EFI_STATUS J250fRpmOpen (VOID);
EFI_STATUS J250fRpmVote (UINT32 Type, UINT32 Id, CONST UINT32 *Keys,
                         CONST UINT32 *Values, UINTN Count);
EFI_STATUS J250fRpmSnapshot (UINT32 Values[6]);
CONST CHAR8 *J250fRpmStage (VOID);
#endif
