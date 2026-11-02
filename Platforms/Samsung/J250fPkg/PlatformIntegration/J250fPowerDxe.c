// SPDX-License-Identifier: GPL-2.0-only
// PM8917 L8/L5 always-on votes from the J250F revision-04 device tree.
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include "J250fPowerProtocol.h"
#include "RpmSmd.h"
#include "SmemMap.h"

STATIC EFI_STATUS EFIAPI SetRails (J250F_EMMC_POWER_PROTOCOL *This,
  BOOLEAN Enable, UINT32 Vdd, UINT32 Vqmmc)
{
  STATIC CONST UINT32 Keys[] = {0x6e657773U, 0x00007675U, 0x0000616dU}; // swen, uv, ma
  UINT32 Values[3];
  EFI_STATUS S;
  if (Vdd != 2900000 || Vqmmc != 1800000) return EFI_INVALID_PARAMETER;
  // Both supplies are qcom,vdd[-io]-always-on with low-power support.
  // Logical BUS_OFF lowers load, never physically disables shared rails.
  Values[0] = 1; Values[1] = Vdd; Values[2] = Enable ? 570 : 0;
  S = J250fRpmVote (0x616f646cU, 8, Keys, Values, 3); // ldoa8
  if (EFI_ERROR (S)) return S;
  Values[1] = Vqmmc; Values[2] = Enable ? 325 : 0;
  return J250fRpmVote (0x616f646cU, 5, Keys, Values, 3); // ldoa5
}
STATIC EFI_STATUS EFIAPI GpllReady (J250F_EMMC_POWER_PROTOCOL *This)
{
  // This recognition build intentionally uses only XO-derived 400kHz.
  // No fake success for PLL/regulator voting that is not implemented.
  return EFI_UNSUPPORTED;
}
STATIC J250F_EMMC_POWER_PROTOCOL mPower = {SetRails, GpllReady};
EFI_STATUS EFIAPI J250fPowerEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
  STATIC CONST UINT32 XoKey = 0x62616e45U, XoEnable = 1;
  EFI_STATUS S;
  CHAR8 Text[512];
  UINT32 Snapshot[6];
  UINTN Length, Second;
  SerialPortInitialize ();
  #if defined(J250F_ADB_RAM_EXECUTION)
  FbConStartScreen ();
  #endif
  Length = AsciiSPrint (Text, sizeof (Text), "J250F: opening RPM-SMD power service...\r\n");
  SerialPortWrite ((UINT8 *)Text, Length);
  S = J250fMapRpmSmem ();
  if (!EFI_ERROR (S)) S = J250fRpmOpen ();
  if (!EFI_ERROR (S)) S = J250fRpmVote (0x306b6c63U, 0, &XoKey, &XoEnable, 1);
  if (!EFI_ERROR (S)) S = SetRails (&mPower, TRUE, 2900000, 1800000);
  if (!EFI_ERROR (S)) S = gBS->InstallMultipleProtocolInterfaces (&Image,
    &gJ250fEmmcPowerProtocolGuid, &mPower, NULL);
  if (EFI_ERROR (S)) {
    #if defined(J250F_ADB_RAM_EXECUTION)
    FbConStartScreen ();
    #endif
    Length = AsciiSPrint (Text, sizeof (Text),
      "J250F RPM power backend failed: %r\r\nSTAGE=%a\r\n"
      "No eMMC initialization attempted.\r\nHolding for 90 seconds.\r\n",
      S, J250fRpmStage ());
    SerialPortWrite ((UINT8 *)Text, Length);
    if (!EFI_ERROR (J250fRpmSnapshot (Snapshot))) {
      Length = AsciiSPrint (Text, sizeof (Text),
        "LOCAL_STATE=%u REMOTE_STATE=%u\r\nTX_HEAD=%u TX_TAIL=%u\r\nRX_HEAD=%u RX_TAIL=%u\r\n",
        Snapshot[0], Snapshot[1], Snapshot[2], Snapshot[3], Snapshot[4], Snapshot[5]);
      SerialPortWrite ((UINT8 *)Text, Length);
    }
    for (Second = 0; Second < 90; ++Second) gBS->Stall (1000000);
  }
  return S;
}
