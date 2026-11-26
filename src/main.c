#include "uefi_compat.h"
#if !defined(FORCE_DEMO_FRAMES) && __has_include("video_frames.h")
#include "video_frames.h"
#else
#include "demo_frames.h"
#endif

/* Set to 0 to jump straight into the ASCII video without the Linux dmesg/systemd intro */
#define ENABLE_LINUX_INTRO 1

/* SBAT metadata section for Shim 15+/16+ compatibility */
__attribute__((used, section(".sbat")))
const char kSbatMetadata[] =
    "sbat,1,SBAT Version,sbat,1,https://uefi.org/\n"
    "linuxsim,1,Custom,LinuxBootSim,1.1,https://local.invalid/\n";

#define CONSOLE_CONTROL_PROTO_GUID \
    { 0xf42f7782, 0x012e, 0x4c12, { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x04, 0xf7, 0x21 } }

#define SIMPLE_TEXT_OUT_PROTO_GUID \
    { 0x387477c2, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct _CONSOLE_CTRL_PROTO CONSOLE_CTRL_PROTO;
typedef EFI_STATUS (EFIAPI *CONSOLE_CTRL_SET_MODE_FN)(CONSOLE_CTRL_PROTO *self, INT32 mode);
struct _CONSOLE_CTRL_PROTO {
    VOID                    *GetMode;
    CONSOLE_CTRL_SET_MODE_FN SetMode;
    VOID                    *LockStdIn;
};

/* Stage 1 (dmesg) & Stage 2 (systemd) timing constants in microseconds */
#define DMESG_MIN_US      5000U
#define DMESG_MAX_US     15000U

#define SYSD_BURST_MIN    8000U
#define SYSD_BURST_MAX   20000U
#define SYSD_NORM_MIN    25000U
#define SYSD_NORM_MAX    70000U
#define SYSD_HITCH_MIN  110000U
#define SYSD_HITCH_MAX  195000U

static EFI_SYSTEM_TABLE             *gSys;
static EFI_BOOT_SERVICES            *gBoot;
static SIMPLE_TEXT_OUTPUT_INTERFACE *gCon;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gGop;

void *memset(void *dst, int val, size_t len) {
    UINT8 *p = (UINT8 *)dst;
    for (size_t i = 0; i < len; ++i) p[i] = (UINT8)val;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t len) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (size_t i = 0; i < len; ++i) d[i] = s[i];
    return dst;
}

static UINTN WcsLen(const CHAR16 *s) {
    UINTN n = 0;
    while (s && s[n]) ++n;
    return n;
}

static BOOLEAN WcsEqNoCase(const CHAR16 *a, const CHAR16 *b) {
    if (!a || !b) return FALSE;
    while (*a && *b) {
        CHAR16 ca = (*a >= L'A' && *a <= L'Z') ? (*a + 32) : *a;
        CHAR16 cb = (*b >= L'A' && *b <= L'Z') ? (*b + 32) : *b;
        if (ca != cb) return FALSE;
        ++a;
        ++b;
    }
    return *a == *b;
}

static BOOLEAN WcsStartsWithNoCase(const CHAR16 *s, const CHAR16 *prefix) {
    while (*prefix) {
        CHAR16 cs = (*s >= L'A' && *s <= L'Z') ? (*s + 32) : *s;
        CHAR16 cp = (*prefix >= L'A' && *prefix <= L'Z') ? (*prefix + 32) : *prefix;
        if (cs != cp) return FALSE;
        ++s;
        ++prefix;
    }
    return TRUE;
}

/* Live RDTSC mixed with xorshift64* */
static UINT32 RdtscJitterUs(UINT32 lo_us, UINT32 hi_us) {
    static UINT64 rng = 0x6a09e667f3bcc908ULL;
    if (hi_us <= lo_us) return lo_us;
    UINT32 eax = 0, edx = 0;
    __asm__ __volatile__("rdtsc" : "=a"(eax), "=d"(edx));
    rng ^= ((UINT64)edx << 32) | eax;
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    UINT64 mixed = rng * 0x2545f4914f6cdd1dULL;
    return lo_us + (UINT32)((mixed >> 16) % (hi_us - lo_us + 1U));
}

static EFI_STATUS FindProtocolInstance(EFI_GUID *guid, VOID **out_iface) {
    EFI_HANDLE list[64];
    UINTN bytes = sizeof(list);
    if (!EFI_ERROR(gBoot->LocateHandle(ByProtocol, guid, NULL, &bytes, list))) {
        UINTN count = bytes / sizeof(EFI_HANDLE);
        for (UINTN i = 0; i < count; ++i) {
            if (!EFI_ERROR(gBoot->HandleProtocol(list[i], guid, out_iface))) {
                return EFI_SUCCESS;
            }
        }
    }
    return gBoot->LocateProtocol ? gBoot->LocateProtocol(guid, NULL, out_iface) : EFI_NOT_FOUND;
}

/*
 * Requirement 2:
 * Force text mode, switch all GOP outputs to native max pixel resolution,
 * and pick the SimpleTextOutput console + mode with maximum (Columns * Rows)
 * (even if firmware ConSplitter caps default ConOut to 80x25).
 */
