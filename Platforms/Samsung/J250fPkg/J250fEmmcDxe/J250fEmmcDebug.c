/* SPDX-License-Identifier: GPL-2.0-only
 * Named MMIO inspection and reversible, clock-gated diagnostic profiles.
 * No command-register writes, power-rail changes or CRC bypass interface.
 */
#include "J250fEmmcDebug.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
STATIC j250f_emmc *mDebugHost;
typedef struct {
  CONST CHAR8 *Region, *Name;
  UINT32 Base, Offset, Width;
} REG;
#define H(N, O, W) {"hc", N, J250F_HC_PHYS, O, W}
#define C(N, O) {"core", N, J250F_CORE_PHYS, O, 32}
#define G(N, O) {"gcc", N, J250F_GCC_PHYS, O, 32}
STATIC CONST REG mRegs[] = {
    H("ARG", 0x08, 32),
    H("TRANSFER", 0x0c, 16),
    H("COMMAND", 0x0e, 16),
    H("RESP0", 0x10, 32),
    H("RESP1", 0x14, 32),
    H("RESP2", 0x18, 32),
    H("RESP3", 0x1c, 32),
    H("PRESENT", 0x24, 32),
    H("HOST_CONTROL", 0x28, 8),
    H("POWER", 0x29, 8),
    H("CLOCK", 0x2c, 16),
    H("TIMEOUT", 0x2e, 8),
    H("RESET", 0x2f, 8),
    H("IRQ", 0x30, 32),
    H("IRQ_ENABLE", 0x34, 32),
    H("SIGNAL_ENABLE", 0x38, 32),
    H("HC2", 0x3e, 16),
    H("CAPS", 0x40, 32),
    H("CAPS1", 0x44, 32),
    H("VERSION", 0xfe, 16),
    H("DLL", 0x100, 32),
    H("VENDOR", 0x10c, 32),
    H("VENDOR_CAPS", 0x11c, 32),
    C("CORE_VERSION", 0x50),
    C("CORE_MODE", 0x78),
    C("PWR_STATUS", 0xdc),
    C("PWR_MASK", 0xe0),
    C("PWR_CTL", 0xe8),
    G("GCC_CMD", 0x42004),
    G("GCC_CFG", 0x42008),
    G("GCC_M", 0x4200c),
    G("GCC_N", 0x42010),
    G("GCC_D", 0x42014),
    G("GCC_APPS", 0x42018),
    G("GCC_AHB", 0x4201c),
    {"tlmm", "SDC1_PADS", J250F_TLMM_PHYS, 0x10a000, 32}};
typedef struct {
  CONST CHAR8 *Name;
  UINT32 Address, Width, Allowed, Baseline, Mask, Value;
} PROFILE;
STATIC PROFILE mProfiles[] = {
    {"vendor", J250F_HC_PHYS + 0x10c, 32, 0x302, 0, 0, 0},
    {"dll", J250F_HC_PHYS + 0x100, 32, 0x60ff0000, 0, 0, 0},
    {"hc2", J250F_HC_PHYS + 0x3e, 16, 7, 0, 0, 0},
    {"tlmm", J250F_TLMM_PHYS + 0x10a000, 32, 0x1ffff, 0, 0, 0}};
