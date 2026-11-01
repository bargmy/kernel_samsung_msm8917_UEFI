// Actual SMEM target-info/GCD adapter; mocked firmware memory descriptors.
#include <stdio.h>
#include <assert.h>
#include <string.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/PlatformIntegration/SmemMap.c"
static UINT32 target_ptr, target_hi, magic, smem_size, smem_base;
static unsigned info_reads, add_calls, attr_calls;
static int system_memory, nonexistent, add_fail;
static UINTN configured_base;static UINT32 configured_size;
static EFI_DXE_SERVICES ds;
EFI_DXE_SERVICES *gDS=&ds;
UINT32 EFIAPI MmioRead32(UINTN a){
 switch(a){
 case 0x193d000:return target_ptr;
 case 0x193d004:return target_hi;
 case 0x862ff000:info_reads++;return magic;
 case 0x862ff004:info_reads++;return smem_size;
 case 0x862ff008:info_reads++;return smem_base;
 default:assert(!"unexpected read: 32-bit target-info base has no high word");return 0;
 }
}
EFI_STATUS J250fRpmConfigureSmem(UINTN b,UINT32 n){configured_base=b;configured_size=n;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI get(EFI_PHYSICAL_ADDRESS b,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *d){
 memset(d,0,sizeof(*d));d->BaseAddress=0;d->Length=0x100000000ULL;
 d->Capabilities=EFI_MEMORY_UC;
 d->GcdMemoryType=(system_memory==1 || (system_memory==2 && b>=0x99000000U))?EfiGcdMemoryTypeSystemMemory:
   nonexistent?EfiGcdMemoryTypeNonExistent:EfiGcdMemoryTypeMemoryMappedIo;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI add(EFI_GCD_MEMORY_TYPE t,EFI_PHYSICAL_ADDRESS b,UINT64 n,UINT64 c){
 assert(t==EfiGcdMemoryTypeMemoryMappedIo && !(b&4095) && !(n&4095) && c==EFI_MEMORY_UC);
 add_calls++;return add_fail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attrs(EFI_PHYSICAL_ADDRESS b,UINT64 n,UINT64 a){
 assert(!(b&4095) && !(n&4095) && a==EFI_MEMORY_UC);attr_calls++;return EFI_SUCCESS;
}
static void setup(void){
 memset(&ds,0,sizeof(ds));ds.GetMemorySpaceDescriptor=get;ds.AddMemorySpace=add;ds.SetMemorySpaceAttributes=attrs;
 target_ptr=0x862ff000;target_hi=0;magic=0x49494953;smem_size=0x100000;smem_base=0x86300000;
 info_reads=add_calls=attr_calls=system_memory=nonexistent=add_fail=0;configured_base=configured_size=0;
}
int main(void){
 setup();assert(J250fMapRpmSmem()==EFI_SUCCESS && info_reads==3 && attr_calls==2);
 assert(configured_base==0x86300000 && configured_size==0x100000);
 setup();smem_base=0x86400000;assert(J250fMapRpmSmem()==EFI_SUCCESS && configured_base==0x86400000);
 setup();target_ptr=0;assert(J250fMapRpmSmem()==EFI_SUCCESS && !info_reads && attr_calls==1);
 setup();system_memory=1;assert(J250fMapRpmSmem()==EFI_ACCESS_DENIED && !info_reads && !configured_base);
 setup();nonexistent=1;assert(J250fMapRpmSmem()==EFI_SUCCESS && add_calls==2);
 setup();nonexistent=add_fail=1;assert(J250fMapRpmSmem()==EFI_DEVICE_ERROR && !info_reads);
 setup();smem_size=0xffffffff;assert(J250fMapRpmSmem()==EFI_COMPROMISED_DATA && !configured_base);
 setup();smem_base=0x99300000;system_memory=2;
 assert(J250fMapRpmSmem()==EFI_ACCESS_DENIED && !configured_base);
 puts("PASS: SMEM target-info (32-bit phys_addr_t), relocation, DT fallback, GCD mapping, allocated RAM rejection, bounds and mapping failure propagation");
}
