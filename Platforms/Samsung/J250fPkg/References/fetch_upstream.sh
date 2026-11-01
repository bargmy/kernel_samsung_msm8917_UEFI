#!/bin/sh
# Fetch upstream reference sources ONLY. This does not perform an LK-to-UEFI port.
# Must run on a connected development machine with curl, not in the offline test VM.
set -eu
LKREV=fd380ac637d1af38598e3f3c5321d4c962b96315
LK=https://raw.githubusercontent.com/msm8916-mainline/lk2nd/$LKREV
LUMIA=https://raw.githubusercontent.com/rickliu2000/Lumia930Pkg/master
mkdir -p lk/msm_shared/include lk/msm8952 lk/target/msm8952 lumia930
for f in mmc_sdhci.c sdhci.c sdhci_msm.c; do
  curl -fLsS "$LK/platform/msm_shared/$f" -o "lk/msm_shared/$f"
done
for f in mmc_sdhci.h sdhci.h sdhci_msm.h; do
  curl -fLsS "$LK/platform/msm_shared/include/$f" -o "lk/msm_shared/include/$f"
done
for f in init.c; do
  curl -fLsS "$LK/target/msm8952/$f" -o "lk/target/msm8952/$f"
done
curl -fLsS "$LK/platform/msm8952/include/platform/iomap.h" -o lk/msm8952/iomap.h
for f in MMCHS.c MMCHS.h SdhciMMCHS.inf Source.txt; do
  curl -fLsS "$LUMIA/Driver/SdhciMMCHSDxe/$f" -o "lumia930/$f"
done
printf '%s\n' 'Fetched upstream LK reference only; not a buildable J250F driver.'
