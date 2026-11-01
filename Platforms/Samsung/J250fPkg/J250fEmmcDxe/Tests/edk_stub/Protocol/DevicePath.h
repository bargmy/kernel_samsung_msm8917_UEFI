#include <Uefi.h>
#pragma pack(push,1)
typedef struct {UINT8 Type,SubType,Length[2];} EFI_DEVICE_PATH_PROTOCOL;
typedef struct {EFI_DEVICE_PATH_PROTOCOL Header;EFI_GUID Guid;} VENDOR_DEVICE_PATH;
#pragma pack(pop)
#define HARDWARE_DEVICE_PATH 1
#define HW_VENDOR_DP 4
#define END_DEVICE_PATH_TYPE 0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xff
extern EFI_GUID gEfiDevicePathProtocolGuid;
extern EFI_GUID gJ250fEmmcDevicePathGuid;
