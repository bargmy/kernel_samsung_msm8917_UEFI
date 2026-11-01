// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// J250F experimental SDCC1 *IDENTIFICATION ONLY* diagnostic.
// Only MMC CMD0 (GO_IDLE_STATE), CMD1 (SEND_OP_COND), and CMD2 (ALL_SEND_CID).
// No data commands, GPT reads, boot partitions, erases, card SWITCH (CMD6),
// partition configuration, DMA, block protocols, or storage writes.
//
// This code DOES modify VOLATILE controller registers and resets card state.
// It is NOT a hardware write-protection mechanism, and is UNTESTED ON DEVICE.
// Separate opt-in branch, never automatically merged into known-good firmware.
//
// References:
//  Samsung J250F MSM8917 DTS (sdhci@7824900 and core_mem@7824000).
//  Qualcomm SDHCI variant source: Samsung drivers/mmc/host/sdhci-msm.c.
//  Standard SDHCI offsets/flags: U-Boot include/sdhci.h.
//  MMC ID sequence: U-Boot drivers/mmc/mmc.c.
// No GPL code copied.

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/BaseLib.h>

#define HOST_BASE          0x07824900U
#define CORE_BASE          0x07824000U

#define REG_ARGUMENT       0x08U
#define REG_COMMAND        0x0EU
#define REG_RESPONSE       0x10U
#define REG_PRESENT_STATE  0x24U
#define REG_POWER          0x29U
#define REG_CLOCK          0x2CU
#define REG_RESET          0x2FU
#define RESET_CMD          0x02U
#define REG_INT_STATUS     0x30U
#define REG_INT_ENABLE     0x34U
#define REG_CAPS           0x40U
#define REG_HOST_VERSION   0xFEU
#define REG_CORE_VERSION   0x50U

#define CMD_INHIBIT        0x00000001U
#define DATA_INHIBIT       0x00000002U
#define CMD_COMPLETE       0x00000001U
#define INT_ERRORS         0xFFFF8000U
#define CLOCK_INT_EN       0x0001U
#define CLOCK_INT_STABLE   0x0002U
#define CLOCK_SD_EN        0x0004U
#define MMC_OCR_BUSY       0x80000000U
#define MMC_OCR_HCS        0x40000000U
#define MMC_OCR_VOLTAGE    0x00FF8000U

STATIC EFI_BOOT_SERVICES *mBs;

STATIC VOID
Log (CONST CHAR8 *Message)
{
  // This SerialPortLib API takes a mutable pointer; it does not modify input.
  SerialPortWrite ((UINT8 *)Message, AsciiStrLen (Message));
}

STATIC VOID
PauseMs (UINTN Milliseconds)
{
  if (mBs != NULL) {
    mBs->Stall (Milliseconds * 1000U);
  }
}

STATIC EFI_STATUS
WaitCommandIdle (VOID)
{
  UINTN Retry;
  for (Retry = 0; Retry < 2000; ++Retry) {
    if ((MmioRead32 (HOST_BASE + REG_PRESENT_STATE) &
         (CMD_INHIBIT | DATA_INHIBIT)) == 0) {
      return EFI_SUCCESS;
    }
    PauseMs (1);
  }
  return EFI_TIMEOUT;
}

