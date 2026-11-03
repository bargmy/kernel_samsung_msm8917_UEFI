/** Firmware debug + EFI console tee. SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/J250fTextLog.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextOut.h>
#include "j250f_log_format.h"

extern EFI_STATUS EFIAPI J250fLogWriteRecord(UINT32 Index, CONST VOID *Data);
STATIC J250F_TEXT_LOG *mLog;
STATIC BOOLEAN mFlushing;
STATIC UINT32 mSector;
STATIC UINT32 mReportedDrops;
STATIC UINTN mFlushCpu;
STATIC EFI_EVENT mTimer, mReady, mExit;
#define LOG_CONSOLES 8U
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mOutputs[LOG_CONSOLES];
STATIC EFI_TEXT_STRING mOriginal[LOG_CONSOLES];
STATIC UINTN mOutputCount;

STATIC VOID QueueBytes(CONST UINT8 *Bytes, UINTN Length) {
  BOOLEAN Enabled = ArmGetInterruptState();
  if (Enabled) ArmDisableInterrupts();
  if (mLog->Head >= J250F_TEXT_LOG_BYTES || mLog->Tail >= J250F_TEXT_LOG_BYTES) {
    mLog->Stopped = TRUE;
    if (Enabled) ArmEnableInterrupts();
    return;
  }
  J250fTextAppend(mLog, Bytes, Length);
  if (Enabled) ArmEnableInterrupts();
}

STATIC VOID EFIAPI Flush(VOID) {
  EFI_TPL OldTpl;
  UINT8 Record[512];
  UINT32 Tail, End, Count;
  EFI_STATUS Status;
  BOOLEAN Enabled;
  if (!mLog || mLog->Stopped || ArmReadMpidr() != mFlushCpu ||
      mFlushing || !ArmGetInterruptState()) return;
  OldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if (OldTpl > TPL_CALLBACK || mFlushing || mLog->Stopped) {
    gBS->RestoreTPL(OldTpl);
    return;
  }
  /* RestoreTPL may dispatch our timer. Publish the recursion guard FIRST. */
  mFlushing = TRUE;
  gBS->RestoreTPL(OldTpl);
  /* Never do storage I/O in an interrupt, notification, or exception path. */
  if (mLog->Head >= J250F_TEXT_LOG_BYTES || mLog->Tail >= J250F_TEXT_LOG_BYTES) {
    mLog->Stopped = TRUE;
    mFlushing = FALSE;
    return;
  }
  while (!mLog->Stopped && mLog->Tail != mLog->Head) {
    if (mSector >= 2048) { mLog->Stopped = TRUE; break; }
    SetMem(Record, sizeof(Record), ' ');
    Tail = mLog->Tail;
    Enabled = ArmGetInterruptState();
    if (Enabled) ArmDisableInterrupts();
    End = mLog->Head;
    MemoryFence();
    if (Enabled) ArmEnableInterrupts();
    Count = 0;
    while (Tail != End && Count < 480) {
      Record[32 + Count++] = mLog->Data[Tail];
      Tail = (Tail + 1) % J250F_TEXT_LOG_BYTES;
    }
    j250f_log_seal(Record, mSector, Count);
    Status = J250fLogWriteRecord(mSector, Record);
    if (Status == EFI_NOT_READY) break; /* backend was already in a read */
    if (EFI_ERROR(Status)) {
      mLog->Stopped = TRUE; /* uncertain write: never retry or advance */
      break;
    }
    MemoryFence();
    mLog->Tail = Tail;
    ++mSector;
  }
  mFlushing = FALSE;
}

STATIC EFI_STATUS EFIAPI TeeOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String) {
  UINTN Which;
  EFI_STATUS Status;
  BOOLEAN Previous;
  UINTN PreviousCpu;
  for (Which = 0; Which < mOutputCount; ++Which)
    if (This == mOutputs[Which]) break;
  if (Which == mOutputCount || !mOriginal[Which]) return EFI_INVALID_PARAMETER;
  if (String && !mLog->Stopped) {
    CONST CHAR16 *Cursor = String;
    while (*Cursor) {
      UINT32 Ch = *Cursor++;
      UINT8 Utf8[4];
      UINTN Count;
      if (Ch >= 0xd800 && Ch <= 0xdbff && *Cursor >= 0xdc00 && *Cursor <= 0xdfff)
        Ch = 0x10000 + ((Ch - 0xd800) << 10) + (*Cursor++ - 0xdc00);
      else if (Ch >= 0xd800 && Ch <= 0xdfff) Ch = 0xfffd;
      if (Ch < 0x80) { Utf8[0] = (UINT8)Ch; Count = 1; }
      else if (Ch < 0x800) {
        Utf8[0] = (UINT8)(0xc0 | (Ch >> 6)); Utf8[1] = (UINT8)(0x80 | (Ch & 63)); Count = 2;
      } else if (Ch < 0x10000) {
        Utf8[0] = (UINT8)(0xe0 | (Ch >> 12)); Utf8[1] = (UINT8)(0x80 | ((Ch >> 6) & 63));
        Utf8[2] = (UINT8)(0x80 | (Ch & 63)); Count = 3;
      } else {
        Utf8[0] = (UINT8)(0xf0 | (Ch >> 18)); Utf8[1] = (UINT8)(0x80 | ((Ch >> 12) & 63));
        Utf8[2] = (UINT8)(0x80 | ((Ch >> 6) & 63)); Utf8[3] = (UINT8)(0x80 | (Ch & 63)); Count = 4;
      }
      QueueBytes(Utf8, Count);
    }
  }
  /* Avoid duplicating console text if the original also uses SerialPortLib. */
  Previous = mLog->SuppressSerial;
  PreviousCpu = mLog->SuppressCpu;
  mLog->SuppressCpu = ArmReadMpidr();
  mLog->SuppressSerial = TRUE;
  Status = mOriginal[Which](This, String);
  mLog->SuppressSerial = Previous;
  mLog->SuppressCpu = PreviousCpu;
  Flush();
  return Status;
}

