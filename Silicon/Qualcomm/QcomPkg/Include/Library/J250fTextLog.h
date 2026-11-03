/** Shared, bounded pre-OS text queue. SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef J250F_TEXT_LOG_H
#define J250F_TEXT_LOG_H
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/SynchronizationLib.h>

#define J250F_TEXT_LOG_GUID \
  {0x904d4b15,0x4537,0x4acb,{0xa4,0x74,0x5a,0x39,0x2f,0x53,0x16,0x08}}
#define J250F_TEXT_LOG_MAGIC SIGNATURE_32('J','L','O','G')
#define J250F_TEXT_LOG_BYTES 49152U
typedef VOID (EFIAPI *J250F_TEXT_FLUSH)(VOID);
typedef struct {
  UINT32 Magic;
  volatile UINT32 Head;
  volatile UINT32 Tail;
  volatile UINT32 Dropped;
  volatile UINT32 ProducerLock;
  volatile BOOLEAN Stopped;
  BOOLEAN SuppressSerial; /* suppress duplicate CAPTURE inside a console tee */
  UINTN SuppressCpu;
  /* Installed only by the resident DXE, cleared and Stopped at EBS.
     Callback must reject unsafe TPL, masked IRQs and recursive invocation. */
  J250F_TEXT_FLUSH Flush;
  UINT8 Data[J250F_TEXT_LOG_BYTES];
} J250F_TEXT_LOG;

STATIC inline VOID J250fTextDropped(J250F_TEXT_LOG *Log, UINTN Length) {
  UINT32 Old, New;
  do {
    Old = Log->Dropped;
    New = Length > MAX_UINT32 - Old ? MAX_UINT32 : Old + (UINT32)Length;
  } while (InterlockedCompareExchange32(&Log->Dropped, Old, New) != Old);
}

/* Nonblocking producer: interrupt/AP re-entry drops and COUNTS bytes rather
   than deadlocking a fault path. Caller masks local interrupts while appending. */
STATIC inline VOID J250fTextAppend(J250F_TEXT_LOG *Log, CONST UINT8 *Bytes, UINTN Length) {
  if (Log->Stopped) return;
  if (Log->Head >= J250F_TEXT_LOG_BYTES || Log->Tail >= J250F_TEXT_LOG_BYTES) {
    Log->Stopped = TRUE;
    return;
  }
  if (InterlockedCompareExchange32(&Log->ProducerLock, 0, 1) != 0) {
    J250fTextDropped(Log, Length);
    return;
  }
  MemoryFence();
  while (Length--) {
    UINT32 Next = (Log->Head + 1) % J250F_TEXT_LOG_BYTES;
    if (Next == Log->Tail) {
      J250fTextDropped(Log, Length + 1);
      break;
    }
    Log->Data[Log->Head] = *Bytes++;
    MemoryFence();
    Log->Head = Next;
  }
  MemoryFence();
  InterlockedCompareExchange32(&Log->ProducerLock, 1, 0);
}

/* PrePi attaches explicitly; DXE library instances discover the same GUID HOB.
   No fixed physical addresses and no new MMIO are used for capture. */
VOID EFIAPI FbConAttachTextLog(J250F_TEXT_LOG *Log);
#endif