static void PrepareMaxResolutionConsole(void) {
    EFI_GUID cc_guid = CONSOLE_CONTROL_PROTO_GUID;
    CONSOLE_CTRL_PROTO *cc = NULL;
    if (!EFI_ERROR(FindProtocolInstance(&cc_guid, (VOID **)&cc)) && cc && cc->SetMode) {
        cc->SetMode(cc, 0);
    }

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    gGop = NULL;
    UINT64 best_global_px = 0;

    EFI_HANDLE gop_handles[32];
    UINTN gop_bytes = sizeof(gop_handles);
    if (!EFI_ERROR(gBoot->LocateHandle(ByProtocol, &gop_guid, NULL, &gop_bytes, gop_handles))) {
        UINTN gop_count = gop_bytes / sizeof(EFI_HANDLE);
        for (UINTN h = 0; h < gop_count; ++h) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(gop_handles[h], &gop_guid, (VOID **)&gop)) || !gop || !gop->Mode) {
                continue;
            }
            UINT32 best_mode = gop->Mode->Mode;
            UINT64 best_px = 0;
            for (UINT32 m = 0; m < gop->Mode->MaxMode; ++m) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
                UINTN sz = 0;
                if (!EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)) && info) {
                    UINT64 px = (UINT64)info->HorizontalResolution * info->VerticalResolution;
                    if (info->PixelFormat != PixelBltOnly && px >= best_px) {
                        best_px = px;
                        best_mode = m;
                    } else if (best_px == 0 && px > 0) {
                        best_px = px;
                        best_mode = m;
                    }
                    gBoot->FreePool(info);
                }
            }
            if (best_px > 0 && best_mode != gop->Mode->Mode) {
                gop->SetMode(gop, best_mode);
            }
            if (gop->Blt && gop->Mode->Info) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
                gop->Blt(gop, &black, EfiBltVideoFill, 0, 0, 0, 0,
                         gop->Mode->Info->HorizontalResolution,
                         gop->Mode->Info->VerticalResolution, 0);
                UINT64 cur_px = (UINT64)gop->Mode->Info->HorizontalResolution *
                                gop->Mode->Info->VerticalResolution;
                if (cur_px >= best_global_px) {
                    best_global_px = cur_px;
                    gGop = gop;
                }
            }
        }
    }
    if (!gGop && gSys->ConsoleOutHandle) {
        gBoot->HandleProtocol(gSys->ConsoleOutHandle, &gop_guid, (VOID **)&gGop);
    }

    gCon = gSys->ConOut;
    if (!gCon) return;

    SIMPLE_TEXT_OUTPUT_INTERFACE *best_con = gCon;
    UINTN best_mode = gCon->Mode ? (UINTN)gCon->Mode->Mode : 0;
    UINTN max_cells = 0;

    /* 1) Check all modes on SystemTable->ConOut */
    if (gCon->Mode && gCon->QueryMode) {
        for (INT32 m = 0; m < gCon->Mode->MaxMode; ++m) {
            UINTN cols = 0, rows = 0;
            if (!EFI_ERROR(gCon->QueryMode(gCon, (UINTN)m, &cols, &rows))) {
                UINTN cells = cols * rows;
                if (cells >= max_cells && cols >= 80 && rows >= 25) {
                    max_cells = cells;
                    best_mode = (UINTN)m;
                    best_con  = gCon;
                }
            }
        }
    }

    /* 2) Check direct GOP-backed GraphicsConsole handles in case ConSplitter capped modes to 80x25 */
    EFI_GUID sto_guid = SIMPLE_TEXT_OUT_PROTO_GUID;
    EFI_HANDLE sto_handles[32];
    UINTN sto_bytes = sizeof(sto_handles);
    if (!EFI_ERROR(gBoot->LocateHandle(ByProtocol, &sto_guid, NULL, &sto_bytes, sto_handles))) {
        UINTN sto_count = sto_bytes / sizeof(EFI_HANDLE);
        for (UINTN h = 0; h < sto_count; ++h) {
            VOID *dummy_gop = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(sto_handles[h], &gop_guid, &dummy_gop))) {
                continue; /* Ignore serial/UART consoles that lack GOP */
            }
            SIMPLE_TEXT_OUTPUT_INTERFACE *sto = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(sto_handles[h], &sto_guid, (VOID **)&sto)) ||
                !sto || !sto->Mode || !sto->QueryMode) {
                continue;
            }
            for (INT32 m = 0; m < sto->Mode->MaxMode; ++m) {
                UINTN cols = 0, rows = 0;
                if (!EFI_ERROR(sto->QueryMode(sto, (UINTN)m, &cols, &rows))) {
                    UINTN cells = cols * rows;
                    if (cells > max_cells && cols >= 80 && rows >= 25) {
                        max_cells = cells;
                        best_mode = (UINTN)m;
                        best_con  = sto;
                    }
                }
            }
        }
    }

    gCon = best_con;
    if (max_cells > 0 && gCon->SetMode) {
        gCon->SetMode(gCon, best_mode);
    }

    gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    gCon->ClearScreen(gCon);
    gCon->EnableCursor(gCon, FALSE);
}

/*
 * Stage 1: Rapid kernel dmesg burst (56 dense lines, ~1.2s total)
 */
typedef struct {
    const CHAR16 *ts;
    const CHAR16 *subsys;
    const CHAR16 *msg;
    UINT16        extra_ms;
} DmesgRow;