STATIC BOOLEAN mForceBaseline;
STATIC INT32 mLastApply;
STATIC BOOLEAN Eq(CONST CHAR8 *A, CONST CHAR8 *B) {
  return AsciiStrCmp(A, B) == 0;
}
STATIC UINT32 Read(UINT32 A, UINT32 W) {
  if (W == 8)
    return MmioRead8(A);
  if (W == 16)
    return MmioRead16(A);
  return MmioRead32(A);
}
STATIC VOID Write(UINT32 A, UINT32 W, UINT32 V) {
  if (W == 16)
    MmioWrite16(A, (UINT16)V);
  else
    MmioWrite32(A, V);
}
STATIC int Apply(void *Context) {
  UINT32 Old[4], Clock, V;
  UINTN I;
  BOOLEAN Active = mForceBaseline;
  int Result = J250F_OK;
  (VOID) Context;
  for (I = 0; I < 4; I++)
    if (mProfiles[I].Mask)
      Active = TRUE;
  if (!Active)
    return 0;
  if (mDebugHost && mDebugHost->card_ready && !mForceBaseline)
    return J250F_OK;
  if (!mDebugHost || !mDebugHost->host_ready || mDebugHost->card_ready ||
      (MmioRead32(J250F_HC_PHYS + 0x24) & 3))
    return J250F_E_NOT_READY;
  Clock = MmioRead16(J250F_HC_PHYS + 0x2c);
  MmioWrite16(J250F_HC_PHYS + 0x2c, (UINT16)(Clock & ~4u));
  MemoryFence();
  for (I = 0; I < 4; I++)
    Old[I] = Read(mProfiles[I].Address, mProfiles[I].Width);
  for (I = 0; I < 4; I++) {
    PROFILE *P = &mProfiles[I];
    V = (Old[I] & ~P->Allowed) | (P->Baseline & P->Allowed);
    V = (V & ~P->Mask) | P->Value;
    Write(P->Address, P->Width, V);
    MemoryFence();
    if ((Read(P->Address, P->Width) & P->Allowed) != (V & P->Allowed)) {
      Result = J250F_E_IO;
      break;
    }
  }
  if (Result)
    for (I = 0; I < 4; I++) {
      PROFILE *P = &mProfiles[I];
      V = Read(P->Address, P->Width);
      Write(P->Address, P->Width, (V & ~P->Allowed) | (Old[I] & P->Allowed));
    }
  MemoryFence();
  MmioWrite16(J250F_HC_PHYS + 0x2c, (UINT16)Clock);
  MemoryFence();
  mLastApply = Result;
  return Result;
}
STATIC VOID Capture(void *Context, j250f_trace *T) {
  (VOID) Context;
  T->gcc_cfg = MmioRead32(J250F_GCC_PHYS + 0x42008);
  T->gcc_m = MmioRead32(J250F_GCC_PHYS + 0x4200c);
  T->gcc_n = MmioRead32(J250F_GCC_PHYS + 0x42010);
  T->gcc_d = MmioRead32(J250F_GCC_PHYS + 0x42014);
  T->tlmm = MmioRead32(J250F_TLMM_PHYS + 0x10a000);
}
VOID J250fEmmcDebugAttach(j250f_emmc *Host) {
  UINTN I;
  mDebugHost = Host;
  for (I = 0; I < 4; I++)
    mProfiles[I].Baseline = Read(mProfiles[I].Address, mProfiles[I].Width);
  Host->diagnostic_apply = Apply;
  Host->diagnostic_capture = Capture;
}
STATIC BOOLEAN Number(CONST CHAR8 *S, UINT32 *V) {
  UINT32 B = 10, D, N = 0;
  if (!*S)
    return FALSE;
  if (S[0] == '0' && (S[1] == 'x' || S[1] == 'X')) {
    B = 16;
    S += 2;
    if (!*S)
      return FALSE;
  }
  while (*S) {
    if (*S >= '0' && *S <= '9')
      D = *S - '0';
    else if (*S >= 'a' && *S <= 'f')
      D = *S - 'a' + 10;
    else if (*S >= 'A' && *S <= 'F')
      D = *S - 'A' + 10;
    else
      return FALSE;
    if (D >= B || N > (MAX_UINT32 - D) / B)
      return FALSE;
    N = N * B + D;
    S++;
  }
  *V = N;
  return TRUE;
}
STATIC UINTN Trace(CONST j250f_trace *T, CHAR8 *O, UINTN C) {
  return AsciiSPrint(
      O, C,
      "PRE-RECOVERY seq=%u CMD=%u arg=%08x sent_flags=%04x command_result=%d "
      "requested_hz=%u\r\nIRQ=%08x PRESENT=%08x HC2=%04x CLOCK=%04x "
      "VENDOR=%08x DLL=%08x\r\nRAW_RESPONSE=%08x %08x %08x %08x (untrusted on "
      "CRC error)\r\nCORE_MODE=%08x PWR_STATUS=%08x PWR_MASK=%08x "
      "PWR_CTL=%08x\r\nGCC_CFG=%08x M=%08x N=%08x D=%08x TLMM=%08x\r\n",
      T->sequence, T->command, T->argument, T->flags, (INT32)T->result,
      T->frequency_hz, T->interrupt, T->present, T->hc2, T->clock, T->vendor,
      T->dll, T->response[0], T->response[1], T->response[2], T->response[3],
      T->core_mode, T->power_status, T->power_mask, T->power_control,
      T->gcc_cfg, T->gcc_m, T->gcc_n, T->gcc_d, T->tlmm);
}
STATIC UINTN Config(CHAR8 *O, UINTN C) {
  UINTN I, N = AsciiSPrint(O, C,
                           "profile_apply_result=%d requested_hz=%u "
                           "attempts=%u/32 CID_valid=%u\r\n",
                           mLastApply, mDebugHost->frequency_hz,
                           J250fEmmcAttempts(), (UINT32)mDebugHost->cid_valid);
  for (I = 0; I < 4 && N < C; I++) {
    PROFILE *P = &mProfiles[I];
    N += AsciiSPrint(
        O + N, C - N,
        "%a allowed=%08x baseline=%08x mask=%08x value=%08x live=%08x\r\n",
        P->Name, P->Allowed, P->Baseline, P->Mask, P->Value,
        Read(P->Address, P->Width));
  }
  return N;
}
BOOLEAN J250fEmmcDebugCommand(CONST CHAR8 *Command, CHAR8 *Out, UINTN Cap,
                              UINTN *Length) {
  CHAR8 Text[256], *A[9], *P;
  UINTN Count = 0, I, N = 0;
  UINT32 X, Y, W;
  EFI_STATUS S;
  if (AsciiStrnCmp(Command, "emmc ", 5))
    return FALSE;
  if (AsciiStrLen(Command) >= sizeof(Text)) {
    *Length = AsciiSPrint(Out, Cap, "Command too long.\r\n");
    return TRUE;
  }
  CopyMem(Text, Command, AsciiStrLen(Command) + 1);
  P = Text;
  while (*P && Count < 9) {
    while (*P == ' ')
      P++;
    if (!*P)
      break;
    A[Count++] = P;
    while (*P && *P != ' ')
      P++;
    if (*P)
      *P++ = 0;
  }
  if (Count < 2)
    return FALSE;
  if (Eq(A[1], "capabilities")) {
    N = AsciiSPrint(
        Out, Cap,
        "J250F-ADB-DIAG/2: map, read, registers, snapshot, trace [seq], clock, "
        "set-clock 144000|400000, config show|reset|set target mask value, "
        "identify, init.\r\nProfiles persist before commands. CRC checks stay "
        "enabled.\r\n");
    goto Done;
  }
  if (!mDebugHost) {
    N = AsciiSPrint(Out, Cap, "Debug host not attached.\r\n");
    goto Done;
  }
  if (Eq(A[1], "map") || Eq(A[1], "registers") || Eq(A[1], "snapshot")) {
    BOOLEAN Map = Eq(A[1], "map");
    N = AsciiSPrint(Out, Cap, "J250F-ADB-DIAG/2 %a\r\n",
                    Map ? "REFERENCE MAP" : "LIVE REGISTERS (not pre-reset)");
    for (I = 0; I < sizeof(mRegs) / sizeof(mRegs[0]) && N < Cap; I++) {
      CONST REG *R = &mRegs[I];
      N += AsciiSPrint(Out + N, Cap - N, "%a %a+0x%x @%08x /%u", R->Name,
                       R->Region, R->Offset, R->Base + R->Offset, R->Width);
      if (!Map && N < Cap)
        N += AsciiSPrint(Out + N, Cap - N, " =%08x",
                         Read(R->Base + R->Offset, R->Width));
      if (N < Cap)
        N += AsciiSPrint(Out + N, Cap - N, "\r\n");
    }
    if (Eq(A[1], "snapshot") && N < Cap) {
      N += Config(Out + N, Cap - N);
      if (mDebugHost->trace_sequence && N < Cap)
        N += Trace(&mDebugHost->trace[(mDebugHost->trace_sequence - 1) %
                                      J250F_TRACE_DEPTH],
                   Out + N, Cap - N);
    }
    goto Done;
  }
  if (Eq(A[1], "read")) {
    CONST REG *R = NULL;
    for (I = 0; I < sizeof(mRegs) / sizeof(mRegs[0]); I++) {
      if (Count == 3 && Eq(A[2], mRegs[I].Name))
        R = &mRegs[I];
      if (Count == 5 && Eq(A[2], mRegs[I].Region) && Number(A[3], &X) &&
          Number(A[4], &W) && X == mRegs[I].Offset && W == mRegs[I].Width)
        R = &mRegs[I];
    }
    if (R)
      N = AsciiSPrint(Out, Cap, "LIVE %a %a+0x%x @%08x width=%u value=%08x\r\n",
                      R->Name, R->Region, R->Offset, R->Base + R->Offset,
                      R->Width, Read(R->Base + R->Offset, R->Width));
    else
      N = AsciiSPrint(Out, Cap,
                      "Unknown register/width. Use emmc map; emmc read HC2; "
                      "emmc read hc 0x3e 16.\r\n");
    goto Done;
  }
  if (Eq(A[1], "trace")) {
    if (Count == 3 && Number(A[2], &X) && X &&
        X <= mDebugHost->trace_sequence &&
        mDebugHost->trace_sequence - X < J250F_TRACE_DEPTH)
      N = Trace(&mDebugHost->trace[(X - 1) % J250F_TRACE_DEPTH], Out, Cap);
    else if (Count == 2) {
      UINT32 Start = mDebugHost->trace_sequence >= J250F_TRACE_DEPTH
                         ? mDebugHost->trace_sequence - J250F_TRACE_DEPTH + 1
                         : 1;
      N = AsciiSPrint(Out, Cap,
                      "Command-completion snapshots before recovery; use emmc "
                      "trace <seq>.\r\n");
      for (X = Start; X <= mDebugHost->trace_sequence && N < Cap; X++) {
        j250f_trace *T = &mDebugHost->trace[(X - 1) % J250F_TRACE_DEPTH];
        N += AsciiSPrint(
            Out + N, Cap - N,
            "seq=%u CMD=%u result=%d IRQ=%08x HC2=%04x CLK=%04x\r\n", X,
            T->command, (INT32)T->result, T->interrupt, T->hc2, T->clock);
      }
    } else
      N = AsciiSPrint(
          Out, Cap,
          "Trace sequence unavailable (last 16 commands retained).\r\n");
    goto Done;
  }
  if (Eq(A[1], "clock")) {
    UINT32 Cfg = MmioRead32(J250F_GCC_PHYS + 0x42008),
           M = MmioRead32(J250F_GCC_PHYS + 0x4200c) & 255,
           RawN = MmioRead32(J250F_GCC_PHYS + 0x42010),
           Den = ((~RawN) & 255) + M;
    UINT32 Clk = MmioRead16(J250F_HC_PHYS + 0x2c),
           Div = ((Clk >> 8) & 255) | ((Clk & 0xc0) << 2);
    UINT64 Rate = 0;
    if (((Cfg >> 8) & 7) == 0) {
      Rate = 38400000u / (UINT64)((Cfg & 31) + 1);
      if (((Cfg >> 12) & 3) == 2) {
        if (Den)
          Rate = Rate * M / Den;
        else
          Rate = 0;
      }
      if (Div)
        Rate /= 2u * Div;
    }
    N = AsciiSPrint(Out, Cap,
                    "requested_hz=%u decoded_hz_assuming_19.2MHz_XO=%Lu (not "
                    "measured)\r\nGCC_CFG=%08x M=%08x N=%08x D=%08x CMD=%08x "
                    "APPS=%08x AHB=%08x HC_CLOCK=%04x\r\n",
                    mDebugHost->frequency_hz, Rate, Cfg, M, RawN,
                    MmioRead32(J250F_GCC_PHYS + 0x42014),
                    MmioRead32(J250F_GCC_PHYS + 0x42004),
                    MmioRead32(J250F_GCC_PHYS + 0x42018),
                    MmioRead32(J250F_GCC_PHYS + 0x4201c), Clk);
    goto Done;
  }
  if (Eq(A[1], "set-clock")) {
    if (Count != 3 || !Number(A[2], &X) || (X != 144000 && X != 400000)) {
      N = AsciiSPrint(Out, Cap,
                      "Supported identification rates: 144000, 400000 Hz.\r\n");
      goto Done;
    }
    S = J250fEmmcSetClock(X);
    if (!EFI_ERROR(S) && Apply(NULL))
      S = EFI_DEVICE_ERROR;
    N = AsciiSPrint(Out, Cap,
                    "Set-clock: %r; use emmc clock and emmc snapshot for "
                    "actual readback.\r\n",
                    S);
    goto Done;
  }
  if (Eq(A[1], "init")) {
    S = J250fEmmcTry(FALSE);
    N = AsciiSPrint(Out, Cap, "Card initialization: %r CID_valid=%u card_ready=%u; interactive retries do not publish Block I/O.\r\n", S, (UINT32)mDebugHost->cid_valid, (UINT32)mDebugHost->card_ready);
    goto Done;
  }
  if (Eq(A[1], "identify") || Eq(A[1], "cmd2")) {
    S = J250fEmmcTry(TRUE);
    N = AsciiSPrint(Out, Cap,
                    "CMD0/CMD1/CMD2 identification: %r CID_valid=%u\r\n", S,
                    (UINT32)mDebugHost->cid_valid);
    if (mDebugHost->trace_sequence && N < Cap)
      N += Trace(&mDebugHost->trace[(mDebugHost->trace_sequence - 1) %
                                    J250F_TRACE_DEPTH],
                 Out + N, Cap - N);
    goto Done;
  }
  if (Eq(A[1], "config")) {
    if (Count == 2 || (Count == 3 && Eq(A[2], "show"))) {
      N = Config(Out, Cap);
      goto Done;
    }
    if (!mDebugHost->host_ready || mDebugHost->card_ready ||
        (MmioRead32(J250F_HC_PHYS + 0x24) & 3)) {
      N = AsciiSPrint(Out, Cap,
                      "Settings rejected: host unavailable, busy, or card "
                      "already initialized.\r\n");
      goto Done;
    }
    {
      PROFILE Saved[4];
      int R;
      CopyMem(Saved, mProfiles, sizeof(Saved));
      if (Count == 3 && Eq(A[2], "reset")) {
        for (I = 0; I < 4; I++)
          mProfiles[I].Mask = mProfiles[I].Value = 0;
      } else if (Count == 6 && Eq(A[2], "set") && Number(A[4], &X) &&
                 Number(A[5], &Y)) {
        for (I = 0; I < 4; I++)
          if (Eq(A[3], mProfiles[I].Name))
            break;
        if (I == 4 || !X || (X & ~mProfiles[I].Allowed) || (Y & ~X)) {
          N = AsciiSPrint(Out, Cap,
                          "Rejected target/mask/value. Use config show for "
                          "allowed bits; value must fit mask.\r\n");
          goto Done;
        }
        mProfiles[I].Mask = X;
        mProfiles[I].Value = Y;
      } else {
        N = AsciiSPrint(Out, Cap,
                        "Usage: emmc config show|reset|set vendor|dll|hc2|tlmm "
                        "<mask> <value>\r\n");
        goto Done;
      }
      mForceBaseline = TRUE;
      R = Apply(NULL);
      mForceBaseline = FALSE;
      if (R)
        CopyMem(mProfiles, Saved, sizeof(Saved));
      N = AsciiSPrint(
          Out, Cap,
          "Config apply result=%d; masks persist before each command.\r\n", R);
      if (N < Cap)
        N += Config(Out + N, Cap - N);
    }
    goto Done;
  }
  return FALSE;
Done:
  *Length = N;
  return TRUE;
}
