/* Actual firmware shell/upload callbacks against a mocked UEFI image loader.
 * This checks call ordering/lifetimes; it does not execute ARM machine code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fAdbShell.c"
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X)) {                                                                \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #X);                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
STATIC EFI_BOOT_SERVICES Bs;
STATIC EFI_RUNTIME_SERVICES Rt;
EFI_BOOT_SERVICES *gBS = &Bs;
EFI_RUNTIME_SERVICES *gRT = &Rt;
EFI_GUID gEfiLoadedImageProtocolGuid = {0};
STATIC EFI_LOADED_IMAGE_PROTOCOL Loaded;
STATIC UINTN Pools, Loads, Starts, Unloads;
STATIC BOOLEAN Live, Resident, Reject;
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) { return strlen(S); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A, B); }
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A, CONST CHAR8 *B, UINTN N) {
  return strncmp(A, B, N);
}
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {
  return memcpy(D, S, N);
}
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
UINTN EFIAPI AsciiSPrint(CHAR8 *B, UINTN N, CONST CHAR8 *F, ...) {
  (VOID) F;
  if (N)
    B[0] = 0;
  return 0;
}
STATIC EFI_STATUS EFIAPI Alloc(EFI_MEMORY_TYPE T, UINTN N, VOID **P) {
  (VOID) T;
  *P = malloc(N);
  if (!*P)
    return EFI_OUT_OF_RESOURCES;
  Pools++;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Free(VOID *P) {
  CHECK(P && Pools);
  free(P);
  Pools--;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Load(BOOLEAN Policy, EFI_HANDLE Parent,
                              EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Buffer,
                              UINTN Size, EFI_HANDLE *Image) {
  CHECK(!Policy && Parent == (VOID *)0x1234 && Path == NULL);
  CHECK(Size == 4 && !memcmp(Buffer, "test", 4));
  Loads++;
  if (Reject)
    return EFI_UNSUPPORTED;
  *Image = (VOID *)0x555;
  Live = TRUE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Protocol(EFI_HANDLE H, EFI_GUID *G, VOID **P) {
  CHECK(G == &gEfiLoadedImageProtocolGuid);
  if (!Live || H != (VOID *)0x555)
    return EFI_NOT_FOUND;
  *P = &Loaded;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Start(EFI_HANDLE H, UINTN *Size, CHAR16 **Text) {
  CHECK(H == (VOID *)0x555 && Size == NULL && Text == NULL && Live);
  CHECK(Loaded.LoadOptions && Loaded.LoadOptionsSize > 2);
  CHECK(((CHAR16 *)Loaded.LoadOptions)[0] == '/');
  Starts++;
  if (!Resident)
    Live = FALSE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unload(EFI_HANDLE H) {
  CHECK(H == (VOID *)0x555 && Live);
  Live = FALSE;
  Unloads++;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI J250fUsbDiagCommand(UINT16 R, UINT16 V, UINT16 I, VOID *B,
                                      UINTN *N) {
  (VOID) R;
  (VOID) V;
  (VOID) I;
  (VOID) B;
  (VOID) N;
  return EFI_NOT_READY;
}
EFI_STATUS J250fEmmcGetCid(UINT32 *C) {
  (VOID) C;
  return EFI_NOT_READY;
}
BOOLEAN J250fEmmcDebugCommand(CONST CHAR8 *C,CHAR8 *O,UINTN N,UINTN *L) {
 (VOID)C;(VOID)O;(VOID)N;(VOID)L;return FALSE;
}
int main(void) {
  jadb_ops O;
  CHAR8 Reply[1024];
  UINT32 Mode, Size;
  UINTN I;
  Bs.AllocatePool = Alloc;
  Bs.FreePool = Free;
  Bs.LoadImage = Load;
  Bs.StartImage = Start;
  Bs.HandleProtocol = Protocol;
  Bs.UnloadImage = Unload;
  J250fAdbShellInit((VOID *)0x1234);
  J250fAdbShellOps(&O);
  CHECK(O.begin(NULL, "/outside/test.efi") < 0);
  CHECK(O.begin(NULL, "/ram/test.efi") == 0);
  CHECK(O.done(NULL) < 0);
  CHECK(O.data(NULL, (CONST UINT8 *)"test", 4) == 0);
  CHECK(O.done(NULL) == 0);
  CHECK(O.stat(NULL, "/ram/test.efi", &Mode, &Size) == 0 && Size == 4 &&
        Mode == 0100644);
  CHECK(O.data(NULL, (CONST UINT8 *)"", JADB_FILE_MAX + 1u) < 0);
  for (I = 0; I < 64; I++)
    O.execute(NULL, "efi run /ram/test.efi arg", Reply, sizeof(Reply));
  CHECK(Starts == 64 && Loads == 64 &&
        Pools == 1); // returned apps release load options
  Resident = TRUE;
  O.execute(NULL, "efi run /ram/test.efi", Reply, sizeof(Reply));
  CHECK(Pools == 2 && Live);
  O.execute(NULL, "efi unload 0", Reply, sizeof(Reply));
  CHECK(Pools == 1 && Unloads == 1 && !Live);
  Reject = TRUE;
  O.execute(NULL, "efi run /ram/test.efi", Reply, sizeof(Reply));
  CHECK(Starts == 65 && Pools == 1);
  O.execute(NULL, "firmware exit", Reply, sizeof(Reply));
  CHECK(J250fAdbShellExitRequested());
  J250fAdbShellFinish();
  CHECK(Pools == 0);
  puts("PASS: actual RAM upload validation, UEFI LoadImage/StartImage calls, "
       "app option cleanup, resident-driver unload, loader failure, explicit "
       "exit");
  return 0;
}
