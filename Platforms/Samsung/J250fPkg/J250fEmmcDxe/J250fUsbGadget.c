/** @file
  Experimental MSM8917 USB2 ChipIdea control-only gadget for Project Mu.
  Standalone EP0 vendor requests: no UsbFnIo provider is needed.
  Physical controller base and operations cross-checked with Qualcomm LK
  platform/msm_shared/hsusb.{c,h} and target/msm8952/init.c.
  NO arbitrary memory access, flash writes, bulk DMA or USB flashing.
  PHY, clocks and regulators must be kept powered by the preceding bootloader;
  this file does not implement GCC/PMIC USB bringup. GPL-2.0-only.
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include "J250fUsbGadget.h"

#define BASE             ((UINTN)0x078DB000)
#define USBCMD           0x140
#define USBSTS           0x144
#define USBINTR          0x148
#define DEVICEADDR       0x154
#define ENDPOINTLIST     0x158
#define ULPI             0x170
#define PORTSC           0x184
#define GENCONFIG2       0x0a0
#define USBMODE          0x1a8
#define SETUPSTAT        0x1ac
#define EPPRIME          0x1b0
#define EPFLUSH          0x1b4
#define EPCOMPLETE       0x1bc
#define EPCTRL0          0x1c0
#define STS_RESET        (1u<<6)
#define STS_PORT_CHANGE  (1u<<2)
#define STS_USB          1u
#define STS_ERROR        (1u<<1)
#define CMD_RUN          1u
#define CMD_RESET        2u
#define CMD_TRIPWIRE     (1u<<13)
#define CMD_ITC          (1u<<19)
#define CMD_SESSVALID    (1u<<25)
#define QH_IOS           (1u<<15)
#define QH_ZLT           (1u<<29)
#define TD_ACTIVE        (1u<<7)
#define TD_IOC           (1u<<15)
#define TD_TERMINATE     1u
#define RX0              1u
#define TX0              (1u<<16)
#define MAX_DATA         512u
/* Test VID/PID: REPLACE with an authorized VID/PID for distribution. */
#ifndef J250F_USB_TEST_VID
#define J250F_USB_TEST_VID 0x1209
#endif
#ifndef J250F_USB_TEST_PID
#define J250F_USB_TEST_PID 0x8917
#endif

#pragma pack(1)
typedef struct {UINT8 Type, Request; UINT16 Value, Index, Length;} USB_SETUP;
#pragma pack()
typedef struct {
  volatile UINT32 Config, Current, Next, Info, Page[5], Reserved0;
  volatile UINT8 Setup[8];
  volatile UINT32 Reserved[4];
} USB_QH;
typedef struct {
  volatile UINT32 Next, Info, Page[5], Reserved;
} USB_TD;
/* A page for 32 queue heads and a page for TDs; DMA buffers start page 3. */
STATIC USB_QH *mQh;
STATIC USB_TD *mTdIn, *mTdOut;
STATIC UINT8 *mBuffer;
STATIC EFI_PHYSICAL_ADDRESS mDma;
STATIC BOOLEAN mStarted, mTxPending, mNeedStatusOut;
STATIC UINT8 mConfiguration;
STATIC J250F_USB_DIAG_COMMAND mCommand;

STATIC CONST UINT8 mDevice[] = {
  18,1,0x00,0x02,0,0,0,64,
  (UINT8)J250F_USB_TEST_VID,(UINT8)(J250F_USB_TEST_VID>>8),
  (UINT8)J250F_USB_TEST_PID,(UINT8)(J250F_USB_TEST_PID>>8),
  0x01,0x00,1,2,0,1
};
/* Single vendor-specific interface, no non-control endpoints. */
STATIC CONST UINT8 mConfig[] = {
  9,2,18,0,1,1,0,0x80,50,
  9,4,0,0,0,0xff,0,0,0
};
STATIC CONST UINT8 mLang[] = {4,3,9,4};
STATIC CONST UINT8 mManufacturer[] = {
  18,3,'J',0,'2',0,'5',0,'0',0,'F',0,' ',0,'M',0,'u',0
};
STATIC CONST UINT8 mProduct[] = {
  34,3,'U',0,'S',0,'B',0,' ',0,'D',0,'e',0,'b',0,'u',0,'g',0,
  ' ',0,'(',0,'t',0,'e',0,'s',0,'t',0,')',0
};

