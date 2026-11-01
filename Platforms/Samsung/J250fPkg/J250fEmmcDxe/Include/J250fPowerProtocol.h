/** @file
  Samsung J250F platform PM8917/RPM power handoff.
  The platform firmware supplies this protocol. Do not install an implementation
  which blindly returns EFI_SUCCESS: without verified rail/PLL votes, issuing
  SDCC1 commands can fail or be unsafe.
  SPDX-License-Identifier: GPL-2.0-only
**/
#ifndef J250F_POWER_PROTOCOL_H
#define J250F_POWER_PROTOCOL_H

#include <Uefi.h>

typedef struct _J250F_EMMC_POWER_PROTOCOL J250F_EMMC_POWER_PROTOCOL;

struct _J250F_EMMC_POWER_PROTOCOL {
  // Validate / acquire the PM8917 eMMC VDD and VQMMC supplies using firmware
  // RPM, including appropriate votes/current. VDD=2900000uV, IO=1800000uV.
  EFI_STATUS (EFIAPI *SetRails)(
    IN J250F_EMMC_POWER_PROTOCOL *This,
    IN BOOLEAN Enable,
    IN UINT32 VddMicrovolts,
    IN UINT32 VqmmcMicrovolts
    );

  // Verify GPLL0 (and prerequisite digital rail votes) is available before
  // programming SDCC1 to 20MHz. No unconditional success stubs.
  EFI_STATUS (EFIAPI *Gpll0Ready)(
    IN J250F_EMMC_POWER_PROTOCOL *This
    );
};

extern EFI_GUID gJ250fEmmcPowerProtocolGuid;

#endif