static const DmesgRow kDmesgRows[] = {
    { L"    0.000000", L"Linux version 6.12.6-amd64",  L"(gcc 14.2.0, GNU ld 2.43.1) #1 SMP PREEMPT_DYNAMIC", 0 },
    { L"    0.000000", L"Command line",                L"BOOT_IMAGE=/vmlinuz-6.12.6-amd64 ro quiet loglevel=4 mitigations=auto", 0 },
    { L"    0.000104", L"BIOS-e820",                   L"[mem 0x0000000000000000-0x000000000009ffff] usable", 0 },
    { L"    0.000118", L"BIOS-e820",                   L"[mem 0x0000000000100000-0x000000007a8defff] usable", 0 },
    { L"    0.000131", L"BIOS-e820",                   L"[mem 0x0000000100000000-0x000000087fffffff] usable", 0 },
    { L"    0.000412", L"NX (Execute Disable)",        L"protection: active", 0 },
    { L"    0.000689", L"efi",                         L"EFI v2.80 by American Megatrends: ACPI=0x7a9f2000 SMBIOS=0x7ab10000", 0 },
    { L"    0.001205", L"SMBIOS 3.6.0",                L"present, structures occupy 4192 bytes", 0 },
    { L"    0.002018", L"tsc",                         L"Fast TSC calibration using PIT: 3792.842 MHz", 15 },
    { L"    0.003491", L"MTRR",                        L"default type: uncachable, fixed ranges enabled, variable ranges 10", 0 },
    { L"    0.004128", L"ACPI",                        L"Core revision 20240827", 0 },
    { L"    0.004510", L"ACPI",                        L"RSDP 0x000000007A9F2014 000024 (v02 ALASKA)", 0 },
    { L"    0.004892", L"ACPI",                        L"XSDT 0x000000007A9F1728 0000FC (v01 ALASKA A M I    01072009)", 0 },
    { L"    0.005314", L"ACPI",                        L"FADT 0x000000007A9E9000 000114 (v06 ALASKA A M I    01072009)", 0 },
    { L"    0.006040", L"ACPI",                        L"MCFG 0x000000007A9ED280 00003C (v01 ALASKA A M I    01072009)", 0 },
    { L"    0.006419", L"ACPI",                        L"APIC 0x000000007A9EC400 00015C (v05 ALASKA A M I    01072009)", 0 },
    { L"    0.006875", L"ACPI",                        L"DMAR 0x000000007A9EE900 000088 (v01 INTEL  EDK2     00000002)", 0 },
    { L"    0.007912", L"Zone ranges",                 L"DMA32 [mem 0x00001000-0xffffffff], Normal [mem 0x100000000-0x87fffffff]", 0 },
    { L"    0.009480", L"SLUB",                        L"HWalign=64, Order=0-3, MinObjects=0, CPUs=16, Nodes=1", 0 },
    { L"    0.010824", L"rcu",                         L"Hierarchical RCU implementation, RCU restricting CPUs from NR_CPUS=512 to 16", 0 },
    { L"    0.012309", L"APIC",                        L"Switched APIC routing to physical x2apic", 15 },
    { L"    0.014112", L"IOAPIC[0]",                   L"apic_id 2, version 32, address 0xfec00000, GSI 0-119", 0 },
    { L"    0.016044", L"smpboot",                     L"Allowing 16 CPUs, 0 hotplug CPUs (x86_64 topo: 1 pkg, 8 cores, 2 threads)", 25 },
    { L"    0.017508", L"smp",                         L"Brought up 1 node, 16 CPUs — Total of 16 processors activated", 20 },
    { L"    0.018203", L"devtmpfs",                    L"initialized", 0 },
    { L"    0.018940", L"pci 0000:00:01.0",            L"PME# supported from D0 D3hot D3cold", 0 },
    { L"    0.019612", L"ACPI",                        L"PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])", 20 },
    { L"    0.020488", L"pci 0000:00:00.0",            L"[8086:a703] type 00 class 0x060000 Host bridge", 0 },
    { L"    0.021190", L"pci 0000:00:01.0",            L"PCI bridge to [bus 01] [mem 0x82000000-0x830fffff 64bit pref]", 0 },
    { L"    0.022045", L"pci 0000:00:06.0",            L"PCI bridge to [bus 02] (PCIe Gen4 Root Port x4)", 0 },
    { L"    0.022871", L"pci 0000:00:14.0",            L"[8086:7a60] type 00 class 0x0c0330 USB xHCI Controller", 0 },
    { L"    0.023510", L"pci 0000:00:17.0",            L"[8086:7a62] type 00 class 0x010601 AHCI SATA Controller", 0 },
    { L"    0.024904", L"pci 0000:01:00.0",            L"VGA compatible controller: BAR 0 [mem 0x6000000000-0x63ffffffff 64bit pref]", 25 },
    { L"    0.026318", L"DMAR",                        L"IOMMU enabled: Pass-Through=0, Queued invalidation & IRQ remapping active", 15 },
    { L"    0.028102", L"PCI",                         L"Using configuration type 1 for base access & MMCONFIG [bus 00-ff]", 0 },
    { L"    0.030441", L"clocksource",                 L"Switched to clocksource tsc-early (mask: 0xffffffffffffffff)", 0 },
    { L"    0.032119", L"pnp",                         L"PnP ACPI init — 9 system board resources parsed", 0 },
    { L"    0.034890", L"NET",                         L"Registered PF_INET, PF_INET6, PF_NETLINK, PF_UNIX protocol families", 0 },
    { L"    0.036712", L"PCI-DMA",                     L"Using software IO TLB [mem 0x74800000-0x78800000] (64MB)", 0 },
    { L"    0.038504", L" workingset",                 L"timestamp_bits=36 max_order=22 bucket_order=0", 0 },
    { L"    0.040192", L"cryptd",                      L"max_cpu_qlen set to 1000 (AES-NI, AVX2, SHA-NI hardware accel)", 0 },
    { L"    0.042110", L"nvme nvme0",                  L"pci function 0000:02:00.0 (MSI-X: 17 vectors allocated)", 35 },
    { L"    0.045820", L"nvme nvme0",                  L"16/0/0 default/read/poll queues, NVMe 1.4 Namespace 1 ready", 25 },
    { L"    0.047302", L" nvme0n1",                    L"p1 p2 p3 p4", 15 },
    { L"    0.049811", L"xhci_hcd 0000:00:14.0",       L"xHCI Host Controller — new USB bus registered, assigned bus number 1", 25 },
    { L"    0.051290", L"xhci_hcd 0000:00:14.0",       L"Host supports USB 3.2 Enhanced SuperSpeed (20 Gbps)", 0 },
    { L"    0.053408", L"usb 1-2",                     L"new full-speed USB device number 2 using xhci_hcd", 20 },
    { L"    0.055114", L"input",                       L"USB Gaming Keyboard as /devices/pci0000:00/0000:00:14.0/usb1/1-2/input0", 0 },
    { L"    0.056890", L"usb 1-4",                     L"new full-speed USB device number 3 using xhci_hcd (HID Optical Mouse)", 15 },
    { L"    0.058920", L"tpm_tis MSFT0101:00",         L"2.0 TPM (device-id 0x1B, rev-id 22) CRB interface active", 20 },
    { L"    0.061304", L"efivars",                     L"Registered efivars operations on EFI Runtime Services", 0 },
    { L"    0.064118", L"EXT4-fs (nvme0n1p4)",         L"mounted filesystem a8f1c902-441b-4d0e-9a77 ordered data mode", 35 },
    { L"    0.066205", L"VFS",                         L"Mounted root (ext4 filesystem) readonly on device 259:4", 0 },
    { L"    0.068012", L"Freeing unused kernel image", L"(initmem) memory: 3148K", 0 },
    { L"    0.069440", L"Write protecting",            L"kernel read-only data: 26624k", 0 },
    { L"    0.071880", L"systemd[1]",                  L"systemd 256.8 running in system mode (+PAM +AUDIT +SECCOMP +OPENSSL +TPM2 +ZSTD)", 30 },
};

