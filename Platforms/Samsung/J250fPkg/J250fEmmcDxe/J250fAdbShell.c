/* SPDX-License-Identifier: GPL-2.0-only */
#include "J250fAdbShell.h"
#include "J250fEmmcDebug.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Uefi.h>
STATIC EFI_HANDLE mParent, mLastImage;
typedef struct {
  EFI_HANDLE Handle;
  VOID *Options;
} RESIDENT;
STATIC RESIDENT mResident[16];
STATIC VOID Reap(VOID) {
  UINTN I;
  EFI_LOADED_IMAGE_PROTOCOL *P;
  for (I = 0; I < 16; I++)
    if (mResident[I].Handle &&
        EFI_ERROR(gBS->HandleProtocol(
            mResident[I].Handle, &gEfiLoadedImageProtocolGuid, (VOID **)&P))) {
      gBS->FreePool(mResident[I].Options);
      mResident[I].Handle = NULL;
      mResident[I].Options = NULL;
    }
}
STATIC UINT8 *mFile;
STATIC UINTN mFileSize;
STATIC CHAR8 mName[128];
STATIC BOOLEAN mComplete, mExit, mReboot;
extern EFI_STATUS EFIAPI J250fUsbDiagCommand(UINT16, UINT16, UINT16, VOID *,
                                             UINTN *);