// ALL allowed commands are hardcoded here. All commands are identification
// or volatile-state initialization; none has a data phase or block address.
// Calling with any other command index immediately fails without MMIO writes.
STATIC EFI_STATUS
SendIdCommand (
  IN UINT32 Cmd,
  IN UINT32 Argument,
  OUT UINT32 *Response0
  )
{
  UINT16 Command;
  UINT32 Status;
  UINTN Retry;
  EFI_STATUS Result;
  CHAR8 Trace[160];

  if (Cmd == 0) {
    Command = 0x0000U;  // GO_IDLE_STATE; no response
  } else if (Cmd == 1) {
    Command = 0x0102U;  // SEND_OP_COND; 48-bit R3, no CRC/index check
  } else if (Cmd == 2) {
    Command = 0x0209U;  // ALL_SEND_CID; 136-bit R2, CRC check
  } else {
    return EFI_ACCESS_DENIED;
  }

  Result = WaitCommandIdle ();
  if (EFI_ERROR (Result) && Cmd == 0 &&
      (MmioRead32 (HOST_BASE + REG_PRESENT_STATE) &
       (CMD_INHIBIT | DATA_INHIBIT)) == CMD_INHIBIT) {
    Log ("CMD0 NOT ISSUED: inherited CMD inhibit. Resetting HOST CMD line once.\r\n");
    MmioWrite8 (HOST_BASE + REG_RESET, RESET_CMD);
    for (Retry = 0; Retry < 100; ++Retry) {
      if ((MmioRead8 (HOST_BASE + REG_RESET) & RESET_CMD) == 0) {
        break;
      }
      PauseMs (1);
    }
    if (Retry == 100) {
      Log ("HOST CMD reset did not finish; no card command issued.\r\n");
      return EFI_TIMEOUT;
    }
    Result = WaitCommandIdle ();
  }
  if (EFI_ERROR (Result)) {
    Log ("PRE-COMMAND idle timeout; command register NOT written.\r\n");
    return Result;
  }

  AsciiSPrint (Trace, sizeof (Trace), "Issuing CMD%u: STATE=%08x CLK=%04x EN=%08x\r\n",
    Cmd, MmioRead32 (HOST_BASE + REG_PRESENT_STATE),
    (UINT32)MmioRead16 (HOST_BASE + REG_CLOCK),
    MmioRead32 (HOST_BASE + REG_INT_ENABLE));
  Log (Trace);
  // These are HOST CONTROLLER register writes only. REG_INT_STATUS is W1C.
  MmioWrite32 (HOST_BASE + REG_INT_STATUS, 0xFFFFFFFFU);
  MmioWrite32 (HOST_BASE + REG_ARGUMENT, Argument);
  MmioWrite16 (HOST_BASE + REG_COMMAND, Command);

  for (Retry = 0; Retry < 3000; ++Retry) {
    Status = MmioRead32 (HOST_BASE + REG_INT_STATUS);
    if ((Status & INT_ERRORS) != 0) {
      AsciiSPrint (Trace, sizeof (Trace), "CMD%u error: INT_STATUS=%08x\r\n", Cmd, Status);
      Log (Trace);
      return EFI_DEVICE_ERROR;
    }
    if ((Status & CMD_COMPLETE) != 0) {
      if (Response0 != NULL) {
        *Response0 = MmioRead32 (HOST_BASE + REG_RESPONSE);
      }
      MmioWrite32 (HOST_BASE + REG_INT_STATUS, CMD_COMPLETE);
      return EFI_SUCCESS;
    }
    PauseMs (1);
  }
  Log ("POST-COMMAND completion timeout; command register WAS written.\r\n");
  return EFI_TIMEOUT;
}

