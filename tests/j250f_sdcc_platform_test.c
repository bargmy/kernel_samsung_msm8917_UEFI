// Test the source-ported clock/pin implementation against register semantics.
#include <assert.h>
#include <stdio.h>
#undef NULL
#include "../GPLDrivers/Library/J250fSdccPlatformLib/J250fSdccPlatformLib.c"
static UINT32 regs[8], pads;
static int update_stuck, branch_stuck;
static UINT32 *address(UINTN a) {
 if(a==PADS)return &pads;
 assert(a>=CMD && a<=AHB && !(a&3));return &regs[(a-CMD)/4];
}
UINT32 EFIAPI MmioRead32(UINTN a){return *address(a);}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v){
 if(a==CMD && !update_stuck)v&=~1U;
 if((a==APPS||a==AHB)&&branch_stuck)v|=1U<<31;
 *address(a)=v;return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n){return n;}
int main(void){
 assert(J250fSdccSetIdentificationClock()==EFI_SUCCESS);
 assert(regs[2]==1 && regs[3]==~3U && regs[4]==~4U);
 assert((regs[1]&0x371f)==0x2017);
 assert((regs[5]&1) && (regs[6]&1));
 pads=0xa5a50000;UINT32 prior=pads;
 assert(J250fSdccSetActivePads()==EFI_SUCCESS);
 UINT32 mask=(3U<<13)|(3U<<11)|(3U<<9)|(3U<<15)|(7U<<6)|(7U<<3)|7;
 assert((pads&~mask)==(prior&~mask));
 assert(((pads>>6)&7)==7 && ((pads>>3)&7)==4 && (pads&7)==4);
 assert(((pads>>11)&3)==3 && ((pads>>9)&3)==3 && ((pads>>15)&3)==1);
 update_stuck=1;assert(J250fSdccSetIdentificationClock()==EFI_TIMEOUT);
 update_stuck=0;branch_stuck=1;assert(J250fSdccSetIdentificationClock()==EFI_TIMEOUT);
 puts("PASS: Samsung 400kHz M/N/D recipe, source/divider fields, branches, pad config/preservation, timeout propagation");
}