extern EFI_STATUS J250fEmmcGetCid(UINT32 *Cid);
extern UINTN EFIAPI J250fGopDebugCommand(BOOLEAN RunTest, CHAR8 *Out, UINTN Cap);
extern UINTN EFIAPI J250fGopRepairCommand(CHAR8 *Out, UINTN Cap);
extern UINTN EFIAPI J250fDeepDebugCommand(CONST CHAR8 *Args, CHAR8 *Out, UINTN Cap);
extern BOOLEAN EFIAPI J250fWpCommand(CONST CHAR8 *Cmd, CHAR8 *Out, UINTN Cap, UINTN *Used);
extern BOOLEAN EFIAPI J250fExt4Command(CONST CHAR8 *Cmd, CHAR8 *Out, UINTN Cap, UINTN *Used);
STATIC BOOLEAN Eq(CONST CHAR8 *A, CONST CHAR8 *B) {
  return AsciiStrCmp(A, B) == 0;
}
STATIC int Begin(void *Ctx, const char *Name) {
  UINTN N = AsciiStrLen(Name), I;
  (VOID) Ctx;
  if (N < 6 || N >= sizeof(mName) || AsciiStrnCmp(Name, "/ram/", 5))
    return -1;
  for (I = 5; I < N; I++)
    if (Name[I] == '/' || Name[I] == '\\' || Name[I] == ',' || Name[I] < 32)
      return -1;
  if (mFile) {
    gBS->FreePool(mFile);
    mFile = NULL;
  }
  mComplete = FALSE;
  mFileSize = 0;
  if (EFI_ERROR(
          gBS->AllocatePool(EfiLoaderData, JADB_FILE_MAX, (VOID **)&mFile)))
    return -1;
  CopyMem(mName, Name, N + 1);
  return 0;
}
STATIC int Data(void *Ctx, const uint8_t *P, size_t N) {
  (VOID) Ctx;
  if (!mFile || N > JADB_FILE_MAX - mFileSize)
    return -1;
  CopyMem(mFile + mFileSize, P, N);
  mFileSize += N;
  return 0;
}
STATIC int Done(void *Ctx) {
  (VOID) Ctx;
  if (!mFile || !mFileSize)
    return -1;
  mComplete = TRUE;
  return 0;
}
STATIC int Stat(void *Ctx, const char *Name, uint32_t *Mode, uint32_t *Size) {
  (VOID) Ctx;
  *Mode = *Size = 0;
  if (Eq(Name, "/ram") || Eq(Name, "/ram/")) {
    *Mode = 0040777;
    return 0;
  }
  if (mComplete && Eq(Name, mName)) {
    *Mode = 0100644;
    *Size = (UINT32)mFileSize;
    return 0;
  }
  return -1;
}
STATIC UINTN RunEfi(CONST CHAR8 *Args, CHAR8 *Out, UINTN Cap) {
  CHAR8 Name[128];
  CHAR16 Options[512];
  UINTN N = 0, I = 0, Slot;
  EFI_STATUS S;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  Reap();
  for (Slot = 0; Slot < 16 && mResident[Slot].Handle; Slot++)
    ;
  if (Slot == 16)
    return AsciiSPrint(
        Out, Cap, "Resident image limit reached; unload a driver first.\r\n");
  if (AsciiStrLen(Args) >= 512)
    return AsciiSPrint(Out, Cap, "EFI arguments exceed 511 characters.\r\n");
  while (Args[N] && Args[N] != ' ' && N + 1 < sizeof(Name)) {
    Name[N] = Args[N];
    N++;
  }
  Name[N] = 0;
  if (!mComplete || !Eq(Name, mName))
    return AsciiSPrint(Out, Cap, "No completed RAM upload with that name.\r\n");
  mLastImage = NULL;
  S = gBS->LoadImage(FALSE, mParent, NULL, mFile, mFileSize, &mLastImage);
  if (EFI_ERROR(S)) {
    if (mLastImage)
      gBS->UnloadImage(mLastImage);
    mLastImage = NULL;
    return AsciiSPrint(Out, Cap, "LoadImage: %r (policy denial, malformed image, or unsupported format; inspect status)\r\n",
                       S);
  }
  S = gBS->HandleProtocol(mLastImage, &gEfiLoadedImageProtocolGuid,
                          (VOID **)&Loaded);
  if (EFI_ERROR(S)) {
    gBS->UnloadImage(mLastImage);
    mLastImage = NULL;
    return AsciiSPrint(Out, Cap, "LoadedImage protocol: %r\r\n", S);
  }
  /* Options must remain live throughout StartImage. Successful drivers may
   * retain their options, so use an owned pool until unload/firmware exit. */
  while (Args[I] && I + 1 < sizeof(Options) / sizeof(Options[0])) {
    Options[I] = (CHAR16)(UINT8)Args[I];
    I++;
  }
  Options[I] = 0;
  {
    VOID *Copy = NULL;
    S = gBS->AllocatePool(EfiLoaderData, (I + 1) * sizeof(CHAR16), &Copy);
    if (EFI_ERROR(S)) {
      gBS->UnloadImage(mLastImage);
      mLastImage = NULL;
      return AsciiSPrint(Out, Cap, "Load options: %r\r\n", S);
    }
    CopyMem(Copy, Options, (I + 1) * sizeof(CHAR16));
    Loaded->LoadOptions = Copy;
    Loaded->LoadOptionsSize = (UINT32)((I + 1) * sizeof(CHAR16));
    S = gBS->StartImage(mLastImage, NULL, NULL);
    if (!EFI_ERROR(gBS->HandleProtocol(mLastImage, &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&Loaded))) {
      mResident[Slot].Handle = mLastImage;
      mResident[Slot].Options = Copy;
    } else {
      gBS->FreePool(Copy);
      mLastImage = NULL;
    }
  }
  return AsciiSPrint(Out, Cap, "StartImage returned: %r\r\n", S);
}
STATIC size_t Execute(void *Ctx, const char *Cmd, char *Out, size_t Cap) {
  UINT32 V[16], Cid[4];
  UINTN N = sizeof(V);
  EFI_STATUS S;
  (VOID) Ctx;
  if (!*Cmd)
    return 0;
  if (Eq(Cmd, "help"))
    return AsciiSPrint(
        Out, Cap,
        "help\r\nemmc status | registers | init | cmd2 | dump-cid | set-clock "
        "144000|400000\r\nemmc capabilities | map | read <name> | snapshot | trace [seq]\r\n"
        "emmc clock | identify | config show|reset|set <target> <mask> <value>\r\n"
        "gop status | gop repair | gop test\r\n"
        "dbg help | dbg <diagnostic> (generic firmware debugger)\r\n"
        "ext4 volumes | ls <index> <path> | cat <index> <path>\r\n"
        "wp status | mount | probe | boot CONFIRM (read-only backing)\r\n"
        "efi list | run /ram/name.efi [args] | unload [slot]\r\nfirmware "
        "exit\r\nreboot\r\n"
        "Upload: adb push tool.efi /ram/tool.efi (one RAM slot, max 16MiB).\r\n"
        "cmd2/identify stop after CMD2; init continues through capacity discovery.\r\n");
  { UINTN Used;
    if(J250fEmmcDebugCommand(Cmd,Out,Cap,&Used))return Used;
    if(J250fExt4Command(Cmd,Out,Cap,&Used))return Used;
    if(J250fWpCommand(Cmd,Out,Cap,&Used))return Used;
  }
  if (Eq(Cmd, "emmc status") || Eq(Cmd, "emmc registers") ||
      Eq(Cmd, "emmc init") || Eq(Cmd, "emmc cmd2")) {
    BOOLEAN Init = Eq(Cmd, "emmc init") || Eq(Cmd, "emmc cmd2");
    S = J250fUsbDiagCommand(Init ? 0x72 : 0x71, Init ? 1 : 0, 0, V, &N);
    if (EFI_ERROR(S))
      return AsciiSPrint(Out, Cap, "Diagnostic: %r\r\n", S);
    return AsciiSPrint(Out, Cap,
                       "result=%d host=%u card=%u hz=%u CMD=%u\r\nIRQ=%08x "
                       "R1=%08x STATE=%08x\r\nVENDOR=%08x DLL=%08x "
                       "CLK=%04x\r\nLIVE IRQ=%08x STATE=%08x VENDOR=%08x\r\n",
                       (INT32)V[2], V[3], V[4], V[5], V[6], V[7], V[8], V[9],
                       V[10], V[11], V[12], V[13], V[14], V[15]);
  }
  if (Eq(Cmd, "emmc dump-cid")) {
    S = J250fEmmcGetCid(Cid);
    if (EFI_ERROR(S))
      return AsciiSPrint(Out, Cap, "CID unavailable: %r\r\n", S);
    return AsciiSPrint(Out, Cap, "CID=%08x %08x %08x %08x\r\n", Cid[0], Cid[1],
                       Cid[2], Cid[3]);
  }
  if (Eq(Cmd, "gop status") || Eq(Cmd, "gop test"))
    return J250fGopDebugCommand(Eq(Cmd, "gop test"), Out, Cap);
  if (Eq(Cmd, "gop repair"))
    return J250fGopRepairCommand(Out, Cap);
  if (Eq(Cmd, "dbg") || Eq(Cmd, "dbg help"))
    return J250fDeepDebugCommand("help", Out, Cap);
  if (AsciiStrnCmp(Cmd, "dbg ", 4) == 0)
    return J250fDeepDebugCommand(Cmd + 4, Out, Cap);
  if (Eq(Cmd, "efi list")) {
    UINTN I, Used = AsciiSPrint(Out, Cap, "%a %u bytes complete=%u\r\n", mName,
                                (UINT32)mFileSize, (UINT32)mComplete);
    Reap();
    for (I = 0; I < 16 && Used < Cap; I++)
      if (mResident[I].Handle)
        Used += AsciiSPrint(Out + Used, Cap - Used, "resident slot %u\r\n",
                            (UINT32)I);
    return Used;
  }
  if (AsciiStrnCmp(Cmd, "efi run ", 8) == 0)
    return RunEfi(Cmd + 8, Out, Cap);
  if (Eq(Cmd, "efi unload") || AsciiStrnCmp(Cmd, "efi unload ", 11) == 0) {
    EFI_HANDLE Handle = mLastImage;
    if (Cmd[10]) {
      UINTN Slot = 0, I = 11;
      if (!Cmd[I])
        return AsciiSPrint(Out, Cap, "Specify a slot from efi list.\r\n");
      for (; Cmd[I]; I++) {
        if (Cmd[I] < '0' || Cmd[I] > '9' || Slot > 15)
          return AsciiSPrint(Out, Cap, "Invalid resident slot.\r\n");
        Slot = Slot * 10 + (UINTN)(Cmd[I] - '0');
      }
      if (Slot >= 16)
        return AsciiSPrint(Out, Cap, "Invalid resident slot.\r\n");
      Handle = mResident[Slot].Handle;
    }
    S = Handle ? gBS->UnloadImage(Handle) : EFI_NOT_FOUND;
    if (!EFI_ERROR(S)) {
      if (Handle == mLastImage)
        mLastImage = NULL;
      Reap();
    }
    return AsciiSPrint(Out, Cap, "UnloadImage: %r\r\n", S);
  }
  if (Eq(Cmd, "firmware exit")) {
    mExit = TRUE;
    return AsciiSPrint(
        Out, Cap, "Leaving firmware debug session. USB will disconnect.\r\n");
  }
  if (Eq(Cmd, "reboot")) {
    mExit = mReboot = TRUE;
    return AsciiSPrint(Out, Cap, "Rebooting.\r\n");
  }
  return AsciiSPrint(Out, Cap,
                     "Unknown firmware command: %a\r\nUse help, or upload and "
                     "run an ARM32 .efi application.\r\n",
                     Cmd);
}
VOID J250fAdbShellInit(EFI_HANDLE Parent) { mParent = Parent; }
VOID J250fAdbShellOps(jadb_ops *Ops) {
  ZeroMem(Ops, sizeof(*Ops));
  Ops->execute = Execute;
  Ops->begin = Begin;
  Ops->data = Data;
  Ops->done = Done;
  Ops->stat = Stat;
}
BOOLEAN J250fAdbShellExitRequested(VOID) { return mExit; }
VOID J250fAdbShellFinish(VOID) {
  if (mFile) {
    gBS->FreePool(mFile);
    mFile = NULL;
  }
  if (mReboot)
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
}
