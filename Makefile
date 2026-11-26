# Makefile for HackBGRT Linux-style Fake Boot Loader (x86_64 UEFI)
# Supports:
#   1. MinGW-w64 (Windows mingw32-make / MSYS2 / WSL make):  make
#   2. Clang + LLD (as in HackBGRT 2.6+):                    make TOOLCHAIN=clang
#   3. Optional GNU-EFI headers:                             make GNUEFI_INC=gnu-efi/inc

TOOLCHAIN  ?= mingw
BUILD_DIR  ?= efi
SRC        := src/main.c

COMMON_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
                 -ffreestanding -fno-stack-protector -fno-stack-check -fno-builtin \
                 -fshort-wchar -mno-red-zone -mno-mmx -mno-sse

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
MKDIR_P  = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
CP_FILE  = copy /Y "$(subst /,\,$(1))" "$(subst /,\,$(2))" >nul
RM_DIR   = if exist "$(subst /,\,$(1))" rmdir /S /Q "$(subst /,\,$(1))"
else
MKDIR_P  = mkdir -p "$(1)"
CP_FILE  = cp "$(1)" "$(2)"
RM_DIR   = rm -rf "$(1)"
endif

TARGETS := $(BUILD_DIR)/bootx64.efi $(BUILD_DIR)/grubx64.efi $(BUILD_DIR)/loader.efi

.PHONY: all clean

all: $(TARGETS)

VIDEO_HDRS := $(wildcard src/video_frames.h) $(wildcard src/demo_frames.h)

$(BUILD_DIR)/bootx64.efi: $(SRC) src/uefi_compat.h $(VIDEO_HDRS)
	@$(call MKDIR_P,$(BUILD_DIR))
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@

$(BUILD_DIR)/grubx64.efi: $(BUILD_DIR)/bootx64.efi
	@$(call CP_FILE,$<,$@)

$(BUILD_DIR)/loader.efi: $(BUILD_DIR)/bootx64.efi
	@$(call CP_FILE,$<,$@)

clean:
	@$(call RM_DIR,$(BUILD_DIR))
