/** @file
  Experimental MSM8917 ChipIdea USB2 ADB gadget for Project Mu.
  EP0 enumeration/Windows descriptors and asynchronous bulk IN/OUT transport.
  PHY, clocks and rails still require the preceding bootloader handoff.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include "J250fUsbGadget.h"
#include "J250fAdbShell.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Uefi.h>

#define BASE ((UINTN)0x078DB000)
#define USBCMD 0x140
#define USBSTS 0x144
#define USBINTR 0x148
#define DEVICEADDR 0x154
#define ENDPOINTLIST 0x158
#define ULPI 0x170
#define PORTSC 0x184
#define GENCONFIG2 0x0a0
#define USBMODE 0x1a8
#define SETUPSTAT 0x1ac
#define EPPRIME 0x1b0
#define EPFLUSH 0x1b4
#define EPCOMPLETE 0x1bc
#define EPCTRL0 0x1c0
#define STS_RESET (1u << 6)
#define STS_PORT_CHANGE (1u << 2)
#define STS_USB 1u
#define STS_ERROR (1u << 1)
#define CMD_RUN 1u
#define CMD_RESET 2u
#define CMD_TRIPWIRE (1u << 13)
#define CMD_ITC (1u << 19)
#define CMD_SESSVALID (1u << 25)
#define QH_IOS (1u << 15)
#define QH_ZLT (1u << 29)
#define TD_ACTIVE (1u << 7)
#define TD_IOC (1u << 15)
#define TD_TERMINATE 1u
#define RX0 1u
#define TX0 (1u << 16)
#define MAX_DATA 512u
/* Test VID/PID: REPLACE with an authorized VID/PID for distribution. */
#ifndef J250F_USB_TEST_VID
#define J250F_USB_TEST_VID 0x1209
#endif
#ifndef J250F_USB_TEST_PID
#define J250F_USB_TEST_PID 0x8918
#endif

#pragma pack(1)
typedef struct {
  UINT8 Type, Request;
  UINT16 Value, Index, Length;
} USB_SETUP;
#pragma pack()
typedef struct {
  volatile UINT32 Config, Current, Next, Info, Page[5], Reserved0;
  volatile UINT8 Setup[8];
  volatile UINT32 Reserved[4];
} USB_QH;
typedef struct {
  volatile UINT32 Next, Info, Page[5], Reserved;
} USB_TD;
STATIC_ASSERT(sizeof(USB_QH) == 64, "USB queue head size");
STATIC_ASSERT(sizeof(USB_TD) == 32, "USB transfer descriptor size");
/* Five pages: queue heads, TDs, EP0 buffer, bulk OUT, bulk IN. */
STATIC USB_QH *mQh;
STATIC USB_TD *mTdIn, *mTdOut;
STATIC UINT8 *mBuffer;
STATIC USB_TD *mBulkOut, *mBulkIn;
STATIC UINT8 *mBulkRxBuffer, *mBulkTxBuffer;
STATIC BOOLEAN mBulkRxArmed;
STATIC UINTN mBulkTxStage, mBulkPacketSize;
STATIC UINT8 mBulkPacket[JADB_MAX + 24];
STATIC jadb mAdb;
STATIC jadb_ops mAdbOps;
#define RX1 (1u << 1)
#define TX1 (1u << 17)
STATIC EFI_STATUS BulkConfigure(VOID);
STATIC EFI_STATUS BulkPoll(UINT32 Complete);
STATIC EFI_PHYSICAL_ADDRESS mDma;
STATIC BOOLEAN mStarted, mTxPending, mNeedStatusOut, mDmaQuarantined;
STATIC UINT8 mConfiguration;
STATIC J250F_USB_DIAG_COMMAND mCommand;

STATIC CONST UINT8 mDevice[] = {18,
                                1,
                                0x00,
                                0x02,
                                0,
                                0,
                                0,
                                64,
                                (UINT8)J250F_USB_TEST_VID,
                                (UINT8)(J250F_USB_TEST_VID >> 8),
                                (UINT8)J250F_USB_TEST_PID,
                                (UINT8)(J250F_USB_TEST_PID >> 8),
                                0x01,
                                0x00,
                                1,
                                2,
                                3,
                                1};
