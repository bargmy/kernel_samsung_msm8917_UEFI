# Integration checks against the actual Mu headers. No test is hardware proof.
ROOT := $(abspath ../../..)
OUT := /tmp/j250f-mu-emmc-checks
CC ?= gcc
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
INC := -I$(ROOT)/Mu_Basecore/MdePkg/Include -I$(ROOT)/Mu_Basecore/MdePkg/Include/X64 -I$(ROOT)/Silicon/Qualcomm/QcomPkg/Include -IJ250fEmmcDxe/Include
UEFI := -DMDEPKG_NDEBUG -fshort-wchar -ffunction-sections -fdata-sections
GC := -Wl,--gc-sections
CORE := J250fEmmcDxe/Core/j250f_emmc.c J250fEmmcDxe/Core/j250f_board.c
ARM := --target=armv7a-none-eabi -mthumb -ffreestanding -fshort-wchar -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$(ROOT)/Mu_Basecore/MdePkg/Include -I$(ROOT)/Mu_Basecore/MdePkg/Include/Arm -I$(ROOT)/Silicon/Qualcomm/QcomPkg/Include -IJ250fEmmcDxe/Include
.PHONY: all test arm syntax sanitizer
all: test
$(OUT):
	mkdir -p $(OUT)
test: | $(OUT)
	$(CC) $(CFLAGS) -IJ250fEmmcDxe/Include J250fEmmcDxe/Tests/test_recognition.c $(CORE) -o $(OUT)/recognition
	$(OUT)/recognition
	$(CC) $(CFLAGS) $(INC) $(UEFI) $(ROOT)/tests/j250f_rpm_smd_test.c $(GC) -o $(OUT)/rpm
	$(OUT)/rpm
	$(CC) $(CFLAGS) $(INC) $(UEFI) $(ROOT)/tests/j250f_smem_map_test.c -o $(OUT)/map
	$(OUT)/map
	$(CC) $(CFLAGS) $(INC) $(UEFI) $(ROOT)/tests/j250f_lk_mu_integration_test.c $(CORE) -o $(OUT)/integration
	$(OUT)/integration
arm: | $(OUT)
	clang $(ARM) -c J250fEmmcDxe/Core/j250f_emmc.c -o $(OUT)/core-arm.o
	clang $(ARM) -c J250fEmmcDxe/Core/j250f_board.c -o $(OUT)/board-arm.o
	clang $(ARM) -c PlatformIntegration/RpmSmd.c -o $(OUT)/rpm-arm.o
	clang $(ARM) -c PlatformIntegration/SmemMap.c -o $(OUT)/map-arm.o
syntax: arm
	clang $(ARM) -fsyntax-only PlatformIntegration/J250fPowerDxe.c
	clang $(ARM) -fsyntax-only J250fEmmcDxe/J250fLkBackend.c
	clang $(ARM) -fsyntax-only J250fEmmcDxe/J250fLkEmmcDxe.c
sanitizer: | $(OUT)
	$(CC) $(CFLAGS) $(INC) $(UEFI) -fsanitize=address,undefined -fno-omit-frame-pointer $(ROOT)/tests/j250f_rpm_smd_test.c $(GC) -o $(OUT)/rpm-sanitized
	$(OUT)/rpm-sanitized
	$(CC) $(CFLAGS) $(INC) $(UEFI) -fsanitize=address,undefined -fno-omit-frame-pointer $(ROOT)/tests/j250f_smem_map_test.c -o $(OUT)/map-sanitized
	$(OUT)/map-sanitized
	$(CC) $(CFLAGS) $(INC) $(UEFI) -fsanitize=address,undefined -fno-omit-frame-pointer $(ROOT)/tests/j250f_lk_mu_integration_test.c $(CORE) -o $(OUT)/integration-sanitized
	$(OUT)/integration-sanitized
