// Host-side controller simulation; never accesses physical MMIO.
#include <stdio.h>
#include <assert.h>
#undef NULL
#include "../GPLDrivers/Library/J250fSdccPlatformLib/J250fSdccPlatformLib.c"
#include "../Platforms/Samsung/J250FPkg/Drivers/J250fEmmcDxe/J250fEmmcDxe.c"
static UINT8 host[0x500], core[0x800], gcc_regs[0x100], pad_regs[4];
static unsigned cmd_count, data_index, current_cmd, read_count;
static BOOLEAN fail_command, wrong_partition, no_completion;
EFI_BOOT_SERVICES *gBS;
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n) {
 UINT8 *a=d; CONST UINT8 *b=s; for(UINTN i=0;i<n;i++)a[i]=b[i];return d;
}
VOID *EFIAPI ZeroMem(VOID *d, UINTN n) {
 UINT8 *a=d;for(UINTN i=0;i<n;i++)a[i]=0;return d;
}
static UINT8 *regptr(UINTN a) {
 if(a==0x0110a000U)return pad_regs;
 if(a>=0x01842000U && a<0x01842100U)return gcc_regs+a-0x01842000U;
 if(a>=HC && a<HC+sizeof(host))return host+a-HC;
 assert(a>=CORE && a<CORE+sizeof(core));return core+a-CORE;
}
UINT8 EFIAPI MmioRead8(UINTN a) {return *regptr(a);}
UINT16 EFIAPI MmioRead16(UINTN a) {
 UINT16 v;CopyMem(&v,regptr(a),2);return v;
}
UINT32 EFIAPI MmioRead32(UINTN a) {
 UINT32 v;
 if(a==HC+0x20) {
  unsigned i=data_index++*4; v=0;
  for(unsigned j=0;j<4;j++) {
   UINT8 b=(UINT8)(i+j);
   if(current_cmd==8) {
    b=0;
    if(i+j==213)b=0x10; // 4096 sectors
    if(i+j==179 && wrong_partition)b=1;
   }
   v|=(UINT32)b<<(j*8);
  }
  return v;
 }
 CopyMem(&v,regptr(a),4);return v;
}
UINT8 EFIAPI MmioWrite8(UINTN a,UINT8 v) {
 if(a==HC+0x2f)host[0x24]=0;
 *regptr(a)= a==HC+0x2f ? 0:v;return v;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
 UINT32 old;
 if(a==CORE+0xe4) {old=MmioRead32(CORE+0xdc);old&=~v;CopyMem(core+0xdc,&old,4);}
 else if(a==HC+0x30) {old=MmioRead32(a);old&=~v;CopyMem(regptr(a),&old,4);}
 else {if(a==0x01842004U)v&=~1U;if(a==CORE+0x78)v&=1;CopyMem(regptr(a),&v,4);}
 return v;
}
UINT16 EFIAPI MmioWrite16(UINTN a,UINT16 v) {
 if(a==HC+0x2c && (v&1))v|=2;
 CopyMem(regptr(a),&v,2);
 if(a==HC+0x0e) {
  current_cmd=v>>8;cmd_count++;data_index=0;
  assert(current_cmd==0 || current_cmd==1 || current_cmd==2 || current_cmd==3 ||
         current_cmd==7 || current_cmd==8 || current_cmd==9 || current_cmd==17);
  if(current_cmd==17)read_count++;
  UINT32 status=no_completion ? 0U : fail_command ? 0x10000U : 0x23U;
  if(no_completion)host[0x24]=1;
  CopyMem(host+0x30,&status,4);
  UINT32 resp=current_cmd==1 ? 0xc0ff8000U : 0;
  CopyMem(host+0x10,&resp,4);
 }
 return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) {return n;}
static EFI_STATUS EFIAPI stall(UINTN n) {return EFI_SUCCESS;}
static EFI_TPL EFIAPI raise(EFI_TPL p) {return TPL_APPLICATION;}
static VOID EFIAPI restore(EFI_TPL p) {}
static EFI_BOOT_SERVICES bs={.Stall=stall,.RaiseTPL=raise,.RestoreTPL=restore};
EFI_GUID gEfiBlockIoProtocolGuid={0},gEfiDevicePathProtocolGuid={0};
static void setup(void) {
 ZeroMem(host,sizeof(host));ZeroMem(core,sizeof(core));ZeroMem(gcc_regs,sizeof(gcc_regs));ZeroMem(pad_regs,sizeof(pad_regs));
 host[0x29]=1;host[0xfe]=2;gBS=&bs;mReady=FALSE;mBusy=FALSE;
 cmd_count=read_count=0;fail_command=wrong_partition=no_completion=FALSE;
}
int main(void) {
 EFI_STATUS s;UINT8 buf[1024];unsigned before;
 setup();core[0xdc]=2;s=InitializeCard();assert(s==EFI_SUCCESS && mReady);
 assert(MmioRead16(HC+0x2c)==7); // /1 on known 400kHz source
 assert(MmioRead32(0x01842008U)==0x2017);
 assert(mMedia.ReadOnly && mMedia.LastBlock==4095 && mMedia.BlockSize==512);
 assert(MmioRead32(CORE+0xdc)==0 && (MmioRead32(CORE+0xe8)&1));
 assert(MmioRead32(CORE+0x78)==1); // unsupported bit13 does not reject host
 before=cmd_count;assert(Command(24,0,0,0)==EFI_ACCESS_DENIED && cmd_count==before);
 assert(Write(&mIo,1,0,512,buf)==EFI_WRITE_PROTECTED && cmd_count==before);
 assert(Read(&mIo,1,4095,1024,buf)==EFI_INVALID_PARAMETER && cmd_count==before);
 assert(Read(&mIo,2,0,512,buf)==EFI_MEDIA_CHANGED);
 assert(Read(&mIo,1,0,513,buf)==EFI_BAD_BUFFER_SIZE);
 assert(Read(&mIo,1,0,1024,buf)==EFI_SUCCESS && read_count==2);
 for(unsigned i=0;i<1024;i++)assert(buf[i]==(UINT8)i);
 fail_command=TRUE;assert(EFI_ERROR(Read(&mIo,1,0,512,buf)) && !mReady);
 before=cmd_count;assert(EFI_ERROR(Read(&mIo,1,0,512,buf)) && before==cmd_count);
 setup();wrong_partition=TRUE;assert(InitializeCard()==EFI_UNSUPPORTED && !mReady);
 setup();core[0xdc]=1;assert(InitializeCard()==EFI_SUCCESS);
 core[0xdc]=1;assert(Read(&mIo,1,0,512,buf)==EFI_UNSUPPORTED && !mReady);
 setup();host[0x29]=0;assert(InitializeCard()==EFI_UNSUPPORTED && cmd_count==0);
 setup();no_completion=TRUE;
 assert(InitializeCard()==EFI_TIMEOUT && !mReady);
 assert(cmd_count==1 && mLastCommand==0 && mLastStatus==0);
 assert(mFailureState==1 && MmioRead32(HC+0x24)==0);
 puts("PASS: CMD0 no-completion case returns timeout, captures pre-reset inhibit, installs no media");
 puts("PASS: initialization, PIO reads, bounds, command/write rejection, failure handling, partition guard");
 return 0;
}