static void EmitDmesgStage(void) {
    const UINTN count = sizeof(kDmesgRows) / sizeof(kDmesgRows[0]);
    for (UINTN i = 0; i < count; ++i) {
        gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, L"[");
        gCon->OutputString(gCon, (CHAR16 *)kDmesgRows[i].ts);
        gCon->OutputString(gCon, L"] ");

        gCon->SetAttribute(gCon, EFI_WHITE | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, (CHAR16 *)kDmesgRows[i].subsys);
        gCon->OutputString(gCon, L": ");

        gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, (CHAR16 *)kDmesgRows[i].msg);
        gCon->OutputString(gCon, L"\r\n");

        UINT32 delay = RdtscJitterUs(DMESG_MIN_US, DMESG_MAX_US) +
                       ((UINT32)kDmesgRows[i].extra_ms * 1000U);
        gBoot->Stall(delay);
    }
}

/*
 * Stage 2: systemd service graph (~3.5s total, 50 lines with burst/normal/hitch cadence,
 * green OK, cyan device/mount identifiers, white service names, no ClearScreen at end).
 */
typedef enum {
    PACE_BURST = 0,
    PACE_NORM  = 1,
    PACE_HITCH = 2,
    PACE_FINAL = 3
} StepPace;

typedef struct {
    BOOLEAN       is_ok;      /* TRUE = "[  OK  ] ", FALSE = "         " */
    const CHAR16 *verb;       /* e.g. "Started ", "Mounted ", "Reached target " */
    const CHAR16 *dev_cyan;   /* Optional device/path highlighted in EFI_LIGHTCYAN */
    const CHAR16 *unit_white; /* Main description in EFI_WHITE */
    StepPace      pace;
} SystemdRow;

static const SystemdRow kSystemdRows[] = {
    { TRUE,  L"Created slice ",   L"system-modprobe.slice",        L" — Slice /system/modprobe",                      PACE_BURST },
    { TRUE,  L"Created slice ",   L"user.slice",                   L" — User and Session Slice",                      PACE_BURST },
    { TRUE,  L"Reached target ",  NULL,                            L"Local Encrypted Volumes",                        PACE_BURST },
    { TRUE,  L"Listening on ",    L"/run/systemd/journal/socket",  L" — Journal Socket",                              PACE_BURST },
    { TRUE,  L"Listening on ",    L"/run/udev/control",            L" — udev Control Socket",                         PACE_BURST },
    { TRUE,  L"Listening on ",    L"/run/udev/kernel",             L" — udev Kernel Netlink Socket",                  PACE_BURST },
    { TRUE,  L"Mounted ",         L"/dev/hugepages",               L" — POSIX Huge Pages File System",                PACE_NORM  },
    { TRUE,  L"Mounted ",         L"/dev/mqueue",                  L" — POSIX Message Queue File System",             PACE_BURST },
    { TRUE,  L"Mounted ",         L"/sys/kernel/debug",            L" — Kernel Debug File System",                    PACE_BURST },
    { TRUE,  L"Mounted ",         L"/sys/kernel/tracing",          L" — Kernel Trace File System",                    PACE_BURST },
    { TRUE,  L"Mounted ",         L"/sys/firmware/efi/efivars",    L" — EFI Variable File System",                    PACE_NORM  },
    { TRUE,  L"Finished ",        NULL,                            L"Load Kernel Modules (nvme, xhci_pci, vfat)",     PACE_NORM  },
    { TRUE,  L"Finished ",        NULL,                            L"Remount Root and Kernel File Systems",           PACE_HITCH },
    { TRUE,  L"Started ",         NULL,                            L"Journal Service (systemd-journald)",             PACE_NORM  },
    { TRUE,  L"Finished ",        NULL,                            L"Apply Kernel Variables (sysctl)",                PACE_BURST },
    { TRUE,  L"Finished ",        NULL,                            L"Create Static Device Nodes in /dev",             PACE_BURST },
    { TRUE,  L"Started ",         NULL,                            L"Rule-based Manager for Device Events and Files", PACE_HITCH },
    { TRUE,  L"Finished ",        NULL,                            L"Coldplug All udev Devices",                      PACE_HITCH },
    { TRUE,  L"Found device ",    L"/dev/nvme0n1p1",               L" — EFI System Partition",                        PACE_NORM  },
    { TRUE,  L"Found device ",    L"/dev/nvme0n1p3",               L" — Linux Swap Partition",                        PACE_BURST },
    { TRUE,  L"Found device ",    L"/dev/tpmrm0",                  L" — TPM 2.0 Security Processor",                  PACE_NORM  },
    { TRUE,  L"Activated swap ",  L"/dev/nvme0n1p3",               L"",                                               PACE_NORM  },
    { TRUE,  L"Reached target ",  NULL,                            L"Swaps",                                          PACE_BURST },
    { TRUE,  L"Finished ",        L"/dev/nvme0n1p1",               L" — File System Check on EFI Partition",          PACE_HITCH },
    { TRUE,  L"Mounted ",         L"/boot/efi",                    L" — EFI System Partition (FAT32)",                PACE_NORM  },
    { TRUE,  L"Mounted ",         L"/tmp",                         L" — Temporary Directory tmpfs",                   PACE_BURST },
    { TRUE,  L"Reached target ",  NULL,                            L"Local File Systems",                             PACE_BURST },
    { TRUE,  L"Finished ",        NULL,                            L"Flush Journal to Persistent Storage",            PACE_HITCH },
    { TRUE,  L"Finished ",        NULL,                            L"Create Volatile Files and Directories",          PACE_NORM  },
    { TRUE,  L"Finished ",        NULL,                            L"TPM2 PCR Machine ID Measurement",                PACE_NORM  },
    { TRUE,  L"Started ",         NULL,                            L"Entropy Daemon based on Hardware RNG",           PACE_BURST },
    { TRUE,  L"Started ",         NULL,                            L"Load/Save RF Kill Switch Status",                PACE_NORM  },
    { TRUE,  L"Found device ",    L"enp4s0",                       L" — PCIe 2.5GbE Ethernet Controller",             PACE_NORM  },
    { TRUE,  L"Found device ",    L"usb1-port2",                   L" — USB Composite HID Input",                     PACE_BURST },
    { TRUE,  L"Reached target ",  NULL,                            L"System Initialization",                          PACE_NORM  },
    { TRUE,  L"Listening on ",    L"/run/dbus/system_bus_socket",  L" — D-Bus System Message Bus Socket",             PACE_BURST },
    { TRUE,  L"Reached target ",  NULL,                            L"Basic System",                                   PACE_BURST },
    { TRUE,  L"Started ",         NULL,                            L"D-Bus System Message Bus",                       PACE_NORM  },
    { TRUE,  L"Started ",         L"enp4s0",                       L" — Network Configuration (systemd-networkd)",    PACE_HITCH },
    { TRUE,  L"Started ",         NULL,                            L"Network Name Resolution (systemd-resolved)",     PACE_NORM  },
    { TRUE,  L"Reached target ",  NULL,                            L"Network",                                        PACE_BURST },
    { TRUE,  L"Started ",         NULL,                            L"Authorization Manager (polkitd)",                PACE_NORM  },
    { TRUE,  L"Started ",         NULL,                            L"Power Profiles & ACPI Thermal Daemon",           PACE_NORM  },
    { TRUE,  L"Started ",         NULL,                            L"Disk Manager (udisksd)",                         PACE_HITCH },
    { TRUE,  L"Started ",         NULL,                            L"User Login Management (systemd-logind)",         PACE_NORM  },
    { TRUE,  L"Finished ",        NULL,                            L"Permit User Sessions",                           PACE_BURST },
    { TRUE,  L"Started ",         L"tty1",                         L" — Getty on Virtual Console 1",                  PACE_NORM  },
    { TRUE,  L"Reached target ",  NULL,                            L"Multi-User System",                              PACE_NORM  },
    { TRUE,  L"Reached target ",  NULL,                            L"Graphical Interface",                            PACE_NORM  },
    { FALSE, L"Starting ",        NULL,                            L"Windows Boot Manager...",                        PACE_FINAL },
};