// Lower clock to <= 400 kHz for identification. Do not change card supply
// voltage, GPIOs or PMIC configuration. Qualcomm timing is normalized below.
// SDHCI v3+ clock divider is in bits [15:8] and [7:6].
// HOST_VERSION spec byte is zero-based: SDHCI 3.0 == 2.
STATIC EFI_STATUS
SetIdentificationClock (
  IN UINT16 OriginalClock,
  IN UINT32 Capabilities,
  IN UINT16 HostVersion
  )
{
  UINT32 Vendor;
  UINT32 Divisor;
  UINT16 Clock;
  UINTN Retry;

  if ((HostVersion & 0xFFU) < 0x02U ||
      (OriginalClock & (CLOCK_INT_EN | CLOCK_INT_STABLE | CLOCK_SD_EN)) !=
          (CLOCK_INT_EN | CLOCK_INT_STABLE | CLOCK_SD_EN)) {
    return EFI_UNSUPPORTED;
  }

  // Qualcomm marks CAPS clock-base broken. Use the maximum v3 divider.
  // J250F DTS supports source rates through 384 MHz: /2046 ~=188 kHz.
  // This does not program GCC or assume that CAPS describes its source rate.
  Divisor = 1023U;

  Clock = (UINT16)(
      CLOCK_INT_EN |
      ((Divisor & 0xFFU) << 8) |
      ((Divisor & 0x300U) >> 2)
      );

  // Stop card clock before changing host timing and Qualcomm clock selection.
  // Stop SDCLK, select a <=400kHz divisor, wait for internal-clock stable,
  // then enable card clock. This changes controller state, not flash sectors.
  MmioWrite16 (HOST_BASE + REG_CLOCK, OriginalClock & ~CLOCK_SD_EN);
  // Normalize inherited HS200/HS400 host settings, following Samsung's
  // non-HS400 clock-selection path. Preserve voltage and pad-power bits.
  Vendor = MmioRead32 (HOST_BASE + 0x10c);
  Vendor &= ~((3U << 8) | (1U << 18) | (7U << 19) | (1U << 1));
  Vendor |= (2U << 8);
  MmioWrite32 (HOST_BASE + 0x10c, Vendor);
  MmioWrite16 (HOST_BASE + 0x3e,
    MmioRead16 (HOST_BASE + 0x3e) & (UINT16)~0x80c7U);
  // Legacy, one-bit host bus for identification; no data transfers occur.
  MmioWrite8 (HOST_BASE + 0x28,
    MmioRead8 (HOST_BASE + 0x28) & (UINT8)~0x26U);
  Log ("Legacy host timing; max divider=1023, CAPS clock ignored.\r\n");
  MmioWrite16 (HOST_BASE + REG_CLOCK, Clock);

  for (Retry = 0; Retry < 500; ++Retry) {
    if ((MmioRead16 (HOST_BASE + REG_CLOCK) & CLOCK_INT_STABLE) != 0) {
      MmioWrite16 (HOST_BASE + REG_CLOCK, Clock | CLOCK_SD_EN);
      return EFI_SUCCESS;
    }
    PauseMs (1);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
EFIAPI
QualcommSdhciDiagnosticEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  CHAR8 Buffer[512];
  UINT16 HostVersion, ClockControl;
  UINT8 PowerControl;
  UINT32 Caps, CoreVersion, Ocr = 0, Cid[4] = {0, 0, 0, 0};
  UINT32 CardOcrRequest;
  UINT32 OriginalIntEnable = 0;
  BOOLEAN StatusEnableChanged = FALSE;
  UINTN Iter;
  EFI_STATUS Result;

  mBs = (SystemTable != NULL) ? SystemTable->BootServices : NULL;

  Log (
    "\r\n=== J250F eMMC CHIP IDENTIFICATION (NO SECTOR ACCESS) ===\r\n"
    "Allowed MMC commands: CMD0 CMD1 CMD2 only.\r\n"
    "Host MMIO writes: clock / timing / HC mode / status enable / command / reset.\r\n"
    "NO DATA TRANSFER, NO ERASE, NO SWITCH, NO BLOCK IO.\r\n"
    );

  HostVersion = MmioRead16 (HOST_BASE + REG_HOST_VERSION);
  Caps = MmioRead32 (HOST_BASE + REG_CAPS);
  CoreVersion = MmioRead32 (CORE_BASE + REG_CORE_VERSION);
  ClockControl = MmioRead16 (HOST_BASE + REG_CLOCK);
  PowerControl = MmioRead8 (HOST_BASE + REG_POWER);

  AsciiSPrint (
    Buffer, sizeof (Buffer),
    "HOST=%04x CORE=%08x CLK=%04x PWR=%02x CAPS=%08x\r\n",
    (UINT32)HostVersion, CoreVersion, (UINT32)ClockControl,
    (UINT32)PowerControl, Caps
    );
  Log (Buffer);

  // Samsung sdhci-msm.c: core HC_MODE +0x78; power handshake +0xdc/e0/e8.
  // Vendor register is HOST-relative, not core-relative.
  AsciiSPrint (Buffer, sizeof (Buffer),
    "QCOM HC_MODE=%08x VENDOR=%08x HC2=%04x\r\n"
    "PWR_STATUS=%08x MASK=%08x CTL=%08x\r\n"
    "CAPS clock base ignored; using maximum SDHCI v3 divisor.\r\n",
    MmioRead32 (CORE_BASE + 0x78), MmioRead32 (HOST_BASE + 0x10c),
    (UINT32)MmioRead16 (HOST_BASE + 0x3e),
    MmioRead32 (CORE_BASE + 0xdc), MmioRead32 (CORE_BASE + 0xe0),
    MmioRead32 (CORE_BASE + 0xe8));
  Log (Buffer);

  // Fail-closed if the inherited host state or readback is suspicious.
  if (HostVersion == 0 || HostVersion == 0xFFFF ||
      CoreVersion == 0 || CoreVersion == 0xFFFFFFFF ||
      (PowerControl & 1U) == 0) {
    Log ("STOP: invalid controller/power state. NO COMMANDS SENT.\r\n");
    Result = EFI_UNSUPPORTED;
    goto Done;
  }

  // Samsung sdhci-msm.c probe: enable the SDHCI frontend and prevent
  // feedback-clock software reset. Preserve other inherited core-mode bits.
  // This changes host state only; it does not send a card command.
  {
    UINT32 CoreMode = MmioRead32 (CORE_BASE + 0x78);
    AsciiSPrint (Buffer, sizeof (Buffer), "HC_MODE before=%08x\r\n", CoreMode);
    Log (Buffer);
    MmioWrite32 (CORE_BASE + 0x78, CoreMode | 0x2001U);
    CoreMode = MmioRead32 (CORE_BASE + 0x78);
    AsciiSPrint (Buffer, sizeof (Buffer), "HC_MODE readback=%08x (required bits=00002001)\r\n", CoreMode);
    Log (Buffer);
    if ((CoreMode & 0x2001U) != 0x2001U) {
      Log ("STOP: Qualcomm HC mode enable did not latch. NO COMMANDS SENT.\r\n");
      Result = EFI_DEVICE_ERROR;
      goto Done;
    }
  }

  Result = SetIdentificationClock (ClockControl, Caps, HostVersion);
  if (EFI_ERROR (Result)) {
    Log ("STOP: cannot safely establish 400kHz identification clock.\r\n");
    goto Done;
  }

  // Enable command/error status latching for polling; do not enable IRQ signals.
  OriginalIntEnable = MmioRead32 (HOST_BASE + REG_INT_ENABLE);
  MmioWrite32 (HOST_BASE + REG_INT_ENABLE,
               OriginalIntEnable | CMD_COMPLETE | 0x000F0000U);
  StatusEnableChanged = TRUE;
  Result = SendIdCommand (0, 0, NULL);
  if (EFI_ERROR (Result)) {
    Log ("STOP: CMD0 failed. No additional commands.\r\n");
    goto Done;
  }
  PauseMs (2);

  // First query available OCR voltage range without requesting high capacity.
  Result = SendIdCommand (1, 0, &Ocr);
  if (EFI_ERROR (Result) || (Ocr & MMC_OCR_VOLTAGE) == 0) {
    Log ("STOP: CMD1 OCR query failed or voltage mismatch.\r\n");
    Result = EFI_DEVICE_ERROR;
    goto Done;
  }

  CardOcrRequest = MMC_OCR_HCS | (Ocr & MMC_OCR_VOLTAGE);
  for (Iter = 0; Iter < 100; Iter++) {
    Result = SendIdCommand (1, CardOcrRequest, &Ocr);
    if (EFI_ERROR (Result)) {
      Log ("STOP: CMD1 OCR negotiation failed.\r\n");
      goto Done;
    }
    if ((Ocr & MMC_OCR_BUSY) != 0) {
      break;
    }
    PauseMs (10);
  }
  if ((Ocr & MMC_OCR_BUSY) == 0) {
    Log ("STOP: CMD1 OCR busy timeout.\r\n");
    Result = EFI_TIMEOUT;
    goto Done;
  }

  Result = SendIdCommand (2, 0, &Cid[0]);
  if (EFI_ERROR (Result)) {
    Log ("STOP: CMD2 CID query failed.\r\n");
    goto Done;
  }
  // SDHCI R2 response is distributed across four 32-bit registers.
  Cid[0] = MmioRead32 (HOST_BASE + REG_RESPONSE + 0U);
  Cid[1] = MmioRead32 (HOST_BASE + REG_RESPONSE + 4U);
  Cid[2] = MmioRead32 (HOST_BASE + REG_RESPONSE + 8U);
  Cid[3] = MmioRead32 (HOST_BASE + REG_RESPONSE + 12U);

  AsciiSPrint (
    Buffer, sizeof (Buffer),
    "MMC IDENTIFIED! OCR=%08x\r\n"
    "CID_RAW: %08x %08x %08x %08x\r\n"
    "NO eMMC SECTOR READ OR WRITE HAS OCCURRED.\r\n",
    Ocr, Cid[3], Cid[2], Cid[1], Cid[0]
    );
  Log (Buffer);

Done:
  if (StatusEnableChanged) {
    AsciiSPrint (Buffer, sizeof (Buffer),
      "FINAL_INT_STATUS=%08x PRESENT_STATE=%08x\r\n",
      MmioRead32 (HOST_BASE + REG_INT_STATUS),
      MmioRead32 (HOST_BASE + REG_PRESENT_STATE));
    Log (Buffer);
    MmioWrite32 (HOST_BASE + REG_INT_ENABLE, OriginalIntEnable);
  }
  AsciiSPrint (Buffer, sizeof (Buffer), "RESULT=0x%08x\r\n", (UINT32)Result);
  Log (Buffer);
  Log (
    "PHOTO MODE: values displayed for 90 seconds.\r\n"
    "NOTE: CMD0 resets VOLATILE card state; power-cycle after test.\r\n"
    "This diagnostic is not a Windows storage driver.\r\n"
    );
  if (mBs != NULL) {
    for (UINTN Second = 0; Second < 90; ++Second) {
      mBs->Stall (1000000);
    }
  }

  return EFI_SUCCESS;  // Diagnostic reports failure via framebuffer text.
}