/* ADB interface: vendor class, ADB subclass/protocol, bulk OUT/IN. */
STATIC UINT8 mConfig[] = {
    9, 2, 32, 0, 1,    1, 0,    0x80, 50, 9, 4, 0,    0, 2,    0xff, 0x42,
    1, 0, 7,  5, 0x01, 2, 0x00, 0x02, 0,  7, 5, 0x81, 2, 0x00, 0x02, 0};
STATIC CONST UINT8 mQualifier[] = {10, 6, 0, 2, 0, 0, 0, 64, 1, 0};
STATIC CONST UINT8 mSerial[] = {22,  3, 'J', 0, '2', 0, '5', 0, '0', 0, 'F', 0,
                                '-', 0, 'U', 0, 'E', 0, 'F', 0, 'I', 0};

/* Microsoft OS 1.0 descriptors: bind WinUSB and expose the ADB GUID. */
STATIC CONST UINT8 mMsString[] = {0x12, 0x03, 0x4d, 0x00, 0x53, 0x00,
                                  0x46, 0x00, 0x54, 0x00, 0x31, 0x00,
                                  0x30, 0x00, 0x30, 0x00, 0x20, 0x00};
STATIC CONST UINT8 mMsCompat[] = {
    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x57, 0x49,
    0x4e, 0x55, 0x53, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
STATIC CONST UINT8 mMsProperties[] = {
    0x92, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x01, 0x00, 0x88, 0x00,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x44, 0x00, 0x65, 0x00,
    0x76, 0x00, 0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x49, 0x00, 0x6e, 0x00,
    0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00,
    0x65, 0x00, 0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00,
    0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x7b, 0x00, 0x46, 0x00, 0x37, 0x00,
    0x32, 0x00, 0x46, 0x00, 0x45, 0x00, 0x30, 0x00, 0x44, 0x00, 0x34, 0x00,
    0x2d, 0x00, 0x43, 0x00, 0x42, 0x00, 0x43, 0x00, 0x42, 0x00, 0x2d, 0x00,
    0x34, 0x00, 0x30, 0x00, 0x37, 0x00, 0x44, 0x00, 0x2d, 0x00, 0x38, 0x00,
    0x38, 0x00, 0x31, 0x00, 0x34, 0x00, 0x2d, 0x00, 0x39, 0x00, 0x45, 0x00,
    0x44, 0x00, 0x36, 0x00, 0x37, 0x00, 0x33, 0x00, 0x44, 0x00, 0x30, 0x00,
    0x44, 0x00, 0x44, 0x00, 0x36, 0x00, 0x42, 0x00, 0x7d, 0x00, 0x00, 0x00,
    0x00, 0x00};
STATIC CONST UINT8 mLang[] = {4, 3, 9, 4};
STATIC CONST UINT8 mManufacturer[] = {18,  3, 'J', 0, '2', 0, '5', 0, '0', 0,
                                      'F', 0, ' ', 0, 'M', 0, 'u', 0};
STATIC CONST UINT8 mProduct[] = {30, 3, 74, 0, 50, 0, 53, 0, 48, 0,
                                 70, 0, 32, 0, 85, 0, 69, 0, 70, 0,
                                 73, 0, 32, 0, 65, 0, 68, 0, 66, 0};

STATIC UINT32 R(UINTN Offset) { return MmioRead32(BASE + Offset); }
STATIC VOID W(UINTN Offset, UINT32 Value) {
  MmioWrite32(BASE + Offset, Value);
  MemoryFence();
}
STATIC VOID Flush(VOID *P, UINTN N) {
  WriteBackInvalidateDataCacheRange(P, N);
  MemoryFence();
}
STATIC VOID Invalidate(VOID *P, UINTN N) {
  InvalidateDataCacheRange(P, N);
  MemoryFence();
}
STATIC EFI_STATUS WaitMask(UINTN Offset, UINT32 Mask, UINT32 Expected,
                           UINT32 Tries) {
  UINT32 I;
  for (I = 0; I < Tries; ++I) {
    if ((R(Offset) & Mask) == Expected)
      return EFI_SUCCESS;
    gBS->Stall(10);
  }
  return EFI_TIMEOUT;
}
STATIC VOID SetupHeads(VOID) {
  ZeroMem(mQh, 2048);
  mQh[0].Config = (64u << 16) | QH_IOS | QH_ZLT;
  mQh[1].Config = (64u << 16) | QH_ZLT;
  mQh[0].Next = mQh[1].Next = TD_TERMINATE;
  Flush(mQh, 2048);
  mTxPending = FALSE;
  mNeedStatusOut = FALSE;
  mConfiguration = 0;
  mBulkRxArmed = FALSE;
  mBulkTxStage = 0;
  jadb_init(&mAdb, &mAdbOps);
  W(ENDPOINTLIST, (UINT32)mDma);
  W(EPCTRL0, (1u << 23) | (1u << 7));
}
STATIC EFI_STATUS Prime(UINT32 Bit, USB_TD *Td, USB_QH *Qh, UINTN Length) {
  UINTN I;
  if (Length > MAX_DATA)
    return EFI_BAD_BUFFER_SIZE;
  ZeroMem(Td, sizeof(*Td));
  Td->Next = TD_TERMINATE;
  Td->Info = ((UINT32)Length << 16) | TD_IOC | TD_ACTIVE;
  for (I = 0; I < 5; I++)
    Td->Page[I] = (UINT32)((mDma + 8192) & ~0xfffULL) + (UINT32)(I * 4096);
  Td->Page[0] = (UINT32)(mDma + 8192);
  Qh->Next = (UINT32)(mDma + 4096 + (Bit == TX0 ? 64 : 0));
  Qh->Info = 0;
  Flush(mBuffer, MAX_DATA);
  Flush(Td, sizeof(*Td));
  Flush(Qh, sizeof(*Qh));
  W(EPPRIME, Bit);
  return WaitMask(EPPRIME, Bit, 0, 10000);
}
STATIC EFI_STATUS SendIn(UINTN Length, BOOLEAN StatusOut) {
  EFI_STATUS S = Prime(TX0, mTdIn, &mQh[1], Length);
  if (!EFI_ERROR(S)) {
    mTxPending = TRUE;
    mNeedStatusOut = StatusOut;
  }
  return S;
}
STATIC VOID StallEp0(VOID) { W(EPCTRL0, R(EPCTRL0) | (1u << 16) | 1u); }
STATIC BOOLEAN ReadSetup(USB_SETUP *Out) {
  UINT32 I;
  UINT8 Raw[8];
  for (I = 0; I < 50; I++) {
    W(USBCMD, R(USBCMD) | CMD_TRIPWIRE);
    Invalidate(mQh, 2048);
    CopyMem(Raw, (CONST VOID *)mQh[0].Setup, 8);
    if (R(USBCMD) & CMD_TRIPWIRE) {
      W(USBCMD, R(USBCMD) & ~CMD_TRIPWIRE);
      CopyMem(Out, Raw, 8);
      return TRUE;
    }
  }
  return FALSE;
}
STATIC VOID HandleSetup(VOID) {
  USB_SETUP S;
  CONST UINT8 *Source = NULL;
  UINTN N = 0;
  UINTN Limit;
  EFI_STATUS Status;
  if (!ReadSetup(&S)) {
    StallEp0();
    return;
  }
  W(SETUPSTAT, RX0);
  /* New SETUP cancels the previous control transfer. */
  W(EPFLUSH, TX0 | RX0);
  if (EFI_ERROR(WaitMask(EPFLUSH, TX0 | RX0, 0, 10000))) {
    StallEp0();
    return;
  }
  mTxPending = FALSE;
  mNeedStatusOut = FALSE;
  /* A fresh control SETUP clears any previous EP0 stall. */
  W(EPCTRL0, R(EPCTRL0) & ~((1u << 16) | 1u));
  if (S.Type == 0x80 && S.Request == 6) {
    switch (S.Value) {
    case 0x0100:
      Source = mDevice;
      N = sizeof(mDevice);
      break;
    case 0x0200:
    case 0x0700: {
      UINT16 Mps = (((R(PORTSC) >> 26) & 3u) == 2u) ? 512 : 64;
      if (S.Value == 0x0700)
        Mps = Mps == 512 ? 64 : 512;
      mConfig[1] = (S.Value == 0x0700) ? 7 : 2;
      mConfig[22] = mConfig[29] = (UINT8)Mps;
      mConfig[23] = mConfig[30] = (UINT8)(Mps >> 8);
      Source = mConfig;
      N = sizeof(mConfig);
      break;
    }
    case 0x0600:
      Source = mQualifier;
      N = sizeof(mQualifier);
      break;
    case 0x03ee:
      Source = mMsString;
      N = sizeof(mMsString);
      break;
    case 0x0300:
      Source = mLang;
      N = sizeof(mLang);
      break;
    case 0x0301:
      Source = mManufacturer;
      N = sizeof(mManufacturer);
      break;
    case 0x0302:
      Source = mProduct;
      N = sizeof(mProduct);
      break;
    case 0x0303:
      Source = mSerial;
      N = sizeof(mSerial);
      break;
    default:
      break;
    }
  } else if (S.Type == 0xc0 && S.Request == 0x20 && S.Index == 4) {
    Source = mMsCompat;
    N = sizeof(mMsCompat);
  } else if ((S.Type == 0xc0 || S.Type == 0xc1) && S.Request == 0x20 &&
             S.Index == 5) {
    Source = mMsProperties;
    N = sizeof(mMsProperties);
  } else if (S.Type == 0x80 && S.Request == 8) {
    mBuffer[0] = mConfiguration;
    N = 1;
  } else if (S.Type == 0x80 && S.Request == 0) {
    mBuffer[0] = 0;
    mBuffer[1] = 0;
    N = 2;
  } else if (S.Type == 0x81 && S.Request == 10) {
    mBuffer[0] = 0;
    N = 1;
  } else if (S.Type == 0x00 && S.Request == 5 && S.Value <= 127 &&
             S.Length == 0) {
    W(DEVICEADDR, ((UINT32)S.Value << 25) | (1u << 24));
    (VOID) SendIn(0, FALSE);
    return;
  } else if (S.Type == 0x00 && S.Request == 9 && S.Value <= 1 &&
             S.Length == 0) {
    mConfiguration = (UINT8)S.Value;
    if (EFI_ERROR(BulkConfigure())) {
      StallEp0();
      return;
    }
    (VOID) SendIn(0, FALSE);
    return;
  } else if (S.Type == 0x01 && S.Request == 11 && S.Value == 0 &&
             S.Index == 0 && S.Length == 0) {
    (VOID) SendIn(0, FALSE);
    return;
  } else if (S.Type == 0x82 && S.Request == 0 &&
             (S.Index == 1 || S.Index == 0x81)) {
    mBuffer[0] = mBuffer[1] = 0;
    N = 2;
  } else if (S.Type == 0x02 && S.Request == 1 && S.Value == 0 &&
             (S.Index == 1 || S.Index == 0x81) && S.Length == 0) {
    UINT32 C = R(EPCTRL0 + 4);
    if (S.Index == 1)
      C = (C & ~1u) | (1u << 6);
    else
      C = (C & ~(1u << 16)) | (1u << 22);
    W(EPCTRL0 + 4, C);
    (VOID) SendIn(0, FALSE);
    return;
  } else if (S.Type == 0xC0 && mConfiguration == 1 && mCommand != NULL &&
             S.Length <= MAX_DATA) {
    Limit = MAX_DATA;
    Status = mCommand(S.Request, S.Value, S.Index, mBuffer, &Limit);
    if (!EFI_ERROR(Status) && Limit <= MAX_DATA)
      N = Limit;
    else {
      StallEp0();
      return;
    }
  } else {
    StallEp0();
    return;
  }
  if (N == 0) {
    StallEp0();
    return;
  }
  if (N > S.Length)
    N = S.Length;
  if (N > MAX_DATA)
    N = MAX_DATA;
  if (Source != NULL)
    CopyMem(mBuffer, Source, N);
  (VOID) SendIn(N, TRUE);
}
STATIC EFI_STATUS UlpiWrite(UINT8 Reg, UINT8 Value) {
  UINT32 I;
  W(ULPI, (1u << 30) | (1u << 29) | ((UINT32)Reg << 16) | Value);
  for (I = 0; I < 10000; I++) {
    if (!(R(ULPI) & (1u << 30)))
      return EFI_SUCCESS;
    gBS->Stall(10);
  }
  return EFI_TIMEOUT;
}
/* Do not release DMA storage until endpoints are flushed or reset completes.
 * Reserved pages remain reserved if hardware cannot be stopped. */
STATIC EFI_STATUS Quiesce(VOID) {
  EFI_STATUS S;
  W(USBINTR, 0);
  W(USBCMD, R(USBCMD) & ~CMD_RUN);
  W(EPFLUSH, 0xffffffffu);
  S = WaitMask(EPFLUSH, 0xffffffffu, 0, 10000);
  if (EFI_ERROR(S)) {
    W(USBCMD, (R(USBCMD) & ~CMD_RUN) | CMD_RESET);
    S = WaitMask(USBCMD, CMD_RESET, 0, 20000);
  }
  return S;
}
EFI_STATUS J250fUsbGadgetStart(J250F_USB_DIAG_COMMAND Command) {
  EFI_STATUS S;
  EFI_PHYSICAL_ADDRESS Addr = 0xffffffffULL;
  if (mStarted)
    return EFI_ALREADY_STARTED;
  if (mDmaQuarantined)
    return EFI_DEVICE_ERROR; /* Retain one DMA allocation until reboot. */
  if (Command == NULL)
    return EFI_INVALID_PARAMETER;
  /* No ability to enable clocks/PHY: don't reset a dead controller. */
  if (R(0x000) == 0xffffffffu || R(0x00c) == 0xffffffffu ||
      (R(0x00c) & 0xffu) == 0)
    return EFI_NOT_READY;
  S = gBS->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType, 5, &Addr);
  if (EFI_ERROR(S))
    return S;
  mDma = Addr;
  mQh = (USB_QH *)(UINTN)Addr;
  mTdOut = (USB_TD *)(UINTN)(Addr + 4096);
  mTdIn = (USB_TD *)(UINTN)(Addr + 4096 + 64);
  mBuffer = (UINT8 *)(UINTN)(Addr + 8192);
  mBulkOut = (USB_TD *)(UINTN)(Addr + 4096 + 128);
  mBulkIn = (USB_TD *)(UINTN)(Addr + 4096 + 192);
  mBulkRxBuffer = (UINT8 *)(UINTN)(Addr + 12288);
  mBulkTxBuffer = (UINT8 *)(UINTN)(Addr + 16384);
  J250fAdbShellOps(&mAdbOps);
  mCommand = Command;
  /* LK's MSM8952 ordering: reset -> ULPI selection -> external VBUS -> device
   * mode. */
  W(USBCMD, CMD_ITC | CMD_RESET);
  S = WaitMask(USBCMD, CMD_RESET, 0, 20000);
  if (EFI_ERROR(S))
    goto Fail;
  W(PORTSC, 0x80000000u);
  S = UlpiWrite(0x97,
                0x03); /* ULPI MISC_A_SET: VBUSVALID externally asserted */
  if (EFI_ERROR(S))
    goto Fail;
  W(GENCONFIG2, R(GENCONFIG2) | (1u << 7));
  W(USBCMD, R(USBCMD) | CMD_SESSVALID);
  W(0x090, 0);
  W(0x098, 8);
  W(USBMODE, 2);
  SetupHeads();
  W(EPCOMPLETE, R(EPCOMPLETE));
  W(SETUPSTAT, R(SETUPSTAT));
  W(EPFLUSH, 0xffffffffu);
  S = WaitMask(EPFLUSH, 0xffffffffu, 0, 10000);
  if (EFI_ERROR(S))
    goto Fail;
  W(USBSTS, R(USBSTS));
  W(USBINTR, 0); /* polling; no GIC handler required */
  W(USBCMD, R(USBCMD) | CMD_ITC | CMD_RUN);
  mStarted = TRUE;
  return EFI_SUCCESS;
Fail:
  mCommand = NULL;
  if (!EFI_ERROR(Quiesce()))
    gBS->FreePages(Addr, 5);
  else {
    mDmaQuarantined = TRUE;
    DEBUG(
        (DEBUG_ERROR, "J250F USB: DMA stop failed; reserved pages retained\n"));
  }
  return S;
}
EFI_STATUS J250fUsbGadgetPoll(VOID) {
  UINT32 Events, Complete, Pending;
  if (!mStarted)
    return EFI_NOT_READY;
  Events = R(USBSTS);
  if (Events)
    W(USBSTS, Events);
  if (Events & STS_RESET) {
    W(DEVICEADDR, 0);
    W(EPCTRL0 + 4, 0);
    W(EPCOMPLETE, R(EPCOMPLETE));
    W(SETUPSTAT, R(SETUPSTAT));
    W(EPFLUSH, 0xffffffffu);
    if (EFI_ERROR(WaitMask(EPFLUSH, 0xffffffffu, 0, 10000)))
      return EFI_TIMEOUT;
    SetupHeads();
  }
  Pending = R(SETUPSTAT);
  if (Pending & RX0)
    HandleSetup();
  Complete = R(EPCOMPLETE);
  if (Complete)
    W(EPCOMPLETE, Complete);
  if (mTxPending && (Complete & TX0)) {
    mTxPending = FALSE;
    Invalidate(mTdIn, sizeof(*mTdIn));
    if ((mTdIn->Info & 0x68u) != 0) {
      StallEp0();
      return EFI_DEVICE_ERROR;
    }
    if (mNeedStatusOut) {
      EFI_STATUS S;
      mNeedStatusOut = FALSE;
      S = Prime(RX0, mTdOut, &mQh[0], 0);
      if (EFI_ERROR(S))
        return S;
    }
  }
  (VOID) Events;
  return BulkPoll(Complete);
}
VOID J250fUsbGadgetStop(VOID) {
  if (!mStarted)
    return;
  if (!EFI_ERROR(Quiesce()))
    gBS->FreePages(mDma, 5);
  else {
    mDmaQuarantined = TRUE;
    DEBUG(
        (DEBUG_ERROR, "J250F USB: DMA stop failed; reserved pages retained\n"));
  }
  mStarted = FALSE;
  mCommand = NULL;
  mBuffer = NULL;
  mQh = NULL;
}

