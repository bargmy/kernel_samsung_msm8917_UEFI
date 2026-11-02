# Experimental J250F USB EP0 debug gadget

This is **not hardware-validated**. It uses the Qualcomm ChipIdea USB2
device controller at 0x078DB000 with a vendor-only control endpoint.
No ADB, fastboot, mass storage or arbitrary MMIO writes are provided.

This branch is intentionally separate from main. The gadget relies on
inherited USB PHY power/clock and the existing bootloader handoff, does
not vote PM8917 USB L2/L7/L13, and does not establish independent USB
clock initialization. Failing the USB preflight is not proof of usable PHY.

Install PyUSB: `python3 -m pip install pyusb`. For a plugged-in phone
enumerated at the **development-only** VID/PID 1209:8917:
`python3 Platforms/Samsung/J250fPkg/UsbDebug/j250f_usb_ep0.py ping`
or `snapshot`; `probe --rate 400000 --yes` retries eMMC identification
using volatile controller state and should only be run with a recovery
path. Do not distribute with an unassigned VID/PID.

Controller source and integration copied from the J250F experimental
USB-port package. Tests were simulations and minimal header ARM32 syntax,
not a real Project Mu image build or actual USB enumeration.

**Do not merge or flash without verifying the build, memory mapping,
DMA cache coherency, USB PHY clocks/regulators, and physical recovery.**