static void EmitSystemdStage(void) {
    const UINTN count = sizeof(kSystemdRows) / sizeof(kSystemdRows[0]);
    for (UINTN i = 0; i < count; ++i) {
        const SystemdRow *r = &kSystemdRows[i];

        if (r->is_ok) {
            gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, L"[  ");
            gCon->SetAttribute(gCon, EFI_LIGHTGREEN | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, L"OK");
            gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, L"  ] ");
        } else {
            gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, L"         ");
        }

        gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, (CHAR16 *)r->verb);

        if (r->dev_cyan && r->dev_cyan[0]) {
            gCon->SetAttribute(gCon, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, (CHAR16 *)r->dev_cyan);
        }

        if (r->unit_white && r->unit_white[0]) {
            gCon->SetAttribute(gCon, EFI_WHITE | EFI_BACKGROUND_BLACK);
            gCon->OutputString(gCon, (CHAR16 *)r->unit_white);
        }

        gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        if (r->is_ok) {
            gCon->OutputString(gCon, L".\r\n");
        } else {
            gCon->OutputString(gCon, L"\r\n");
        }

        switch (r->pace) {
            case PACE_BURST:
                gBoot->Stall(RdtscJitterUs(SYSD_BURST_MIN, SYSD_BURST_MAX));
                break;
            case PACE_NORM:
                gBoot->Stall(RdtscJitterUs(SYSD_NORM_MIN, SYSD_NORM_MAX));
                break;
            case PACE_HITCH:
                gBoot->Stall(RdtscJitterUs(SYSD_NORM_MIN, SYSD_NORM_MAX) +
                             RdtscJitterUs(SYSD_HITCH_MIN, SYSD_HITCH_MAX));
                break;
            case PACE_FINAL:
                /* No stall and no ClearScreen: hand off immediately to bootmgfw.efi */
                break;
        }
    }
}

/* Chainloading: read optional boot=... in \EFI\HackBGRT\config.txt, then bootmgfw.efi */
static EFI_DEVICE_PATH *MakePartitionFilePath(EFI_HANDLE part_handle, const CHAR16 *rel_path) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *base_dp = NULL;
    if (EFI_ERROR(gBoot->HandleProtocol(part_handle, &dp_guid, (VOID **)&base_dp)) || !base_dp) {
        return NULL;
    }

    UINTN prefix_bytes = 0;
    for (EFI_DEVICE_PATH *it = base_dp; !IsDevicePathEnd(it); it = NextDevicePathNode(it)) {
        prefix_bytes += DevicePathNodeLength(it);
    }

    UINTN path_bytes = (WcsLen(rel_path) + 1) * sizeof(CHAR16);
    UINTN file_node_bytes = SIZE_OF_FILEPATH_DEVICE_PATH + path_bytes;
    UINTN end_node_bytes = sizeof(EFI_DEVICE_PATH);

    UINT8 *buf = NULL;
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, prefix_bytes + file_node_bytes + end_node_bytes, (VOID **)&buf))) {
        return NULL;
    }

    gBoot->CopyMem(buf, base_dp, prefix_bytes);

    FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH *)(buf + prefix_bytes);
    fp->Header.Type = MEDIA_DEVICE_PATH;
    fp->Header.SubType = MEDIA_FILEPATH_DP;
    fp->Header.Length[0] = (UINT8)(file_node_bytes & 0xFF);
    fp->Header.Length[1] = (UINT8)((file_node_bytes >> 8) & 0xFF);
    gBoot->CopyMem(fp->PathName, (VOID *)rel_path, path_bytes);

    EFI_DEVICE_PATH *end = (EFI_DEVICE_PATH *)(buf + prefix_bytes + file_node_bytes);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = (UINT8)end_node_bytes;
    end->Length[1] = 0;

    return (EFI_DEVICE_PATH *)buf;
}

