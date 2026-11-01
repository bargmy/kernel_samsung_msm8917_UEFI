/* Host-run mock DXE protocol integration tests: NOT an actual Mu firmware build. */
#define main inherited_tests_main
#include "test_core.c"
#undef main
#include "../J250fEmmcDxe.c"
#include <stdarg.h>

static emu hw;
static board_emu bh;
static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *gBS = &services;
EFI_GUID gEfiBlockIoProtocolGuid={0},gEfiDevicePathProtocolGuid={0},
         gJ250fEmmcDevicePathGuid={.Data1=1},gJ250fEmmcPowerProtocolGuid={.Data1=2};
static J250F_EMMC_POWER_PROTOCOL power_api;
static int publish_count, provider_available, force_power_failure;

UINT8 MmioRead8(UINTN a) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        return read8(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
    if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        return read8(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS));
    ASSERT(0);return 0;
}
UINT16 MmioRead16(UINTN a) {
    ASSERT(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE);
    return read16(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
}
UINT32 MmioRead32(UINTN a) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        return read32(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS));
    if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        return read32(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS));
    return board_r32(&bh,(uint32_t)a);
}
UINT8 MmioWrite8(UINTN a,UINT8 v) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        write8(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    else if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        write8(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS),v);
    else ASSERT(0);
    return v;
}
UINT16 MmioWrite16(UINTN a,UINT16 v) {
    ASSERT(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE);
    write16(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    return v;
}
UINT32 MmioWrite32(UINTN a,UINT32 v) {
    if(a>=J250F_HC_PHYS && a<J250F_HC_PHYS+J250F_HC_SIZE)
        write32(&hw,J250F_REGION_HC,(uint32_t)(a-J250F_HC_PHYS),v);
    else if(a>=J250F_CORE_PHYS && a<J250F_CORE_PHYS+J250F_CORE_SIZE)
        write32(&hw,J250F_REGION_CORE,(uint32_t)(a-J250F_CORE_PHYS),v);
    else board_w32(&bh,(uint32_t)a,v);
    return v;
}
void MemoryFence(void) {}
UINTN MicroSecondDelay(UINTN us) {hw.time_us+=(unsigned)us;return us;}
void *ZeroMem(void *p,UINTN n) {return memset(p,0,(size_t)n);}
void *CopyMem(void *d,const void *s,UINTN n) {return memcpy(d,s,(size_t)n);}
void DebugPrint(UINTN level,const char *fmt,...) {(void)level;(void)fmt;}
static EFI_TPL EFIAPI fake_raise(EFI_TPL target){return target;}
static VOID EFIAPI fake_restore(EFI_TPL old){(void)old;}
static EFI_STATUS EFIAPI fake_locate(EFI_GUID *guid, VOID *registration, VOID **result) {
    (void)registration;
    ASSERT(guid==&gJ250fEmmcPowerProtocolGuid);
    if(!provider_available) return EFI_NOT_READY;
    *result=&power_api;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_publish(EFI_HANDLE *handle,...) {
    va_list ap;
    EFI_GUID *guid;
    int seen=0;
    va_start(ap,handle);
    while((guid=va_arg(ap,EFI_GUID *))!=NULL) {
        void *ptr=va_arg(ap,void *);
        ASSERT(ptr!=NULL);
        seen++;
    }
    va_end(ap);
    ASSERT(seen==2);
    ++publish_count;
    *handle=(EFI_HANDLE)&hw;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI supply(J250F_EMMC_POWER_PROTOCOL *p, BOOLEAN on, UINT32 v, UINT32 io) {
    (void)p;
    if(force_power_failure) return EFI_DEVICE_ERROR;
    return rails(&hw,on,v,io)==0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI gpll(J250F_EMMC_POWER_PROTOCOL *p) {
    (void)p;
    return bh.power_ok?EFI_SUCCESS:EFI_NOT_READY;
}
static void init_firmware(void) {
    setup(&hw); memset(&bh,0,sizeof(bh));
    bh.power_ok=1;
    services.RaiseTPL=fake_raise;
    services.RestoreTPL=fake_restore;
    services.LocateProtocol=fake_locate;
    services.InstallMultipleProtocolInterfaces=fake_publish;
    power_api.SetRails=supply;
    power_api.Gpll0Ready=gpll;
    publish_count=0;provider_available=1;force_power_failure=0;
}
static void test_uefi_blockio(void) {
    UINT8 data[1024];
    EFI_STATUS status;
    init_firmware();
    provider_available=0;
    ASSERT(J250fEmmcDxeEntryPoint(NULL,NULL)==EFI_NOT_READY);
    ASSERT(publish_count==0);
    provider_available=1;
    force_power_failure=1;
    ASSERT(J250fEmmcDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR);
    ASSERT(publish_count==0);
    force_power_failure=0;
    status=J250fEmmcDxeEntryPoint(NULL,NULL);
    ASSERT(status==EFI_SUCCESS);
    ASSERT(publish_count==1 && mDev.BlockIo.Media->MediaPresent);
    ASSERT(mDev.BlockIo.Media->ReadOnly && mDev.BlockIo.Media->BlockSize==512);
    ASSERT(mDev.BlockIo.Media->LastBlock==31);
    ASSERT(mDev.Path.Vendor.Header.Type==HARDWARE_DEVICE_PATH);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,999,0,512,data)==EFI_MEDIA_CHANGED);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,0,511,data)==EFI_BAD_BUFFER_SIZE);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,0,512,NULL)==EFI_INVALID_PARAMETER);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,31,1024,data)==EFI_INVALID_PARAMETER);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,UINT64_MAX,512,data)==EFI_INVALID_PARAMETER);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,7,1024,data)==EFI_SUCCESS);
    for(unsigned s=0;s<2;s++)for(unsigned i=0;i<512;i++)
        ASSERT(data[s*512+i]==(UINT8)((s+7)^i));
    ASSERT(mDev.BlockIo.WriteBlocks(&mDev.BlockIo,1,2,512,data)==EFI_WRITE_PROTECTED);
    ASSERT(mDev.BlockIo.FlushBlocks(&mDev.BlockIo)==EFI_SUCCESS);
    ASSERT(mDev.BlockIo.Reset(&mDev.BlockIo,FALSE)==EFI_SUCCESS);
    ASSERT(mDev.Media.MediaId==2);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,1,0,512,data)==EFI_MEDIA_CHANGED);
    ASSERT(mDev.BlockIo.ReadBlocks(&mDev.BlockIo,2,0,512,data)==EFI_SUCCESS);
    puts("PASS: DXE publication, power-provider failure, BlockIo read/write/media/reset protocol");
}
int main(void) {
    test_uefi_blockio();
    return 0;
}