STATIC UINT32 R(UINTN Offset) { return MmioRead32(BASE+Offset); }
STATIC VOID W(UINTN Offset,UINT32 Value){MmioWrite32(BASE+Offset,Value);MemoryFence();}
STATIC VOID Flush(VOID *P,UINTN N) {WriteBackInvalidateDataCacheRange(P,N);MemoryFence();}
STATIC VOID Invalidate(VOID *P,UINTN N){InvalidateDataCacheRange(P,N);MemoryFence();}
STATIC EFI_STATUS WaitMask(UINTN Offset,UINT32 Mask,UINT32 Expected,UINT32 Tries){
  UINT32 I;
  for(I=0;I<Tries;++I){if((R(Offset)&Mask)==Expected)return EFI_SUCCESS;gBS->Stall(10);}
  return EFI_TIMEOUT;
}
STATIC VOID SetupHeads(VOID){
  ZeroMem(mQh,2048);
  mQh[0].Config=(64u<<16)|QH_IOS|QH_ZLT;
  mQh[1].Config=(64u<<16)|QH_ZLT;
  mQh[0].Next=mQh[1].Next=TD_TERMINATE;
  Flush(mQh,2048);
  mTxPending=FALSE;mNeedStatusOut=FALSE;
  mConfiguration=0;
  W(ENDPOINTLIST,(UINT32)mDma);
  W(EPCTRL0,(1u<<23)|(1u<<7));
}
STATIC EFI_STATUS Prime(UINT32 Bit,USB_TD *Td,USB_QH *Qh,UINTN Length){
  UINTN I;
  if(Length>MAX_DATA)return EFI_BAD_BUFFER_SIZE;
  ZeroMem(Td,sizeof(*Td));
  Td->Next=TD_TERMINATE;
  Td->Info=((UINT32)Length<<16)|TD_IOC|TD_ACTIVE;
  for(I=0;I<5;I++)Td->Page[I]=(UINT32)((mDma+8192)&~0xfffULL)+(UINT32)(I*4096);
  Td->Page[0]=(UINT32)(mDma+8192);
  Qh->Next=(UINT32)(mDma+4096+(Bit==TX0?64:0));
  Qh->Info=0;
  Flush(mBuffer,MAX_DATA);
  Flush(Td,sizeof(*Td));
  Flush(Qh,sizeof(*Qh));
  W(EPPRIME,Bit);
  return WaitMask(EPPRIME,Bit,0,10000);
}
STATIC EFI_STATUS SendIn(UINTN Length,BOOLEAN StatusOut){
  EFI_STATUS S=Prime(TX0,mTdIn,&mQh[1],Length);
  if(!EFI_ERROR(S)){mTxPending=TRUE;mNeedStatusOut=StatusOut;}
  return S;
}
STATIC VOID StallEp0(VOID){W(EPCTRL0,R(EPCTRL0)|(1u<<16)|1u);}
STATIC BOOLEAN ReadSetup(USB_SETUP *Out){
  UINT32 I; UINT8 Raw[8];
  for(I=0;I<50;I++){
    W(USBCMD,R(USBCMD)|CMD_TRIPWIRE);
    Invalidate(mQh,2048);
    CopyMem(Raw,(CONST VOID*)mQh[0].Setup,8);
    if(R(USBCMD)&CMD_TRIPWIRE){W(USBCMD,R(USBCMD)&~CMD_TRIPWIRE);CopyMem(Out,Raw,8);return TRUE;}
  }
  return FALSE;
}
STATIC VOID HandleSetup(VOID){
  USB_SETUP S; CONST UINT8 *Source=NULL; UINTN N=0; UINTN Limit;
  EFI_STATUS Status;
  if(!ReadSetup(&S)){StallEp0();return;}
  W(SETUPSTAT,RX0);
  /* New SETUP cancels the previous control transfer. */
  W(EPFLUSH,TX0|RX0);
  if(EFI_ERROR(WaitMask(EPFLUSH,TX0|RX0,0,10000))){StallEp0();return;}
  mTxPending=FALSE;mNeedStatusOut=FALSE;
  /* A fresh control SETUP clears any previous EP0 stall. */
  W(EPCTRL0,R(EPCTRL0)&~((1u<<16)|1u));
  if(S.Type==0x80 && S.Request==6){
    switch(S.Value){
      case 0x0100:Source=mDevice;N=sizeof(mDevice);break;
      case 0x0200:Source=mConfig;N=sizeof(mConfig);break;
      case 0x0300:Source=mLang;N=sizeof(mLang);break;
      case 0x0301:Source=mManufacturer;N=sizeof(mManufacturer);break;
      case 0x0302:Source=mProduct;N=sizeof(mProduct);break;
      default:break;
    }
  } else if(S.Type==0x80 && S.Request==8){mBuffer[0]=mConfiguration;N=1;}
  else if(S.Type==0x80 && S.Request==0){mBuffer[0]=0;mBuffer[1]=0;N=2;}
  else if(S.Type==0x81 && S.Request==10){mBuffer[0]=0;N=1;}
  else if(S.Type==0x00 && S.Request==5 && S.Value<=127 && S.Length==0){
    W(DEVICEADDR,((UINT32)S.Value<<25)|(1u<<24));(VOID)SendIn(0,FALSE);return;
  }else if(S.Type==0x00 && S.Request==9 && S.Value<=1 && S.Length==0){
    mConfiguration=(UINT8)S.Value;(VOID)SendIn(0,FALSE);return;
  }else if(S.Type==0xC0 && mConfiguration==1 && mCommand!=NULL && S.Length<=MAX_DATA){
    Limit=MAX_DATA;
    Status=mCommand(S.Request,S.Value,S.Index,mBuffer,&Limit);
    if(!EFI_ERROR(Status) && Limit<=MAX_DATA)N=Limit;
    else {StallEp0();return;}
  }else {StallEp0();return;}
  if(N==0){StallEp0();return;}
  if(N>S.Length)N=S.Length;
  if(N>MAX_DATA)N=MAX_DATA;
  if(Source!=NULL)CopyMem(mBuffer,Source,N);
  (VOID)SendIn(N,TRUE);
}
STATIC EFI_STATUS UlpiWrite(UINT8 Reg,UINT8 Value){
  UINT32 I;
  W(ULPI,(1u<<30)|(1u<<29)|((UINT32)Reg<<16)|Value);
  for(I=0;I<10000;I++){if(!(R(ULPI)&(1u<<30)))return EFI_SUCCESS;gBS->Stall(10);}
  return EFI_TIMEOUT;
}
/* Do not release DMA storage until endpoints are flushed or reset completes.
 * Reserved pages remain reserved if hardware cannot be stopped. */
