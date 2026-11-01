// Native tests of the actual RPM transport, with simulated remote responses.
#include <stdio.h>
#include <assert.h>
#include <string.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/PlatformIntegration/RpmSmd.c"
#include "../Platforms/Samsung/J250fPkg/PlatformIntegration/J250fPowerDxe.c"
static UINT8 memory[0x100000], auxiliary[AUX_SIZE];
static UINTN hdr, fifo;
static int packet_mode, no_ack, nack, wrong_id, doorbells, no_close_ack, no_open_ack;
static UINT32 requests, observed_type, observed_id, observed_count, observed_values[8];
static UINT8 *ptr(UINTN a,UINTN n){
 if(a>=SMEM && a-SMEM<=SMEM_SIZE && n<=SMEM_SIZE-(a-SMEM))return memory+a-SMEM;
 if(a>=AUX && a-AUX<=AUX_SIZE && n<=AUX_SIZE-(a-AUX))return auxiliary+a-AUX;
 assert(!"out-of-bounds or unexpected MMIO");return 0;
}
static UINT32 raw(UINTN a){UINT32 v;memcpy(&v,ptr(a,4),4);return v;}
static void put(UINTN a,UINT32 v){memcpy(ptr(a,4),&v,4);}
static void put16(UINTN a,UINT16 v){memcpy(ptr(a,2),&v,2);}
UINT8 EFIAPI MmioRead8(UINTN a){return *ptr(a,1);}
UINT16 EFIAPI MmioRead16(UINTN a){UINT16 v;memcpy(&v,ptr(a,2),2);return v;}
UINT32 EFIAPI MmioRead32(UINTN a){return raw(a);}
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
UINTN EFIAPI MicroSecondDelay(UINTN n){return n;}
static UINT32 txword(UINT32 tail,UINT32 off){return raw(fifo+(tail+off)%512);}
static void remote(void){
 if(raw(hdr)==0){if(!no_close_ack)put(hdr+HALF,4);return;}
 if(raw(hdr)==1){
  if(no_open_ack)return;
  put(hdr+TAIL,0);put(hdr+HALF+HEAD,0);put(hdr+HALF,2);return;
 }
 if(raw(hdr)!=2)return;
 UINT32 tail=raw(hdr+TAIL),head=raw(hdr+HEAD);
 if(tail==head)return;
 UINT32 prefix=packet_mode?20:0;
 UINT32 bytes=packet_mode?txword(tail,0):txword(tail,4)+8;
 assert(txword(tail,prefix)==0x716572 && bytes>=40);
 assert(txword(tail,prefix+12)==0); // active set
 observed_type=txword(tail,prefix+16);observed_id=txword(tail,prefix+20);
 observed_count=txword(tail,prefix+24)/12;
 assert(bytes==28+observed_count*12);
 for(UINT32 i=0;i<observed_count;i++){assert(txword(tail,prefix+32+i*12)==4);observed_values[i]=txword(tail,prefix+36+i*12);}
 UINT32 id=txword(tail,prefix+8);requests++;
 put(hdr+TAIL,head);
 if(no_ack)return;
 UINT32 ack[13]={0};UINT32 ackbytes=nack?32:20;
 unsigned start=0;
 if(packet_mode){ack[0]=ackbytes;start=5;}
 ack[start]=0x716572;ack[start+1]=ackbytes-8;
 ack[start+2]=0x2367736d;ack[start+3]=4;ack[start+4]=wrong_id?id+1:id;
 UINT32 rh=raw(hdr+HALF+HEAD);
 for(unsigned i=0;i<(ackbytes+prefix)/4;i++){put(fifo+512+rh,ack[i]);rh=(rh+4)%512;}
 put(hdr+HALF+HEAD,rh);
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v){
 if(a==DOORBELL){assert(v==1);doorbells++;remote();return v;}
 put(a,v);return v;
}
static void heapitem(UINT32 id,UINT32 off,UINT32 size){
 UINTN e=SMEM+0xd0+id*16;put(e,1);put(e+4,off);put(e+8,size);put(e+12,0);
}
static void channel(UINTN table){
 memcpy(ptr(table,13),"rpm_requests",13);
 put(table+20,3);put(table+24,(packet_mode?0x200:0x100)|15);put(table+28,1);
}
static void setup(int packet){
 memset(memory,0,sizeof(memory));memset(auxiliary,0,sizeof(auxiliary));
 packet_mode=packet;no_ack=nack=wrong_id=doorbells=no_close_ack=no_open_ack=0;requests=0;
 put(SMEM+0xc0,1);heapitem(13,0x4000,32);heapitem(17,0x5000,88);heapitem(341,0x6000,1024);
 channel(SMEM+0x4000);hdr=SMEM+0x5000;fifo=SMEM+0x6000;
}
static UINTN alloc_item(UINTN part,UINT32 *off,UINT16 id,UINT32 n){
 UINTN e=part+*off;put16(e,0xa5a5);put16(e+2,id);put(e+4,n);
 *off+=16+n;return e+16;
}
static void secure_setup(void){
 setup(1);UINTN part=SMEM+0x20000;UINT32 off=32;
 put(TOC,0x434f5424);put(TOC+4,1);put(TOC+8,1);
 put(TOC+32,0x20000);put(TOC+36,0x10000);put16(TOC+44,0);put16(TOC+46,6);put(TOC+48,64);
 put(part,0x54525024);put16(part+4,0);put16(part+6,6);put(part+8,0x10000);
 UINTN table=alloc_item(part,&off,13,32);
 hdr=alloc_item(part,&off,17,88);fifo=alloc_item(part,&off,341,1024);
 channel(table);put(part+12,off);
}
int main(void){
 UINT32 keys[3]={0x6e657773,0x7675,0x616d},values[3]={1,2900000,570};
 for(int packet=0;packet<2;packet++){
  setup(packet);assert(J250fRpmOpen()==EFI_SUCCESS);
  assert(J250fRpmVote(0x616f646c,8,keys,values,3)==EFI_SUCCESS);
  assert(requests==1 && observed_type==0x616f646c && observed_id==8 && observed_count==3);
  // Exercise circular FIFOs across wrap boundaries.
  for(int i=0;i<30;i++)assert(J250fRpmVote(0x616f646c,5,keys,values,3)==EFI_SUCCESS);
 }
 secure_setup();assert(J250fRpmOpen()==EFI_SUCCESS);
 assert(J250fRpmVote(0x616f646c,8,keys,values,3)==EFI_SUCCESS);
 setup(1);put(SMEM+0xd0+17*16+8,80);assert(J250fRpmOpen()==EFI_COMPROMISED_DATA);
 setup(1);put(hdr,2);put(hdr+HALF,2);put(hdr+HEAD,4);put(hdr+HALF+HEAD,40);
 assert(J250fRpmOpen()==EFI_SUCCESS && !requests); // stale bootloader FIFOs reclaimed via close/open
 assert(J250fRpmVote(0x616f646c,8,keys,values,3)==EFI_SUCCESS);
 setup(1);put(hdr,2);put(hdr+HALF,2);no_close_ack=1;
 assert(J250fRpmOpen()==EFI_TIMEOUT && !requests);
 setup(1);no_open_ack=1;assert(J250fRpmOpen()==EFI_TIMEOUT && !requests);
 setup(1);assert(J250fRpmOpen()==EFI_SUCCESS);no_ack=1;
 assert(J250fRpmVote(1,1,keys,values,1)==EFI_TIMEOUT);
 assert(J250fRpmVote(1,1,keys,values,1)==EFI_NOT_READY && requests==1);
 setup(1);assert(J250fRpmOpen()==EFI_SUCCESS);nack=1;
 assert(J250fRpmVote(1,1,keys,values,1)==EFI_DEVICE_ERROR);
 setup(1);assert(J250fRpmOpen()==EFI_SUCCESS);wrong_id=1;
 assert(J250fRpmVote(1,1,keys,values,1)==EFI_COMPROMISED_DATA);
 setup(1);put(TOC,0x434f5424);put(TOC+4,1);put(TOC+8,1000);
 assert(J250fRpmOpen()==EFI_COMPROMISED_DATA);
 setup(1);assert(J250fRpmOpen()==EFI_SUCCESS);
 assert(SetRails(&mPower,TRUE,2900000,1800000)==EFI_SUCCESS);
 assert(requests==2 && observed_type==0x616f646c && observed_id==5);
 assert(observed_values[0]==1 && observed_values[1]==1800000 && observed_values[2]==325);
 assert(SetRails(&mPower,FALSE,2900000,1800000)==EFI_SUCCESS);
 assert(observed_values[0]==1 && observed_values[2]==0); // always-on stays on
 assert(SetRails(&mPower,TRUE,3300000,1800000)==EFI_INVALID_PARAMETER && requests==4);
 assert(GpllReady(&mPower)==EFI_UNSUPPORTED);
 setup(1);assert(J250fRpmOpen()==EFI_SUCCESS);nack=1;
 assert(SetRails(&mPower,TRUE,2900000,1800000)==EFI_DEVICE_ERROR && requests==1);
 puts("PASS: provider rail votes require matching ACK, always-on/load/voltage settings, failed vote propagation, no fake PLL readiness");
 puts("PASS: simulated RPM transport packet/stream framing, secure/global SMEM, wraparound, ACK/NACK/ID matching, bounds, peer-acknowledged handoff, bounded timeout and fault latch");
}