static CHAR16 *ParseBootTargetFromConfig(EFI_HANDLE part_handle) {
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_FILE_IO_INTERFACE *vol = NULL;
    if (EFI_ERROR(gBoot->HandleProtocol(part_handle, &fs_guid, (VOID **)&vol)) || !vol) {
        return NULL;
    }
    EFI_FILE_HANDLE root = NULL;
    if (EFI_ERROR(vol->OpenVolume(vol, &root)) || !root) {
        return NULL;
    }
    EFI_FILE_HANDLE cfg = NULL;
    if (EFI_ERROR(root->Open(root, &cfg, L"\\EFI\\HackBGRT\\config.txt", EFI_FILE_MODE_READ, 0)) || !cfg) {
        root->Close(root);
        return NULL;
    }

    enum { CFG_MAX_BYTES = 8192 };
    UINT8 *raw = NULL;
    CHAR16 *wide = NULL;
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, CFG_MAX_BYTES, (VOID **)&raw)) ||
        EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, CFG_MAX_BYTES * sizeof(CHAR16), (VOID **)&wide))) {
        if (raw) gBoot->FreePool(raw);
        cfg->Close(cfg);
        root->Close(root);
        return NULL;
    }

    UINTN len = CFG_MAX_BYTES - 2;
    EFI_STATUS st = cfg->Read(cfg, &len, raw);
    cfg->Close(cfg);
    root->Close(root);
    if (EFI_ERROR(st) || len == 0) {
        gBoot->FreePool(wide);
        gBoot->FreePool(raw);
        return NULL;
    }

    UINTN wlen = 0;
    if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        wlen = (len - 2) / 2;
        gBoot->CopyMem(wide, raw + 2, wlen * sizeof(CHAR16));
    } else {
        UINTN start = (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        for (UINTN i = start; i < len && wlen < (CFG_MAX_BYTES - 1); ++i) {
            wide[wlen++] = (CHAR16)raw[i];
        }
    }
    wide[wlen] = 0;
    gBoot->FreePool(raw);

    CHAR16 *result = NULL;
    UINTN pos = 0;
    while (pos < wlen) {
        while (pos < wlen && (wide[pos] == L' ' || wide[pos] == L'\t' || wide[pos] == L'\r' || wide[pos] == L'\n')) {
            ++pos;
        }
        UINTN line_start = pos;
        while (pos < wlen && wide[pos] != L'\r' && wide[pos] != L'\n') {
            ++pos;
        }
        wide[pos++] = 0;

        const CHAR16 *line = &wide[line_start];
        if (line[0] != L'#' && WcsStartsWithNoCase(line, L"boot=")) {
            const CHAR16 *val = line + 5;
            while (*val == L' ' || *val == L'\t') ++val;
            UINTN vlen = WcsLen(val);
            if (vlen > 0) {
                if (result) gBoot->FreePool(result);
                result = NULL;
                if (!EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, (vlen + 1) * sizeof(CHAR16), (VOID **)&result))) {
                    gBoot->CopyMem(result, (VOID *)val, (vlen + 1) * sizeof(CHAR16));
                }
            }
        }
    }
    gBoot->FreePool(wide);
    return result;
}

static BOOLEAN BufferHasAscii(const UINT8 *buf, UINTN buf_len, const char *needle) {
    UINTN nlen = 0;
    while (needle[nlen]) ++nlen;
    if (nlen == 0 || buf_len < nlen) return FALSE;
    for (UINTN i = 0; i <= buf_len - nlen; ++i) {
        UINTN j = 0;
        while (j < nlen && buf[i + j] == (UINT8)needle[j]) ++j;
        if (j == nlen) return TRUE;
    }
    return FALSE;
}

/*
 * Verify backup .efi exists and is neither our own LinuxBootSim image
 * nor a UEFI Shim binary (prevents infinite chainload loops if .bak was overwritten).
 */
static BOOLEAN IsValidBackupEfi(EFI_HANDLE part_handle, const CHAR16 *rel_path) {
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_FILE_IO_INTERFACE *vol = NULL;
    if (EFI_ERROR(gBoot->HandleProtocol(part_handle, &fs_guid, (VOID **)&vol)) || !vol) {
        return FALSE;
    }
    EFI_FILE_HANDLE root = NULL;
    if (EFI_ERROR(vol->OpenVolume(vol, &root)) || !root) {
        return FALSE;
    }
    EFI_FILE_HANDLE fh = NULL;
    if (EFI_ERROR(root->Open(root, &fh, (CHAR16 *)rel_path, EFI_FILE_MODE_READ, 0)) || !fh) {
        root->Close(root);
        return FALSE;
    }

    UINT64 size64 = 0;
    fh->SetPosition(fh, ~(UINT64)0);
    fh->GetPosition(fh, &size64);
    fh->SetPosition(fh, 0);

    UINTN size = (UINTN)size64;
    if (size < 512 || size > (32U * 1024U * 1024U)) {
        fh->Close(fh);
        root->Close(root);
        return FALSE;
    }

    UINT8 *data = NULL;
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, size, (VOID **)&data))) {
        fh->Close(fh);
        root->Close(root);
        return FALSE;
    }

    EFI_STATUS st = fh->Read(fh, &size, data);
    fh->Close(fh);
    root->Close(root);
    if (EFI_ERROR(st)) {
        gBoot->FreePool(data);
        return FALSE;
    }

    BOOLEAN is_loop_hazard = BufferHasAscii(data, size, "LinuxBootSim") ||
                             BufferHasAscii(data, size, "UEFI shim");
    gBoot->FreePool(data);
    return !is_loop_hazard;
}

