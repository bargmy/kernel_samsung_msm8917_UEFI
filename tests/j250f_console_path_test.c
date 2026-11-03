/* Both build variants must use the identity actually published by their GOP. */
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Configuration/BootDevices.h>
int main(void) {
#ifdef J250F_PLATFORM
  EFI_GUID Expected=J250F_DISPLAY_DEVICE_GUID;
#else
  EFI_GUID Expected=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
#endif
  if(memcmp(&Expected,&DisplayDevicePath.DisplayDevicePath.Guid,sizeof(Expected))) return 1;
  puts("platform display device-path identity: PASS");
  return 0;
}
