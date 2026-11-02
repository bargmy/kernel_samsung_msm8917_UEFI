/* Simulated hardware + real backend + new DXE wrapper integration. */
#define main inherited_tests_main
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/Tests/test_core.c"
#undef NULL
#undef ASSERT
#undef main
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fLkBackend.c"
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fEmmcDebug.c"
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *A){return strlen(A);}
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fLkEmmcDxe.c"
#include <stdarg.h>
#undef ASSERT
#define ASSERT(x) do { if(!(x)) {fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);} }while(0)

static emu hw;
static board_emu bh;
static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *gBS = &services;
EFI_GUID gEfiBlockIoProtocolGuid={0}, gEfiDevicePathProtocolGuid={0},
         gJ250fEmmcDevicePathGuid={.Data1=1},gJ250fEmmcPowerProtocolGuid={.Data1=2};
static J250F_EMMC_POWER_PROTOCOL power_api;
static int publish_count, provider_available, force_power_failure;
static UINTN held_us,usb_polls,usb_stops;
EFI_STATUS J250fUsbGadgetStart(J250F_USB_DIAG_COMMAND C){(void)C;return EFI_SUCCESS;}
EFI_STATUS J250fUsbGadgetPoll(VOID){usb_polls++;return EFI_SUCCESS;}
VOID J250fUsbGadgetStop(VOID){usb_stops++;}
VOID J250fAdbShellInit(EFI_HANDLE P){(void)P;usb_polls=usb_stops=0;}
BOOLEAN J250fAdbShellExitRequested(VOID){return usb_polls>=100001;}
VOID J250fAdbShellFinish(VOID){}

