/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J250F_USB_GADGET_H
#define J250F_USB_GADGET_H
#include <Uefi.h>
typedef EFI_STATUS (EFIAPI *J250F_USB_DIAG_COMMAND)(
  UINT16 Request, UINT16 Value, UINT16 Index, VOID *Output, UINTN *Length);
EFI_STATUS J250fUsbGadgetStart(J250F_USB_DIAG_COMMAND Command);
EFI_STATUS J250fUsbGadgetPoll(VOID);
VOID J250fUsbGadgetStop(VOID);
#endif
