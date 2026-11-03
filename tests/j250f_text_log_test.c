/* Actual logger engine, mocked UEFI scheduler/console and persistent sink. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include <PiDxe.h>
/* Host-only CPU adapter. Production source is separately ARM-compiled. */
#define ARM_LIB_H_
BOOLEAN EFIAPI ArmGetInterruptState(VOID);
UINTN EFIAPI ArmDisableInterrupts(VOID);
VOID EFIAPI ArmEnableInterrupts(VOID);
UINTN EFIAPI ArmReadMpidr(VOID);
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fTextLogDxe.c"
#undef ASSERT
#define CHECK(X) do { if(!(X)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#X); exit(1); } } while(0)
STATIC struct { EFI_HOB_GUID_TYPE Header; J250F_TEXT_LOG Log; } Hob;
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_SYSTEM_TABLE System;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_SYSTEM_TABLE *gST=&System;
EFI_GUID gEfiEventReadyToBootGuid, gEfiEventExitBootServicesGuid;
EFI_STATUS EFIAPI J250fHandoffTraceInstall(VOID) { return EFI_SUCCESS; }
VOID EFIAPI J250fHandoffTraceStop(VOID) {}
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
STATIC EFI_TPL CurrentTpl=TPL_APPLICATION;
STATIC BOOLEAN Irqs=TRUE;
STATIC UINTN Cpu;
STATIC UINT8 Records[2048][512];
STATIC UINT32 Written, Attempts, OriginalCalls;
STATIC EFI_STATUS NextWrite=EFI_SUCCESS;
STATIC BOOLEAN TickOnRestore;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D,V,N); }
VOID EFIAPI MemoryFence(VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *V, UINT32 Compare, UINT32 Exchange) {
  __atomic_compare_exchange_n(V,&Compare,Exchange,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); return Compare;
}
BOOLEAN EFIAPI ArmGetInterruptState(VOID) { return Irqs; }
UINTN EFIAPI ArmDisableInterrupts(VOID) { UINTN Old=Irqs; Irqs=FALSE; return Old; }
VOID EFIAPI ArmEnableInterrupts(VOID) { Irqs=TRUE; }
UINTN EFIAPI ArmReadMpidr(VOID) { return Cpu; }
VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *Guid) { (VOID)Guid; return &Hob; }
UINTN EFIAPI AsciiSPrint(CHAR8 *Out, UINTN Size, CONST CHAR8 *Format, ...) {
  va_list Args; int N; va_start(Args,Format); N=vsnprintf(Out,Size,Format,Args); va_end(Args);
  return N<0 ? 0 : (UINTN)N;
}
EFI_STATUS EFIAPI J250fLogWriteRecord(UINT32 Index, CONST VOID *Data) {
  EFI_STATUS Result=NextWrite; NextWrite=EFI_SUCCESS; ++Attempts;
  if (EFI_ERROR(Result)) return Result;
  CHECK(Index<2048); memcpy(Records[Index],Data,512); ++Written;
  /* A backend diagnostic may recursively trigger serial flushing. */
  if (mLog->Flush) mLog->Flush();
  return EFI_SUCCESS;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL Tpl) { EFI_TPL Old=CurrentTpl; CurrentTpl=Tpl; return Old; }
