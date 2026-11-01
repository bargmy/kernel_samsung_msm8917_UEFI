# [Project Mu](https://microsoft.github.io/mu/) UEFI Implementation for Qualcomm Snapdragon™ Devices

<!-- ![Banner](https://github.com/Robotix22/Mu-Qcom/blob/main/Pictures/Banner.png) -->

## Supported Devices

### Samsung Galaxy J2 Pro 2018 (SM-J250F / j2y18lte)
- **SoC:** Qualcomm Snapdragon 425 (MSM8917), Quad-core Cortex-A53
- **Architecture:** ARM32 (AArch32)
- **RAM:** 1.5 GB
- **Display:** 540x960 qHD Super AMOLED (Continuous Splash at `0xA8000000`)
- **UEFI Base:** `0x99000000` (Avoids QSEE TrustZone collision)
- **Keypad:** Volume Up (GPIO 127), Home (GPIO 128), Vol Down / Power (PMIC)
- **Build Command:**
  ```bash
  export CLANG38_BIN=/usr/bin/
  export CLANG38_ARM_PREFIX=arm-linux-gnueabihf-
  export GCC5_ARM_PREFIX=arm-linux-gnueabihf-
  ./build_uefi.sh -d J250F -a ARM -r RELEASE -t CLANG38
  ```

## Description

This repo brings UEFI Firmware on your Device (If supported) to boot OSs like Windows or Linux

## Resources

[Discord Server](https://discord.gg/Dx2QgMx7Sv)

## [Status](https://github.com/Robotix22/Mu-Qcom/blob/main/Status.md)

## [Building](https://github.com/Robotix22/Mu-Qcom/blob/main/Building.md)

## [Guides](https://github.com/Robotix22/UEFI-Guides/blob/main/Mu-Qcom/README.md)

## Credits

Thanks to [Gustave Monce](https://github.com/gus33000) for his [SurfaceDuoPkg](https://github.com/WOA-Project/SurfaceDuoPkg), without it this repo would probally not exist.

## License

All code except drivers in `GPLDrivers` directory are licensed under BSD 2-Clause. <br />
GPL Drivers are licensed under GPLv2 license.
