# Makefile for HackBGRT Linux-style Fake Boot Loader (x86_64 UEFI)
# Supports:
#   1. MinGW-w64 (Windows mingw32-make / MSYS2 / WSL make):  make
#   2. Clang + LLD (as in HackBGRT 2.6+):                    make TOOLCHAIN=clang
#   3. Optional GNU-EFI headers:                             make GNUEFI_INC=gnu-efi/inc

TOOLCHAIN  ?= mingw
BUILD_DIR  ?= efi
SRC        := src/main.c
DEMO       ?= 0

COMMON_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
                 -ffreestanding -fno-stack-protector -fno-stack-check -fno-builtin \
                 -fshort-wchar -mno-red-zone -mno-mmx -mno-sse

ifeq ($(DEMO),1)
COMMON_CFLAGS += -DFORCE_DEMO_FRAMES=1
endif

ifneq ($(strip $(GNUEFI_INC)),)
COMMON_CFLAGS += -DUSE_GNUEFI=1 -DGNU_EFI_USE_MS_ABI=1 \
                 -I$(GNUEFI_INC) -I$(GNUEFI_INC)/x86_64 -I$(GNUEFI_INC)/protocol
endif

ifeq ($(TOOLCHAIN),clang)
CC      := clang
CFLAGS  := -target x86_64-pc-windows-msvc $(COMMON_CFLAGS)
LDFLAGS := -target x86_64-pc-windows-msvc -nostdlib \
           -Wl,-entry:efi_main -Wl,-subsystem:efi_application -fuse-ld=lld
else
CC      := x86_64-w64-mingw32-gcc
CFLAGS  := $(COMMON_CFLAGS) -mno-stack-arg-probe
LDFLAGS := -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -e efi_main
endif

ifeq ($(OS),Windows_NT)
SHELL   := cmd.exe
PYTHON  ?= python
MKDIR_P  = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
CP_FILE  = copy /Y "$(subst /,\,$(1))" "$(subst /,\,$(2))" >nul
RM_DIR   = if exist "$(subst /,\,$(1))" rmdir /S /Q "$(subst /,\,$(1))"
else
PYTHON  ?= python3
MKDIR_P  = mkdir -p "$(1)"
CP_FILE  = cp "$(1)" "$(2)"
RM_DIR   = rm -rf "$(1)"
endif

TARGETS   := $(BUILD_DIR)/bootx64.efi $(BUILD_DIR)/grubx64.efi $(BUILD_DIR)/loader.efi
MOCK_STUB := $(BUILD_DIR)/mock_bootmgfw.efi
QEMU_ARGS ?=

.PHONY: all clean esp-image qemu

all: $(TARGETS)

ifeq ($(DEMO),1)
VIDEO_HDRS := $(wildcard src/demo_frames.h)
else
VIDEO_HDRS := $(wildcard src/video_frames.h) $(wildcard src/demo_frames.h)
endif

$(BUILD_DIR)/bootx64.efi: $(SRC) src/uefi_compat.h $(VIDEO_HDRS)
	@$(call MKDIR_P,$(BUILD_DIR))
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@

$(BUILD_DIR)/grubx64.efi: $(BUILD_DIR)/bootx64.efi
	@$(call CP_FILE,$<,$@)

$(BUILD_DIR)/loader.efi: $(BUILD_DIR)/bootx64.efi
	@$(call CP_FILE,$<,$@)

$(MOCK_STUB): tools/mock_bootmgfw.c src/uefi_compat.h
	@$(call MKDIR_P,$(BUILD_DIR))
	$(CC) $(CFLAGS) tools/mock_bootmgfw.c $(LDFLAGS) -o $@

esp-image: $(BUILD_DIR)/bootx64.efi $(MOCK_STUB)
	$(PYTHON) tools/run_qemu.py --build-only $(QEMU_ARGS)

qemu: $(BUILD_DIR)/bootx64.efi $(MOCK_STUB)
	$(PYTHON) tools/run_qemu.py $(QEMU_ARGS)

clean:
	@$(call RM_DIR,$(BUILD_DIR))
