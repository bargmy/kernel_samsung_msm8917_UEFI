# J250F USB debugging

Experimental USB control endpoint on the MSM8917 ChipIdea controller. USB PHY,
clocks and rails still depend on the bootloader handoff. A successful firmware
build does not establish that the phone will enumerate.

## PC setup

Download j250f_usb_ep0.py from this directory. Install Python 3 and PyUSB:

    python -m pip install pyusb

Linux: install your distribution's libusb-1.0 package. Run the commands with
sudo if USB access is denied, or configure a udev rule for VID 1209 / PID 8917.
Windows: install libusb-package with pip, then use Zadig to bind WinUSB to the
J250F USB Debug device (1209:8917). Select that device only. If PyUSB reports
"No backend available", place libusb-1.0.dll matching your Python architecture
on PATH, or use the backend supplied by libusb-package.

Connect the phone by a USB data cable and boot the experimental firmware.
Look for "USB DEBUG start" on screen. The gadget is polled for 90 seconds after
eMMC initialization returns. Run commands during that window. This is a custom
PyUSB interface; adb and fastboot commands do not control it.

## Commands

    python j250f_usb_ep0.py ping
    python j250f_usb_ep0.py snapshot --log emmc.jsonl
    python j250f_usb_ep0.py monitor --count 60 --interval 0.5 --log emmc.jsonl
    python j250f_usb_ep0.py probe --rate 400000 --yes --log emmc.jsonl

ping identifies the gadget. snapshot reads controller state. monitor appends
one JSON record per sample on the PC, so the logs survive rebooting the phone.
probe retries the complete card identification sequence beginning with CMD0,
not just CMD2. It changes volatile card/controller state, stays read-only,
and is limited to 32 attempts per firmware boot. A successful retry records
card_ready but does not publish Block I/O afterward; the original initialization
must succeed for normal disk publication. Power-cycle before returning to Android.

result_signed is the driver result (0 success, -2 timeout, -3 I/O error).
last_interrupt, last_present and last_vendor are captured register values;
irq_live, present_live and vendor_live are current values. Numbers in JSON are
decimal. CMD2 interrupt 163840 is hexadecimal 0x28000, the command CRC failure.

If no 1209:8917 device appears, the client cannot repair PHY/clock bringup from
the PC. Record the on-screen USB start result. USB enumeration and physical eMMC
recognition remain unverified. The development VID/PID must be replaced with
an authorized allocation before distribution.