STATIC EFI_STATUS Quiesce(VOID){
  EFI_STATUS S;
  W(USBINTR,0);
  W(USBCMD,R(USBCMD)&~CMD_RUN);
  W(EPFLUSH,0xffffffffu);
  S=WaitMask(EPFLUSH,0xffffffffu,0,10000);
  if(EFI_ERROR(S)){
    W(USBCMD,(R(USBCMD)&~CMD_RUN)|CMD_RESET);
    S=WaitMask(USBCMD,CMD_RESET,0,20000);
  }
  return S;
}
EFI_STATUS J250fUsbGadgetStart(J250F_USB_DIAG_COMMAND Command){
  EFI_STATUS S;
  EFI_PHYSICAL_ADDRESS Addr=0xffffffffULL;
  if(mStarted)return EFI_ALREADY_STARTED;
  if(Command==NULL)return EFI_INVALID_PARAMETER;
  /* No ability to enable clocks/PHY: don't reset a dead controller. */
  if(R(0x000)==0xffffffffu||R(0x00c)==0xffffffffu ||
     (R(0x00c)&0xffu)==0)return EFI_NOT_READY;
  S=gBS->AllocatePages(AllocateMaxAddress,EfiReservedMemoryType,3,&Addr);
  if(EFI_ERROR(S))return S;
  mDma=Addr;
  mQh=(USB_QH*)(UINTN)Addr;
  mTdOut=(USB_TD*)(UINTN)(Addr+4096);
  mTdIn=(USB_TD*)(UINTN)(Addr+4096+64);
  mBuffer=(UINT8*)(UINTN)(Addr+8192);
  mCommand=Command;
  /* LK's MSM8952 ordering: reset -> ULPI selection -> external VBUS -> device mode. */
  W(USBCMD,CMD_ITC|CMD_RESET);
  S=WaitMask(USBCMD,CMD_RESET,0,20000);
  if(EFI_ERROR(S))goto Fail;
  W(PORTSC,0x80000000u);
  S=UlpiWrite(0x97,0x03); /* ULPI MISC_A_SET: VBUSVALID externally asserted */
  if(EFI_ERROR(S))goto Fail;
  W(GENCONFIG2,R(GENCONFIG2)|(1u<<7));
  W(USBCMD,R(USBCMD)|CMD_SESSVALID);
  W(0x090,0);
  W(0x098,8);
  W(USBMODE,2);
  SetupHeads();
  W(EPCOMPLETE,R(EPCOMPLETE));
  W(SETUPSTAT,R(SETUPSTAT));
  W(EPFLUSH,0xffffffffu);
  S=WaitMask(EPFLUSH,0xffffffffu,0,10000);
  if(EFI_ERROR(S))goto Fail;
  W(USBSTS,R(USBSTS));
  W(USBINTR,0); /* polling; no GIC handler required */
  W(USBCMD,R(USBCMD)|CMD_ITC|CMD_RUN);
  mStarted=TRUE;
  return EFI_SUCCESS;
Fail:
  mCommand=NULL;
  if(!EFI_ERROR(Quiesce()))gBS->FreePages(Addr,3);
  else DEBUG((DEBUG_ERROR,"J250F USB: DMA stop failed; reserved pages retained\n"));
  return S;
}
EFI_STATUS J250fUsbGadgetPoll(VOID){
  UINT32 Events,Complete,Pending;
  if(!mStarted)return EFI_NOT_READY;
  Events=R(USBSTS);
  if(Events)W(USBSTS,Events);
  if(Events&STS_RESET){
    W(EPCOMPLETE,R(EPCOMPLETE));
    W(SETUPSTAT,R(SETUPSTAT));
    W(EPFLUSH,0xffffffffu);
    if(EFI_ERROR(WaitMask(EPFLUSH,0xffffffffu,0,10000)))return EFI_TIMEOUT;
    SetupHeads();
  }
  Pending=R(SETUPSTAT);
  if(Pending&RX0)HandleSetup();
  Complete=R(EPCOMPLETE);
  if(Complete)W(EPCOMPLETE,Complete);
  if(mTxPending&&(Complete&TX0)){
    mTxPending=FALSE;
    Invalidate(mTdIn,sizeof(*mTdIn));
    if((mTdIn->Info&0x68u)!=0){StallEp0();return EFI_DEVICE_ERROR;}
    if(mNeedStatusOut){mNeedStatusOut=FALSE;return Prime(RX0,mTdOut,&mQh[0],0);}
  }
  (VOID)Events;
  return EFI_SUCCESS;
}
VOID J250fUsbGadgetStop(VOID){
  if(!mStarted)return;
  if(!EFI_ERROR(Quiesce()))gBS->FreePages(mDma,3);
  else DEBUG((DEBUG_ERROR,"J250F USB: DMA stop failed; reserved pages retained\n"));
  mStarted=FALSE;mCommand=NULL;mBuffer=NULL;mQh=NULL;
}
