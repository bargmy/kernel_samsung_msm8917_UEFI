/* Simulated hardware + real backend + new DXE wrapper integration. */
#define main inherited_tests_main
#include "test_core.c"
#undef main
#include "../J250fLkBackend.c"
#include "../J250fLkEmmcDxe.c"
#include <stdarg.h>

static emu hw;
static board_emu bh;
static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *gBS = &services;
EFI_GUID gEfiBlockIoProtocolGuid={0}, gEfiDevicePathProtocolGuid={0},
         gJ250fEmmcDevicePathGuid={.Data1=1},gJ250fEmmcPowerProtocolGuid={.Data1=2};
static J250F_EMMC_POWER_PROTOCOL power_api;
static int publish_count, provider_available, force_power_failure, event_open;
static VOID (EFIAPI *event_notify)(EFI_EVENT,VOID *);

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
static EFI_STATUS EFIAPI fake_event(UINT32 type,EFI_TPL tpl,
    VOID (EFIAPI *cb)(EFI_EVENT,VOID *),VOID *ctx,EFI_EVENT *out) {
    ASSERT(type==EVT_NOTIFY_SIGNAL && tpl==TPL_CALLBACK && ctx==NULL);
    event_notify=cb;event_open=1;*out=(EFI_EVENT)&hw;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_register(EFI_GUID *guid,EFI_EVENT ev,VOID **reg) {
    ASSERT(guid==&gJ250fEmmcPowerProtocolGuid && ev==(EFI_EVENT)&hw);
    *reg=&hw;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fake_close(EFI_EVENT ev) {
    ASSERT(ev==(EFI_EVENT)&hw);event_open=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI supply(J250F_EMMC_POWER_PROTOCOL *p,BOOLEAN on,UINT32 v,UINT32 io) {
    (void)p;
    if(force_power_failure) return EFI_DEVICE_ERROR;
    return rails(&hw,on,v,io)==0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI gpll(J250F_EMMC_POWER_PROTOCOL *p) {
    (void)p;return bh.power_ok?EFI_SUCCESS:EFI_NOT_READY;
}
static void init_firmware(void) {
    setup(&hw);memset(&bh,0,sizeof(bh));bh.power_ok=1;
    memset(&services,0,sizeof(services));
    services.RaiseTPL=fake_raise;services.RestoreTPL=fake_restore;
    services.LocateProtocol=fake_locate;services.InstallMultipleProtocolInterfaces=fake_publish;
    services.CreateEvent=fake_event;services.RegisterProtocolNotify=fake_register;
    services.CloseEvent=fake_close;
    power_api.SetRails=supply;power_api.Gpll0Ready=gpll;
    publish_count=0;provider_available=0;force_power_failure=0;event_open=0;
    mPublished=0;mPowerEvent=NULL;mPowerRegistration=NULL;
}
#ifndef J250F_ALLOW_LK_POWER_HANDOFF
static void test_late_power_and_real_backend(void) {
    UINT8 data[1024];
    init_firmware();
    ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS);
    ASSERT(event_open && publish_count==0);
    provider_available=1;force_power_failure=1;
    event_notify((EFI_EVENT)&hw,NULL);
    ASSERT(publish_count==0 && event_open);
    force_power_failure=0;
    event_notify((EFI_EVENT)&hw,NULL);
    ASSERT(publish_count==1 && !event_open && mPublished);
    ASSERT(hw.cmd_count[17]==1); /* Read LBA0 before installing BlockIo */
    ASSERT(mDisk.Io.Media->ReadOnly && mDisk.Media.BlockSize==512 && mDisk.Media.LastBlock==31);
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,99,0,512,data)==EFI_MEDIA_CHANGED);
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,0,511,data)==EFI_BAD_BUFFER_SIZE);
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,0,512,NULL)==EFI_INVALID_PARAMETER);
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,31,1024,data)==EFI_INVALID_PARAMETER);
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,7,1024,data)==EFI_SUCCESS);
    for(unsigned s=0;s<2;s++)for(unsigned i=0;i<512;i++)
        ASSERT(data[s*512+i]==(UINT8)((s+7)^i));
    ASSERT(mDisk.Io.WriteBlocks(&mDisk.Io,1,7,512,data)==EFI_WRITE_PROTECTED);
    ASSERT(mDisk.Io.Reset(&mDisk.Io,TRUE)==EFI_SUCCESS);
    ASSERT(hw.cmd_count[17]==4); /* LBA0 smoke, 2 sector reads, reset verification */
    printf("PASS: NEW LK-style DXE + actual PIO backend: late PMIC notify, LBA0, BlockIo bounds, read-only\n");
}
#else
static void test_lk_handoff_without_pm8917_protocol(void) {
    UINT8 data[512];
    init_firmware();
    ASSERT(!provider_available);
    ASSERT(J250fLkEmmcEntry(NULL,NULL)==EFI_SUCCESS);
    ASSERT(publish_count==1 && mPublished && !event_open);
    ASSERT(hw.cmd_count[17]==1); /* Real (mocked) LBA0 PIO read, not stub. */
    ASSERT(mDisk.Io.ReadBlocks(&mDisk.Io,1,0,512,data)==EFI_SUCCESS);
    for(unsigned i=0;i<512;i++) ASSERT(data[i]==(UINT8)i);
    ASSERT(mDisk.Io.WriteBlocks(&mDisk.Io,1,0,512,data)==EFI_WRITE_PROTECTED);
    puts("PASS: EXPERIMENTAL LK prepowered handoff path enumerates under powered mock hardware");
}
#endif
int main(void) {
#ifdef J250F_ALLOW_LK_POWER_HANDOFF
    test_lk_handoff_without_pm8917_protocol();
#else
    test_late_power_and_real_backend();
#endif
    return 0;
}