static EFI_STATUS ChainloadNextLoader(EFI_HANDLE self_handle) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *self_img = NULL;
    if (EFI_ERROR(gBoot->HandleProtocol(self_handle, &li_guid, (VOID **)&self_img)) || !self_img) {
        return EFI_LOAD_ERROR;
    }

    const CHAR16 *paths[5];
    UINTN n_paths = 0;

    /* 1. Primary: chainload original HackBGRT backup so it applies splash.bmp to ACPI BGRT */
    if (IsValidBackupEfi(self_img->DeviceHandle, L"\\EFI\\HackBGRT\\grubx64.efi.bak")) {
        paths[n_paths++] = L"\\EFI\\HackBGRT\\grubx64.efi.bak";
    }
    if (IsValidBackupEfi(self_img->DeviceHandle, L"\\EFI\\HackBGRT\\loader.efi.bak")) {
        paths[n_paths++] = L"\\EFI\\HackBGRT\\loader.efi.bak";
    }

    /* 2. Fallback: custom boot=... from config.txt, backup MS loader, or direct bootmgfw.efi */
    CHAR16 *cfg_target = ParseBootTargetFromConfig(self_img->DeviceHandle);
    if (cfg_target && !WcsEqNoCase(cfg_target, L"MS") &&
        !WcsEqNoCase(cfg_target, L"\\EFI\\HackBGRT\\loader.efi") &&
        !WcsEqNoCase(cfg_target, L"\\EFI\\HackBGRT\\grubx64.efi")) {
        paths[n_paths++] = cfg_target;
    }
    if (IsValidBackupEfi(self_img->DeviceHandle, L"\\EFI\\HackBGRT\\bootmgfw-original.efi")) {
        paths[n_paths++] = L"\\EFI\\HackBGRT\\bootmgfw-original.efi";
    }
    paths[n_paths++] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

    EFI_STATUS status = EFI_NOT_FOUND;
    for (UINTN i = 0; i < n_paths; ++i) {
        EFI_DEVICE_PATH *dp = MakePartitionFilePath(self_img->DeviceHandle, paths[i]);
        if (!dp) continue;

        EFI_HANDLE next_img = NULL;
        status = gBoot->LoadImage(FALSE, self_handle, dp, NULL, 0, &next_img);
        gBoot->FreePool(dp);
        if (EFI_ERROR(status) || !next_img) continue;

        status = gBoot->StartImage(next_img, NULL, NULL);
        if (!EFI_ERROR(status)) break;
        if (gBoot->UnloadImage) gBoot->UnloadImage(next_img);
    }

    if (cfg_target) gBoot->FreePool(cfg_target);
    return status;
}

static inline UINT64 ReadTsc64(void) {
    UINT32 eax = 0, edx = 0;
    __asm__ __volatile__("rdtsc" : "=a"(eax), "=d"(edx));
    return ((UINT64)edx << 32) | eax;
}

static BOOLEAN PollSkipKey(void) {
    if (!gSys || !gSys->ConIn || !gSys->ConIn->ReadKeyStroke) {
        return FALSE;
    }
    EFI_INPUT_KEY key;
    return !EFI_ERROR(gSys->ConIn->ReadKeyStroke(gSys->ConIn, &key));
}

static void DrainBufferedKeys(void) {
    if (!gSys || !gSys->ConIn || !gSys->ConIn->ReadKeyStroke) {
        return;
    }
    EFI_INPUT_KEY dummy;
    while (!EFI_ERROR(gSys->ConIn->ReadKeyStroke(gSys->ConIn, &dummy))) {
    }
}

/* Fast 64-bit/32-bit pixel span fill (writes 2 pixels per UINT64 store) */
static inline void FillPixels32(UINT32 *dst, UINTN count, UINT32 color32) {
    UINT64 pair = ((UINT64)color32 << 32) | (UINT64)color32;
    UINT64 *dst64 = (UINT64 *)dst;
    UINTN pairs = count >> 1;
    while (pairs >= 4U) {
        dst64[0] = pair;
        dst64[1] = pair;
        dst64[2] = pair;
        dst64[3] = pair;
        dst64 += 4;
        pairs -= 4U;
    }
    while (pairs--) {
        *dst64++ = pair;
    }
    if (count & 1U) {
        *(UINT32 *)dst64 = color32;
    }
}

/*
 * Direct 24-bit TrueColor GOP->Blt() HD block video player (320x180, 255 RGB colors + temporal skip):
 * - Exact full-screen integer scaling on 16:9 displays (6x = 1920x1080, 8x = 2560x1440, 12x = 3840x2160)
 * - Uses 32-bit/64-bit word stores into offscreen UINT32 pixel buffer (zero byte-by-byte overhead)
 * - Blits only the active dirty scanline band in a single tear-free EfiBltBufferToVideo call per frame
 * - Frame-locked to 24 FPS via calibrated RDTSC + BS->Stall()
 * - Exits immediately on any keystroke via ReadKeyStroke and frees memory before chainloading
 */
