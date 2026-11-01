/** @file
  Integration EXAMPLE only. Keep in your platform package, not in the eMMC
  DXE driver. DO NOT include in platform firmware until your real PM8917 RPM
  functions are implemented. The externs deliberately have no fake bodies.

  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include "J250fPowerProtocol.h"

// Implement these in the *platform's* verified PM8917/RPM service layer.
// A successful status MUST mean voltage/load/votes actually succeeded.
extern EFI_STATUS EFIAPI
J250fPlatformVoteEmmcRails (BOOLEAN Enable, UINT32 VddUv, UINT32 VqmmcUv);
extern EFI_STATUS EFIAPI
J250fPlatformEnsureSdccGpll0 (VOID);

static EFI_STATUS EFIAPI
ProviderSetRails (J250F_EMMC_POWER_PROTOCOL *This, BOOLEAN Enable,
                  UINT32 VddMicrovolts, UINT32 VqmmcMicrovolts)
{
  (VOID)This;
  if (VddMicrovolts != 2900000 || VqmmcMicrovolts != 1800000) {
    return EFI_INVALID_PARAMETER;
  }
  return J250fPlatformVoteEmmcRails (Enable, VddMicrovolts, VqmmcMicrovolts);
}

static EFI_STATUS EFIAPI
ProviderGpll0Ready (J250F_EMMC_POWER_PROTOCOL *This)
{
  (VOID)This;
  return J250fPlatformEnsureSdccGpll0 ();
}

static J250F_EMMC_POWER_PROTOCOL mPowerApi = {
  ProviderSetRails,
  ProviderGpll0Ready
};

EFI_STATUS EFIAPI
J250fInstallPowerProvider (EFI_HANDLE *Handle)
{
  if (Handle == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return gBS->InstallMultipleProtocolInterfaces (
                Handle,
                &gJ250fEmmcPowerProtocolGuid, &mPowerApi,
                NULL);
}
