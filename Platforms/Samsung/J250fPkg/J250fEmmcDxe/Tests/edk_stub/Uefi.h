/* TEST ONLY EDK-II TYPE SHIM; NOT PART OF RELEASE BUILD */
#ifndef TEST_UEFI_H
#define TEST_UEFI_H
#include <stdint.h>
#include <stddef.h>
typedef void VOID;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uintptr_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef UINTN EFI_TPL;
typedef UINT8 BOOLEAN;
typedef UINT64 EFI_LBA;
typedef struct {UINT32 Data1;UINT16 Data2,Data3;UINT8 Data4[8];} EFI_GUID;
typedef struct {int unused;} EFI_SYSTEM_TABLE;
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define TRUE ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#define MAX_UINT32 UINT32_MAX
#define EFI_SUCCESS ((EFI_STATUS)0)
#define EFI_INVALID_PARAMETER ((EFI_STATUS)2)
#define EFI_UNSUPPORTED ((EFI_STATUS)3)
#define EFI_NOT_READY ((EFI_STATUS)6)
#define EFI_DEVICE_ERROR ((EFI_STATUS)7)
#define EFI_WRITE_PROTECTED ((EFI_STATUS)8)
#define EFI_NO_MEDIA ((EFI_STATUS)12)
#define EFI_MEDIA_CHANGED ((EFI_STATUS)13)
#define EFI_BAD_BUFFER_SIZE ((EFI_STATUS)4)
#define EFI_TIMEOUT ((EFI_STATUS)18)
#define EFI_ERROR(x) ((x)!=EFI_SUCCESS)
#define TPL_NOTIFY 16
#define SIGNATURE_32(a,b,c,d) ((UINT32)(a)|((UINT32)(b)<<8)|((UINT32)(c)<<16)|((UINT32)(d)<<24))
typedef struct {
 EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL);
 VOID (EFIAPI *RestoreTPL)(EFI_TPL);
 EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *,VOID *,VOID **);
 EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *,...);
} EFI_BOOT_SERVICES;
#endif