STATIC VOID HookConsole(VOID) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Outputs[2];
  UINTN I;
  Outputs[0] = gST->ConOut; Outputs[1] = gST->StdErr;
  for (I = 0; I < 2; ++I) {
    UINTN Existing;
    if (!Outputs[I] || !Outputs[I]->OutputString || Outputs[I]->OutputString == TeeOutput)
      continue;
    for (Existing = 0; Existing < mOutputCount; ++Existing)
      if (mOutputs[Existing] == Outputs[I]) break;
    if (Existing == LOG_CONSOLES) continue;
    if (Existing == mOutputCount) ++mOutputCount;
    /* Never dereference an old, disconnected protocol just to restore it. */
    mOutputs[Existing] = Outputs[I];
    mOriginal[Existing] = Outputs[I]->OutputString;
    Outputs[I]->OutputString = TeeOutput;
  }
}

STATIC VOID EFIAPI Tick(EFI_EVENT Event, VOID *Context) {
  (VOID)Event; (VOID)Context;
  if (!mLog || mLog->Stopped) return;
  if (mLog->Tail == mLog->Head && mReportedDrops != mLog->Dropped) {
    CHAR8 Notice[96];
    UINTN Size;
    mReportedDrops = mLog->Dropped;
    Size = AsciiSPrint(Notice, sizeof(Notice), "\r\n[UEFI LOG WARNING: dropped bytes total %u]\r\n", mReportedDrops);
    QueueBytes((UINT8 *)Notice, Size);
  }
  HookConsole();
  Flush();
}

STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event, VOID *Context) {
  UINTN I;
  (VOID)Event; (VOID)Context;
  /* Do NOT perform disk I/O here: native OS ownership may be imminent and
     ExitBootServices callbacks must not allocate or change the memory map. */
  mLog->Flush = NULL;
  mLog->Stopped = TRUE;
  for (I = 0; I < mOutputCount; ++I)
    if ((mOutputs[I] == gST->ConOut || mOutputs[I] == gST->StdErr) &&
        mOutputs[I]->OutputString == TeeOutput)
      mOutputs[I]->OutputString = mOriginal[I];
}

EFI_STATUS EFIAPI J250fLogInstall(UINT32 First) {
  EFI_GUID Guid = J250F_TEXT_LOG_GUID;
  VOID *Hob = GetFirstGuidHob(&Guid);
  EFI_STATUS Status;
  CHAR8 Banner[160];
  UINTN Length;
  if (mLog || First >= 2048 || !Hob || GET_GUID_HOB_DATA_SIZE(Hob) != sizeof(*mLog))
    return EFI_NOT_READY;
  mLog = GET_GUID_HOB_DATA(Hob);
  if (mLog->Magic != J250F_TEXT_LOG_MAGIC) { mLog = NULL; return EFI_COMPROMISED_DATA; }
  mSector = First;
  mFlushCpu = ArmReadMpidr();
  mReportedDrops = mLog->Dropped;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ExitBoot, NULL,
                             &gEfiEventExitBootServicesGuid, &mExit);
  if (EFI_ERROR(Status)) goto Fail;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Tick, NULL,
                             &gEfiEventReadyToBootGuid, &mReady);
  if (EFI_ERROR(Status)) goto Fail;
  Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Tick, NULL, &mTimer);
  if (EFI_ERROR(Status)) goto Fail;
  Status = gBS->SetTimer(mTimer, TimerPeriodic, 1000000); /* 100 ms */
  if (EFI_ERROR(Status)) goto Fail;
  /* Queued early text precedes this explicit session/capture-limit marker. */
  Length = AsciiSPrint(Banner, sizeof(Banner),
    "\r\n[UEFI LOG capture active; first sector %u; early dropped bytes %u; pre-HOB/OS graphics excluded]\r\n",
    First, mLog->Dropped);
  QueueBytes((UINT8 *)Banner, Length);
  HookConsole();
  mLog->Flush = Flush;
  Flush();
  return EFI_SUCCESS;
Fail:
  if (mTimer) { gBS->CloseEvent(mTimer); mTimer = NULL; }
  if (mReady) { gBS->CloseEvent(mReady); mReady = NULL; }
  if (mExit) { gBS->CloseEvent(mExit); mExit = NULL; }
  mLog = NULL;
  return Status;
}