UINT8 EFIAPI MmioRead8(UINTN a) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        return read8(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
    if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        return read8(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS));
    ASSERT(0);return 0;
}
UINT16 EFIAPI MmioRead16(UINTN a) {
    ASSERT(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE);
    return read16(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
}
UINT32 EFIAPI MmioRead32(UINTN a) {
    if(a==0x078DB1A8u)return 2;
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        return read32(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
    if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        return read32(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS));
    return board_r32(&bh,(uint32_t)a);
}
UINT8 EFIAPI MmioWrite8(UINTN a,UINT8 v) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        write8(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    else if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        write8(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS),v);
    else ASSERT(0);
    return v;
}
UINT16 EFIAPI MmioWrite16(UINTN a,UINT16 v) {
    ASSERT(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE);
    write16(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    return v;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        write32(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    else if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        write32(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS),v);
    else board_w32(&bh,(uint32_t)a,v);
    return v;
}
void EFIAPI MemoryFence(void) {}
UINTN EFIAPI MicroSecondDelay(UINTN us) {hw.time_us+=(unsigned)us;return us;}
void *ZeroMem(void *p,UINTN n) {return memset(p,0,(size_t)n);}
void *CopyMem(void *d,const void *s,UINTN n) {return memcpy(d,s,(size_t)n);}
void EFIAPI DebugPrint(UINTN level,const char *fmt,...) {(void)level;(void)fmt;}
static EFI_TPL EFIAPI fake_raise(EFI_TPL target){return target;}
static VOID EFIAPI fake_restore(EFI_TPL old){(void)old;}
static EFI_STATUS EFIAPI fake_locate(EFI_GUID *guid,VOID *reg,VOID **result) {
    (void)reg;ASSERT(guid==&gJ250fEmmcPowerProtocolGuid);
    if(!provider_available)return EFI_NOT_FOUND;
    *result=&power_api;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_publish(EFI_HANDLE *handle,...) {
    va_list ap; EFI_GUID *g;int seen=0;
    va_start(ap,handle);
    while((g=va_arg(ap,EFI_GUID *))!=NULL) {
        VOID *v=va_arg(ap,VOID*);ASSERT(v!=NULL);seen++;
    }
    va_end(ap);ASSERT(seen==2);++publish_count;
    *handle=(EFI_HANDLE)&hw;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_stall(UINTN us) {held_us+=us;return EFI_SUCCESS;}
RETURN_STATUS EFIAPI SerialPortInitialize(VOID) {return RETURN_SUCCESS;}
UINTN EFIAPI SerialPortWrite(UINT8 *b,UINTN n) {(void)b;return n;}
void FbConStartScreen(void) {}
UINTN EFIAPI AsciiSPrint(CHAR8 *b,UINTN n,CONST CHAR8 *f,...) {(void)f;if(n)b[0]=0;return 0;}
static EFI_STATUS EFIAPI supply(J250F_EMMC_POWER_PROTOCOL *p,BOOLEAN on,UINT32 v,UINT32 io) {
    (void)p;
    if(force_power_failure) return EFI_DEVICE_ERROR;
    return rails(&hw,on,v,io)==0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI gpll(J250F_EMMC_POWER_PROTOCOL *p) {
    (void)p;return bh.power_ok?EFI_SUCCESS:EFI_NOT_READY;
}
static void init_firmware(void) {
    setup(&hw);hw.ext_csd[179]=0x48;memset(&bh,0,sizeof(bh));bh.power_ok=1;
    memset(&services,0,sizeof(services));
    services.RaiseTPL=fake_raise;services.RestoreTPL=fake_restore;
    services.LocateProtocol=fake_locate;services.InstallMultipleProtocolInterfaces=fake_publish;
    services.Stall=fake_stall;
    power_api.SetRails=supply;power_api.Gpll0Ready=gpll;
    publish_count=0;provider_available=0;force_power_failure=0;held_us=0;
    mPublished=0;
}
int main(void) {
 UINT8 data[1024];
 init_firmware();
 ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS && !publish_count);
 ASSERT(held_us==100501000 && usb_stops==1 && hw.cmd_count[0]==0);
 init_firmware();provider_available=1;force_power_failure=1;
 ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS && !publish_count);
 ASSERT(hw.cmd_count[0]==0);
 init_firmware();provider_available=1;
 ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS && publish_count==1 && mPublished);
 ASSERT(hw.cmd_count[17]==1 && hw.cmd_count[6]==0 && hw.cmd_count[24]==0);
 ASSERT(held_us==100501000 && usb_stops==1 && mHost.frequency_hz==400000 && !mHost.bus_8bit);
 ASSERT(mDisk.Media.ReadOnly && mDisk.Media.LastBlock==31);
 ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,99,0,512,data)==EFI_MEDIA_CHANGED);
 ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,0,511,data)==EFI_BAD_BUFFER_SIZE);
 ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,31,1024,data)==EFI_INVALID_PARAMETER);
 ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,7,1024,data)==EFI_SUCCESS);
 for(unsigned s=0;s<2;s++)for(unsigned i=0;i<512;i++)ASSERT(data[s*512+i]==(UINT8)((s+7)^i));
 ASSERT(mDisk.Io.WriteBlocks(&mDisk.Io,1,0,512,data)==EFI_WRITE_PROTECTED);
 ASSERT(mDisk.Io.Reset(&mDisk.Io,TRUE)==EFI_SUCCESS);
 init_firmware();provider_available=1;hw.suppress_command=17;
 ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS && !publish_count && !mPublished);
 ASSERT(hw.cmd_count[17]==1);
 init_firmware();provider_available=1;
 ASSERT(J250fLkBoardOpen(&(J250F_LK_DISK){0})==EFI_SUCCESS);
 mHost.card_ready=0;mProbeCount=0;
 ASSERT(J250fEmmcSetClock(144000)==EFI_SUCCESS);
 ASSERT((bh.gcc[0x42008/4]&31)==31 && bh.gcc[0x4200c/4]==3);
 ASSERT(J250fEmmcTry(TRUE)==EFI_SUCCESS && mHost.cid_valid && !mHost.card_ready);
 ASSERT(mProbeCount==1);
 {CHAR8 Out[4096];UINTN Len;UINT32 Clock=MmioRead16(J250F_HC_PHYS+0x2c);
  ASSERT(J250fEmmcDebugCommand("emmc config set hc2 7 1",Out,sizeof(Out),&Len));
  ASSERT((MmioRead16(J250F_HC_PHYS+0x3e)&7)==1);
  ASSERT(MmioRead16(J250F_HC_PHYS+0x2c)==Clock);
  ASSERT(J250fEmmcTry(TRUE)==EFI_SUCCESS);
  ASSERT(mHost.trace[(mHost.trace_sequence-1)%J250F_TRACE_DEPTH].hc2%8==1);
  ASSERT(J250fEmmcDebugCommand("emmc config reset",Out,sizeof(Out),&Len));
  ASSERT((MmioRead16(J250F_HC_PHYS+0x3e)&7)==0);
  ASSERT(J250fEmmcDebugCommand("emmc config set hc2 8 8",Out,sizeof(Out),&Len));
  ASSERT(mProfiles[2].Mask==0);
 }
 mProbeCount=32;
 {unsigned Count=hw.cmd_count[0];
  ASSERT(J250fEmmcTry(TRUE)==EFI_ACCESS_DENIED && hw.cmd_count[0]==Count);
 }
 puts("PASS: Mu-header integration simulation, backend+frontend, provider errors, LBA0 required before publication, read-only IO, 400kHz, persistent session beyond 90s and explicit exit, failed read blocks publication");
 return 0;
}
