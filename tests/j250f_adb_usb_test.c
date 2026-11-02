/* Actual gadget state machine with mocked ChipIdea registers/DMA completions.
 * Cache operations are mocks; this does not establish physical coherency. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fUsbGadget.c"
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X)) {                                                                \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #X);                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static EFI_BOOT_SERVICES Bs;
EFI_BOOT_SERVICES *gBS = &Bs;
static UINT32 Regs[0x1000 / 4];
static BOOLEAN FailFlush, FailReset;
static VOID *Pages;
static UINTN Freed;
UINT32 EFIAPI MmioRead32(UINTN A) {
  CHECK(A >= BASE && A < BASE + sizeof(Regs));
  return Regs[(A - BASE) / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN A, UINT32 V) {
  UINTN O = A - BASE;
  CHECK(A >= BASE && A < BASE + sizeof(Regs));
  if (O == USBSTS || O == SETUPSTAT || O == EPCOMPLETE)
    Regs[O / 4] &= ~V;
  else if (O == EPPRIME)
    Regs[O / 4] = 0;
  else if (O == EPFLUSH)
    Regs[O / 4] = FailFlush ? V : 0;
  else if (O == USBCMD)
    Regs[O / 4] = FailReset ? V : (V & ~CMD_RESET);
  else if (O == ULPI)
    Regs[O / 4] = V & ~(1u << 30);
  else
    Regs[O / 4] = V;
  return V;
}
VOID EFIAPI MemoryFence(VOID) {}
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *P, UINTN N) {
  (VOID) N;
  return P;
}
VOID *EFIAPI InvalidateDataCacheRange(VOID *P, UINTN N) {
  (VOID) N;
  return P;
}
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {
  return memcpy(D, S, N);
}
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {
  (VOID) L;
  (VOID) F;
}
static EFI_STATUS EFIAPI Stall(UINTN N) {
  (VOID) N;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Alloc(EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN N,
                               EFI_PHYSICAL_ADDRESS *A) {
  CHECK(T == AllocateMaxAddress && M == EfiReservedMemoryType && N == 5);
  Pages = mmap(NULL, N * 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(Pages != MAP_FAILED && (UINTN)Pages < 0xffffffffu);
  *A = (UINTN)Pages;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(EFI_PHYSICAL_ADDRESS A, UINTN N) {
  CHECK((VOID *)(UINTN)A == Pages && N == 5);
  CHECK(munmap(Pages, N * 4096) == 0);
  Freed++;
  Pages = NULL;
  return EFI_SUCCESS;
}
VOID J250fAdbShellOps(jadb_ops *O) { memset(O, 0, sizeof(*O)); }
static EFI_STATUS EFIAPI Diag(UINT16 R, UINT16 V, UINT16 I, VOID *B, UINTN *N) {
  (VOID) R;
  (VOID) V;
  (VOID) I;
  (VOID) B;
  (VOID) N;
  return EFI_UNSUPPORTED;
}
static void Complete(UINT32 Bit) {
  Regs[EPCOMPLETE / 4] |= Bit;
  Regs[USBSTS / 4] |= STS_USB;
  CHECK(J250fUsbGadgetPoll() == EFI_SUCCESS);
}
int main(void) {
  UINT32 H[6] = {0x4e584e43, 0x1000000, 4096, 0, 0, 0xb1a7b1bc};
  USB_SETUP Setup;
  Bs.AllocatePages = Alloc;
  Bs.FreePages = Free;
  Bs.Stall = Stall;
  Regs[0x00c / 4] = 1;
  CHECK(J250fUsbGadgetStart(Diag) == EFI_SUCCESS);
  // Full-speed configuration and OS compatibility descriptors.
  Setup = (USB_SETUP){0x80, 6, 0x0200, 0, 32};
  CopyMem((VOID *)mQh[0].Setup, &Setup, 8);
  HandleSetup();
  CHECK(mBuffer[22] == 64 && mBuffer[23] == 0 && mBuffer[29] == 64);
  CHECK(sizeof(mMsCompat) == 40 && !memcmp(mMsCompat + 18, "WINUSB", 6));
  CHECK(mMsProperties[6] == 5 && mMsProperties[8] == 1);
  mConfiguration = 1;
  CHECK(BulkConfigure() == EFI_SUCCESS && mBulkRxArmed);
  // CNXN completes OUT, then device must transmit header and payload
  // separately.
  CopyMem(mBulkRxBuffer, H, sizeof(H));
  mBulkOut->Info = (JADB_MAX - sizeof(H)) << 16;
  Complete(RX1);
  CHECK(mBulkTxStage == 1 && ((mBulkIn->Info >> 16) & 0x7fff) == 24);
  CHECK(!memcmp(mBulkTxBuffer, H, 4));
  mBulkIn->Info = 0;
  Complete(TX1);
  CHECK(mBulkTxStage == 2 && ((mBulkIn->Info >> 16) & 0x7fff) > 24);
  CHECK(!memcmp(mBulkTxBuffer, "device::", 8));
  mBulkIn->Info = 0;
  Complete(TX1);
  CHECK(mBulkTxStage == 0 && mAdb.tx_count == 0);
  // Flush timeout succeeds via reset before pages are released.
  FailFlush = TRUE;
  J250fUsbGadgetStop();
  CHECK(Freed == 1 && !mStarted);
  FailFlush = FALSE;
  CHECK(J250fUsbGadgetStart(Diag) == EFI_SUCCESS);
  // Both stop mechanisms fail: keep reserved DMA storage rather than free it.
  FailFlush = FailReset = TRUE;
  J250fUsbGadgetStop();
  CHECK(Freed == 1 && Pages && !mStarted);
  CHECK(J250fUsbGadgetStart(Diag) == EFI_DEVICE_ERROR && Freed == 1);
  CHECK(munmap(Pages, 5 * 4096) ==
        0); // fixture cleanup after mocked hardware stops
  puts("PASS: actual USB gadget CNXN bulk header/payload split, full-speed "
       "descriptor, OS descriptors, reset fallback and retained DMA on stop "
       "failure");
  return 0;
}
