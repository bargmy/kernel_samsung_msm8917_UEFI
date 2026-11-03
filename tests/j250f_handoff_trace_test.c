/* Real service wrappers, fake firmware tables: no device I/O or reset. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fHandoffTrace.c"
#define CHECK(X) do { if(!(X)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#X); exit(1); } } while(0)
STATIC EFI_BOOT_SERVICES Bs;
STATIC EFI_RUNTIME_SERVICES Rt;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_RUNTIME_SERVICES *gRT=&Rt;
EFI_GUID gEfiLoadedImageProtocolGuid;
STATIC EFI_LOADED_IMAGE_PROTOCOL Info;
STATIC EFI_TPL CurrentTpl;
STATIC UINTN Loads,Starts,Exits,Resets,Messages,CrcCalls,FailCrc;
STATIC BOOLEAN Ready,Recurse,StartExits;
STATIC BOOLEAN InReset;
STATIC EFI_STATUS LoadResult,StartResult,ExitResult;
STATIC EFI_HANDLE Handle=(EFI_HANDLE)(UINTN)0x1234;
STATIC CHAR8 LastMessage[256];

UINTN EFIAPI AsciiSPrint(CHAR8 *Out,UINTN Size,CONST CHAR8 *Format,...) {
  UINTN N=strlen(Format); if(N>=Size) N=Size-1; memcpy(Out,Format,N); Out[N]=0; return N;
}
UINTN EFIAPI SerialPortWrite(UINT8 *Text,UINTN Size) {
  ++Messages; CHECK(Size<sizeof(LastMessage)); memcpy(LastMessage,Text,Size); LastMessage[Size]=0;
  if(Recurse) { EFI_HANDLE Image=NULL; Recurse=FALSE; gBS->LoadImage(FALSE,NULL,NULL,NULL,0,&Image); }
  return Size;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out) {
  CHECK(!InReset); (VOID)Guid;(VOID)Registration; if(!Ready) return EFI_NOT_FOUND; *Out=&Ready; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetInfo(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Out) {
  (VOID)Image;(VOID)Guid; *Out=&Info; return EFI_SUCCESS;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New) { EFI_TPL Old=CurrentTpl; CurrentTpl=New; return Old; }
STATIC VOID EFIAPI Restore(EFI_TPL Old) { CurrentTpl=Old; }
STATIC EFI_STATUS EFIAPI Calculate(VOID *Data,UINTN Size,UINT32 *Out) {
  ++CrcCalls; CHECK(Size==((EFI_TABLE_HEADER *)Data)->HeaderSize); CHECK(*Out==0);
  if(CrcCalls==FailCrc) return EFI_DEVICE_ERROR; *Out=0x778899; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI OriginalLoad(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,
                                     VOID *Source,UINTN Bytes,EFI_HANDLE *Image) {
  (VOID)Policy;(VOID)Parent;(VOID)Path;(VOID)Source;(VOID)Bytes; ++Loads;
  if(Image) *Image=Handle; return LoadResult;
}
STATIC EFI_STATUS EFIAPI OriginalExit(EFI_HANDLE Image,UINTN Key) {
  (VOID)Image;(VOID)Key; ++Exits;
  if(!EFI_ERROR(ExitResult)) J250fHandoffTraceStop();
  return ExitResult;
}
STATIC EFI_STATUS EFIAPI OriginalStart(EFI_HANDLE Image,UINTN *Bytes,CHAR16 **Data) {
  (VOID)Bytes;(VOID)Data; ++Starts;
  if(StartExits) return gBS->ExitBootServices(Image,42);
  return StartResult;
}
STATIC VOID EFIAPI OriginalReset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data) {
  CHECK(Type==EfiResetWarm && Status==EFI_ABORTED && Bytes==32 && Data==(VOID *)(UINTN)0xdead0000);
  ++Resets;
}
STATIC VOID Fixture(VOID) {
  memset(&Bs,0,sizeof(Bs)); memset(&Rt,0,sizeof(Rt)); memset(&Info,0,sizeof(Info));
  Bs.Hdr.HeaderSize=sizeof(Bs); Rt.Hdr.HeaderSize=sizeof(Rt); Bs.Hdr.CRC32=123; Rt.Hdr.CRC32=456;
  Bs.LoadImage=OriginalLoad; Bs.StartImage=OriginalStart; Bs.ExitBootServices=OriginalExit;
  Bs.LocateProtocol=Locate; Bs.HandleProtocol=GetInfo; Bs.RaiseTPL=Raise; Bs.RestoreTPL=Restore;
  Bs.CalculateCrc32=Calculate; Rt.ResetSystem=OriginalReset;
  Info.ImageBase=(VOID *)(UINTN)0x10000000; Info.ImageSize=0xbe000;
  mInstalled=mLogging=FALSE; CurrentTpl=TPL_APPLICATION;
  Loads=Starts=Exits=Resets=Messages=CrcCalls=FailCrc=0;
  Ready=TRUE; Recurse=StartExits=FALSE; LoadResult=StartResult=EFI_ACCESS_DENIED; ExitResult=EFI_INVALID_PARAMETER;
  InReset=FALSE;
}
int main(void) {
  EFI_HANDLE Image=NULL; UINTN Bytes=0; CHAR16 *Data=NULL; UINTN Before;
  Fixture(); CHECK(J250fHandoffTraceInstall()==EFI_SUCCESS); CHECK(CrcCalls==2 && CurrentTpl==TPL_APPLICATION);
  CHECK(J250fHandoffTraceInstall()==EFI_ALREADY_STARTED);
  CHECK(gBS->LoadImage(TRUE,NULL,NULL,NULL,0,&Image)==EFI_ACCESS_DENIED && Loads==1 && Messages==2);
  CHECK(gBS->StartImage(Handle,&Bytes,&Data)==EFI_ACCESS_DENIED && Starts==1);
  CHECK(strstr(LastMessage,"StartImage RETURN")!=NULL);
  InReset=TRUE; gRT->ResetSystem(EfiResetWarm,EFI_ABORTED,32,(VOID *)(UINTN)0xdead0000); InReset=FALSE; CHECK(Resets==1);
  CHECK(strstr(LastMessage,"ResetSystem REQUEST")!=NULL);
  Ready=FALSE; Before=Messages; gBS->StartImage(Handle,&Bytes,&Data); CHECK(Messages==Before && Starts==2);
  Ready=TRUE; Recurse=TRUE; gBS->LoadImage(TRUE,NULL,NULL,NULL,0,&Image); CHECK(Loads==3 && !Recurse);
  CHECK(gBS->ExitBootServices(Handle,42)==EFI_INVALID_PARAMETER && mInstalled);
  CHECK(strstr(LastMessage,"ExitBootServices RETURN")!=NULL);
  StartExits=TRUE; ExitResult=EFI_SUCCESS; CHECK(gBS->StartImage(Handle,&Bytes,&Data)==EFI_SUCCESS);
  CHECK(!mInstalled && gBS->LoadImage==OriginalLoad && gBS->StartImage==OriginalStart &&
        gBS->ExitBootServices==OriginalExit && gRT->ResetSystem==OriginalReset);
  CHECK(strstr(LastMessage,"ExitBootServices ENTER")!=NULL);
  Fixture(); FailCrc=2; CHECK(J250fHandoffTraceInstall()==EFI_DEVICE_ERROR);
  CHECK(!mInstalled && Bs.Hdr.CRC32==123 && Rt.Hdr.CRC32==456 && Bs.LoadImage==OriginalLoad && Rt.ResetSystem==OriginalReset);
  puts("handoff hooks: forwarding, CRC rollback, recursion, reset and EBS restoration: PASS");
  return 0;
}