static void PlayGopVideo(void) {
    if (!gGop || !gGop->Blt || !gGop->Mode || !gGop->Mode->Info) {
        return;
    }

    UINTN screen_w = gGop->Mode->Info->HorizontalResolution;
    UINTN screen_h = gGop->Mode->Info->VerticalResolution;
    if (screen_w == 0 || screen_h == 0) {
        return;
    }

    /* Clear screen to pure black via GOP before starting video */
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    gGop->Blt(gGop, &black, EfiBltVideoFill, 0, 0, 0, 0, screen_w, screen_h, 0);
    if (gCon && gCon->EnableCursor) {
        gCon->EnableCursor(gCon, FALSE);
    }
    DrainBufferedKeys();

    UINTN scale_x = screen_w / GOP_VIDEO_WIDTH;
    UINTN scale_y = screen_h / GOP_VIDEO_HEIGHT;
    UINTN scale   = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1U) {
        scale = 1U;
    }

    UINTN render_w = GOP_VIDEO_WIDTH  * scale;
    UINTN render_h = GOP_VIDEO_HEIGHT * scale;
    if (render_w > screen_w) render_w = screen_w;
    if (render_h > screen_h) render_h = screen_h;

    UINTN dst_x = (screen_w > render_w) ? ((screen_w - render_w) / 2U) : 0U;
    UINTN dst_y = (screen_h > render_h) ? ((screen_h - render_h) / 2U) : 0U;

    UINTN buf_pixels = render_w * render_h;
    UINTN buf_bytes  = buf_pixels * sizeof(UINT32);
    UINT32 *blt_u32  = NULL;
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, buf_bytes, (VOID **)&blt_u32)) || !blt_u32) {
        return;
    }
    gBoot->SetMem(blt_u32, buf_bytes, 0);

    /* Precompute darkened scanline palette once (7/8 brightness in 32-bit BGR0) */
    UINT32 edge_palette32[255];
    for (UINTN i = 0; i < 255U; ++i) {
        UINT32 c = kGopVideoPalette32[i];
        UINT32 b = (((c >>  0) & 0xFFU) * 7U) >> 3;
        UINT32 g = (((c >>  8) & 0xFFU) * 7U) >> 3;
        UINT32 r = (((c >> 16) & 0xFFU) * 7U) >> 3;
        edge_palette32[i] = b | (g << 8) | (r << 16);
    }

    /* Calibrate RDTSC ticks/us over a 20 ms stall */
    UINT64 cal_start = ReadTsc64();
    gBoot->Stall(20000U);
    UINT64 tsc_per_us = (ReadTsc64() - cal_start) / 20000ULL;
    if (tsc_per_us == 0) {
        tsc_per_us = 3000ULL;
    }

    const UINTN total_cells = (UINTN)GOP_VIDEO_WIDTH * (UINTN)GOP_VIDEO_HEIGHT;
    const BOOLEAN draw_scanline = (scale >= 4U);

    for (UINT32 f = 0; f < GOP_VIDEO_FRAME_COUNT; ++f) {
        UINT64 frame_t0 = ReadTsc64();
        if (PollSkipKey()) {
            break;
        }

        UINT32 run_idx = kGopFrameOffsets[f];
        UINT32 run_end = kGopFrameOffsets[f + 1U];
        UINTN  cell_pos = 0;
        UINTN  dirty_y_min = render_h;
        UINTN  dirty_y_max = 0;

        while (run_idx < run_end && cell_pos < total_cells) {
            UINT16 token     = kGopVideoRunsU16[run_idx++];
            UINT8  color_idx = (UINT8)(token & 0xFFU);
            UINTN  rl        = (UINTN)(token >> 8);

            if (cell_pos + rl > total_cells) {
                rl = total_cells - cell_pos;
            }

            if (color_idx >= GOP_VIDEO_SKIP_INDEX) {
                cell_pos += rl;
                continue;
            }

            UINT32 main32 = kGopVideoPalette32[color_idx];
            UINT32 edge32 = edge_palette32[color_idx];

            UINTN rem = rl;
            while (rem > 0) {
                UINTN gy = cell_pos / GOP_VIDEO_WIDTH;
                UINTN gx = cell_pos % GOP_VIDEO_WIDTH;
                UINTN span_cells = GOP_VIDEO_WIDTH - gx;
                if (span_cells > rem) {
                    span_cells = rem;
                }

                UINTN px_x0 = gx * scale;
                UINTN px_y0 = gy * scale;
                UINTN px_w  = span_cells * scale;
                if (px_x0 < render_w && px_y0 < render_h) {
                    if (px_x0 + px_w > render_w) {
                        px_w = render_w - px_x0;
                    }
                    UINTN px_h = scale;
                    if (px_y0 + px_h > render_h) {
                        px_h = render_h - px_y0;
                    }

                    if (px_y0 < dirty_y_min) {
                        dirty_y_min = px_y0;
                    }
                    if (px_y0 + px_h > dirty_y_max) {
                        dirty_y_max = px_y0 + px_h;
                    }

                    UINT32 *row_ptr = blt_u32 + px_y0 * render_w + px_x0;
                    UINTN body_rows = (draw_scanline && px_h == scale) ? (px_h - 1U) : px_h;
                    for (UINTN sy = 0; sy < body_rows; ++sy) {
                        FillPixels32(row_ptr, px_w, main32);
                        row_ptr += render_w;
                    }
                    if (body_rows < px_h) {
                        FillPixels32(row_ptr, px_w, edge32);
                    }
                }

                cell_pos += span_cells;
                rem      -= span_cells;
            }
        }

        if (PollSkipKey()) {
            break;
        }

        if (dirty_y_max > dirty_y_min) {
            gGop->Blt(
                gGop,
                (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)blt_u32,
                EfiBltBufferToVideo,
                0, dirty_y_min,
                dst_x, dst_y + dirty_y_min,
                render_w, dirty_y_max - dirty_y_min,
                render_w * sizeof(UINT32)
            );
        }

        if (PollSkipKey()) {
            break;
        }

        UINT64 elapsed_us = (ReadTsc64() - frame_t0) / tsc_per_us;
        if (elapsed_us < GOP_VIDEO_FRAME_US) {
            gBoot->Stall((UINTN)(GOP_VIDEO_FRAME_US - elapsed_us));
        }
    }

    gBoot->FreePool(blt_u32);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    gSys  = system_table;
    gBoot = system_table->BootServices;

    PrepareMaxResolutionConsole();
#if ENABLE_LINUX_INTRO
    EmitDmesgStage();
    EmitSystemdStage();
#endif
    PlayGopVideo();

    EFI_STATUS st = ChainloadNextLoader(image_handle);
    if (EFI_ERROR(st) && gCon) {
        gCon->SetAttribute(gCon, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, L"[FAILED] Unable to start bootmgfw.efi\r\n");
        gBoot->Stall(5000000U);
    }
    return st;
}