STATIC VOID EFIAPI Restore(EFI_TPL Tpl) {
  CurrentTpl=Tpl;
  if (TickOnRestore && Tpl==TPL_APPLICATION) {
    TickOnRestore=FALSE; CurrentTpl=TPL_CALLBACK; Tick(NULL,NULL); CurrentTpl=Tpl;
  }
}
STATIC EFI_STATUS EFIAPI EventEx(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY F, CONST VOID *C, CONST EFI_GUID *G, EFI_EVENT *E) {
  (VOID)Type;(VOID)Tpl;(VOID)F;(VOID)C;(VOID)G; *E=(EFI_EVENT)1; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Event(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY F, VOID *C, EFI_EVENT *E) {
  return EventEx(Type,Tpl,F,C,NULL,E);
}
STATIC EFI_STATUS EFIAPI Timer(EFI_EVENT E, EFI_TIMER_DELAY T, UINT64 D) { (VOID)E;(VOID)T;(VOID)D; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E) { (VOID)E; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Original(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String) {
  (VOID)This; (VOID)String; ++OriginalCalls; CHECK(mLog->SuppressSerial); return EFI_WARN_UNKNOWN_GLYPH;
}
STATIC VOID Fixture(VOID) {
  memset(&Hob,0,sizeof(Hob)); memset(&Services,0,sizeof(Services)); memset(&System,0,sizeof(System));
  memset(&Console,0,sizeof(Console)); memset(mOutputs,0,sizeof(mOutputs)); memset(mOriginal,0,sizeof(mOriginal));
  Hob.Header.Header.HobLength=(UINT16)(sizeof(EFI_HOB_GUID_TYPE)+sizeof(J250F_TEXT_LOG));
  Hob.Log.Magic=J250F_TEXT_LOG_MAGIC;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; Services.CreateEventEx=EventEx;
  Services.CreateEvent=Event; Services.SetTimer=Timer; Services.CloseEvent=Close;
  Console.OutputString=Original; System.ConOut=System.StdErr=&Console;
  mLog=NULL; mFlushing=FALSE; mOutputCount=0; mTimer=mReady=mExit=NULL;
  Cpu=0; Irqs=TRUE; CurrentTpl=TPL_APPLICATION; Written=Attempts=OriginalCalls=0;
  NextWrite=EFI_SUCCESS;
  TickOnRestore=FALSE;
  CHECK(J250fLogInstall(0)==EFI_SUCCESS); CHECK(Console.OutputString==TeeOutput); CHECK(mOutputCount==1);
  Written=Attempts=0; /* Ignore the session banner for subsequent assertions. */
}
int main(int argc, char **argv) {
  UINT32 Start, OldTail;
  UINT8 Big[J250F_TEXT_LOG_BYTES+42];
  CHAR16 Text[]={ 'A', 0x20ac, 0xd83d, 0xde00, ' ', ' ', '\n', 0 };
  STATIC CONST UINT8 Expected[]={ 'A',0xe2,0x82,0xac,0xf0,0x9f,0x98,0x80,' ',' ','\n' };
  Fixture(); Start=mSector;
  CHECK(Console.OutputString(&Console,Text)==EFI_WARN_UNKNOWN_GLYPH); CHECK(OriginalCalls==1);
  CHECK(Written==1); CHECK(!memcmp(Records[Start]+32,Expected,sizeof(Expected)));
  CHECK(!memcmp(Records[Start]+25,"00000b",6)); CHECK(!mLog->SuppressSerial);
  if (argc>1) {
    UINT32 Index; UINT8 Ready[512]; FILE *Out=fopen(argv[1],"wb"); CHECK(Out!=NULL);
    for (Index=0;Index<2048;++Index) {
      memset(Ready,' ',512); j250f_log_marker(Ready,Index,0,0);
      CHECK(fwrite(Index<mSector?Records[Index]:Ready,1,512,Out)==512);
    }
    CHECK(fclose(Out)==0);
  }
  Fixture(); QueueBytes((UINT8 *)"keep",4); OldTail=mLog->Tail;
  NextWrite=EFI_NOT_READY; Flush(); CHECK(Written==0 && mLog->Tail==OldTail && !mLog->Stopped);
  Flush(); CHECK(Written==1 && mLog->Tail==mLog->Head);
  Fixture(); QueueBytes((UINT8 *)"pending",7); CurrentTpl=TPL_NOTIFY; Flush(); CHECK(!Attempts);
  CurrentTpl=TPL_APPLICATION; Irqs=FALSE; Flush(); CHECK(!Attempts);
  Irqs=TRUE; Cpu=1; Flush(); CHECK(!Attempts); Cpu=0; Flush(); CHECK(Written==1);
  Fixture(); QueueBytes((UINT8 *)"fail",4); NextWrite=EFI_DEVICE_ERROR; Flush();
  CHECK(mLog->Stopped && Attempts==1); Flush(); CHECK(Attempts==1);
  Fixture(); QueueBytes((UINT8 *)"pending timer",13); TickOnRestore=TRUE;
  NextWrite=EFI_DEVICE_ERROR; OldTail=mLog->Tail; Flush();
  CHECK(Attempts==1 && mLog->Stopped && mLog->Tail==OldTail);
  Fixture(); memset(Big,'x',sizeof(Big)); QueueBytes(Big,sizeof(Big));
  CHECK(mLog->Dropped==43); Flush(); CHECK(mLog->Tail==mLog->Head);
  Tick(NULL,NULL); CHECK(mReportedDrops==43);
  Fixture(); mLog->ProducerLock=1; QueueBytes((UINT8 *)"busy",4); CHECK(mLog->Dropped==4);
  mLog->ProducerLock=0; CHECK(mLog->Tail==mLog->Head);
  Fixture(); mSector=2047; memset(Big,'y',600); QueueBytes(Big,600); Flush();
  CHECK(Written==1 && mLog->Stopped);
  Fixture(); QueueBytes((UINT8 *)"last",4); Start=Attempts; ExitBoot(NULL,NULL);
  CHECK(mLog->Stopped && mLog->Flush==NULL && Console.OutputString==Original);
  Tick(NULL,NULL); CHECK(Attempts==Start);
  Fixture(); mLog->Head=J250F_TEXT_LOG_BYTES; QueueBytes((UINT8 *)"bad",3); CHECK(mLog->Stopped);
  puts("text tee, UTF-8, exact lengths, queue bounds, TPL, retries, recursion and EBS: PASS");
  return 0;
}