/* Descriptor and payload buffers occupy separate cache lines/pages. ADB USB
 * sends its 24-byte header separately from its payload, as host reads have
 * separate exact lengths. Combining them can overflow the host header read. */
STATIC EFI_STATUS BulkPrime(BOOLEAN In, CONST UINT8 *Data, UINTN Length) {
  USB_TD *Td = In ? mBulkIn : mBulkOut;
  USB_QH *Q = &mQh[In ? 3 : 2];
  UINT8 *Buffer = In ? mBulkTxBuffer : mBulkRxBuffer;
  UINT32 Phys = (UINT32)(mDma + (In ? 16384 : 12288));
  UINTN I;
  if (Length > JADB_MAX)
    return EFI_BAD_BUFFER_SIZE;
  ZeroMem(Td, sizeof(*Td));
  Td->Next = TD_TERMINATE;
  Td->Info = ((UINT32)Length << 16) | TD_IOC | TD_ACTIVE;
  for (I = 0; I < 5; I++)
    Td->Page[I] = (Phys & ~0xfffu) + (UINT32)(I * 4096);
  Td->Page[0] = Phys;
  if (In && Length)
    CopyMem(Buffer, Data, Length);
  Flush(Buffer, JADB_MAX);
  Flush(Td, sizeof(*Td));
  Q->Next = (UINT32)(mDma + 4096 + (In ? 192 : 128));
  Q->Info = 0;
  Flush(Q, sizeof(*Q));
  W(EPPRIME, In ? TX1 : RX1);
  return WaitMask(EPPRIME, In ? TX1 : RX1, 0, 10000);
}
STATIC EFI_STATUS BulkConfigure(VOID) {
  UINT32 Mps = (((R(PORTSC) >> 26) & 3u) == 2u) ? 512 : 64;
  EFI_STATUS S;
  W(EPFLUSH, RX1 | TX1);
  S = WaitMask(EPFLUSH, RX1 | TX1, 0, 10000);
  if (EFI_ERROR(S))
    return S;
  W(EPCOMPLETE, RX1 | TX1);
  W(EPCTRL0 + 4, 0);
  mBulkRxArmed = FALSE;
  mBulkTxStage = 0;
  jadb_init(&mAdb, &mAdbOps);
  if (!mConfiguration)
    return EFI_SUCCESS;
  ZeroMem(&mQh[2], 128);
  mQh[2].Config = mQh[3].Config = (Mps << 16) | QH_ZLT;
  mQh[2].Next = mQh[3].Next = TD_TERMINATE;
  Flush(&mQh[2], 128);
  W(EPCTRL0 + 4,
    (2u << 2) | (1u << 7) | (1u << 6) | (2u << 18) | (1u << 23) | (1u << 22));
  S = BulkPrime(FALSE, NULL, JADB_MAX);
  if (!EFI_ERROR(S))
    mBulkRxArmed = TRUE;
  return S;
}
STATIC EFI_STATUS BulkPoll(UINT32 Complete) {
  EFI_STATUS S;
  CONST UINT8 *P;
  size_t N;
  if (!mConfiguration)
    return EFI_SUCCESS;
  if ((Complete & RX1) && mBulkRxArmed) {
    UINTN Remaining;
    mBulkRxArmed = FALSE;
    Invalidate(mBulkOut, sizeof(*mBulkOut));
    if (mBulkOut->Info & (0x68u | TD_ACTIVE))
      return EFI_DEVICE_ERROR;
    Remaining = (mBulkOut->Info >> 16) & 0x7fffu;
    if (Remaining > JADB_MAX)
      return EFI_DEVICE_ERROR;
    Invalidate(mBulkRxBuffer, JADB_MAX);
    if (jadb_feed(&mAdb, mBulkRxBuffer, JADB_MAX - Remaining))
      return EFI_PROTOCOL_ERROR;
  }
  if ((Complete & TX1) && mBulkTxStage) {
    Invalidate(mBulkIn, sizeof(*mBulkIn));
    if (mBulkIn->Info & (0x68u | TD_ACTIVE))
      return EFI_DEVICE_ERROR;
    if (mBulkTxStage == 1 && mBulkPacketSize > 24) {
      S = BulkPrime(TRUE, mBulkPacket + 24, mBulkPacketSize - 24);
      if (EFI_ERROR(S))
        return S;
      mBulkTxStage = 2;
    } else {
      mBulkTxStage = 0;
      jadb_tx_done(&mAdb);
    }
  }
  if (!mBulkRxArmed) {
    S = BulkPrime(FALSE, NULL, JADB_MAX);
    if (EFI_ERROR(S))
      return S;
    mBulkRxArmed = TRUE;
  }
  if (!mBulkTxStage) {
    P = jadb_tx(&mAdb, &N);
    if (P) {
      CopyMem(mBulkPacket, P, N);
      mBulkPacketSize = N;
      S = BulkPrime(TRUE, mBulkPacket, 24);
      if (EFI_ERROR(S))
        return S;
      mBulkTxStage = 1;
    }
  }
  return EFI_SUCCESS;
}
