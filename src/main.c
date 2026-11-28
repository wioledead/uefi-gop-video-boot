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
static UINT64                        gTscPerUs = 3000ULL;

typedef enum {
    ACT_NONE = 0,
    ACT_SKIP_TO_OS,      /* Space / Enter / Esc / Q -> chainload Windows immediately */
    ACT_SKIP_INTRO,      /* Tab -> jump from Linux text intro straight into video */
    ACT_PAUSE_TOGGLE,    /* P -> pause / resume video */
    ACT_SEEK_BACK,       /* Left Arrow -> rewind 2 seconds */
    ACT_SEEK_FWD,        /* Right Arrow -> fast-forward 2 seconds */
    ACT_STEP_BACK,       /* , or < -> step -1 frame (pauses) */
    ACT_STEP_FWD,        /* . or > -> step +1 frame (pauses) */
    ACT_REPLAY,          /* R or Home -> restart video from frame 0 */
    ACT_TOGGLE_SCANLINE, /* S -> toggle scanline shading live */
    ACT_REBOOT_BIOS      /* F2 or Del -> reboot into UEFI Firmware Setup */
} UserAction;

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
static inline UINT64 ReadTsc64(void) {
    UINT32 eax = 0, edx = 0;
    __asm__ __volatile__("rdtsc" : "=a"(eax), "=d"(edx));
    return ((UINT64)edx << 32) | eax;
}

static inline void CpuIdEx(UINT32 leaf, UINT32 subleaf, UINT32 *eax, UINT32 *ebx, UINT32 *ecx, UINT32 *edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

static UINT32 RdtscJitterUs(UINT32 lo_us, UINT32 hi_us) {
    static UINT64 rng = 0x6a09e667f3bcc908ULL;
    if (hi_us <= lo_us) return lo_us;
    rng ^= ReadTsc64();
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    UINT64 mixed = rng * 0x2545f4914f6cdd1dULL;
    return lo_us + (UINT32)((mixed >> 16) % (hi_us - lo_us + 1U));
}

static BOOLEAN GuidEq(const EFI_GUID *a, const EFI_GUID *b) {
    if (!a || !b) return FALSE;
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); ++i) {
        if (pa[i] != pb[i]) return FALSE;
    }
    return TRUE;
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

/* Freestanding string builders (zero CRT, zero stack bloat) */
typedef struct {
    CHAR16 *buf;
    UINTN   cap;
    UINTN   len;
} WBuf;

static void WBufInit(WBuf *wb, CHAR16 *storage, UINTN cap) {
    wb->buf = storage;
    wb->cap = cap;
    wb->len = 0;
    if (cap > 0) storage[0] = 0;
}

static void WBufPutc(WBuf *wb, CHAR16 c) {
    if (wb->cap > 1 && wb->len + 1 < wb->cap) {
        wb->buf[wb->len++] = c;
        wb->buf[wb->len]   = 0;
    }
}

static void WBufPuts(WBuf *wb, const CHAR16 *s) {
    if (!s) return;
    while (*s) WBufPutc(wb, *s++);
}

static void WBufPutAsciiN(WBuf *wb, const char *s, UINTN max_chars) {
    if (!s) return;
    for (UINTN i = 0; i < max_chars && s[i]; ++i) {
        UINT8 ch = (UINT8)s[i];
        WBufPutc(wb, (ch >= 32 && ch <= 126) ? (CHAR16)ch : L' ');
    }
}

static void WBufPutAsciiTrim(WBuf *wb, const char *s, UINTN max_chars) {
    if (!s) return;
    UINTN n = 0;
    while (n < max_chars && s[n]) ++n;
    UINTN start = 0;
    while (start < n && (UINT8)s[start] <= 32) ++start;
    while (n > start && (UINT8)s[n - 1] <= 32) --n;
    BOOLEAN prev_space = FALSE;
    for (UINTN i = start; i < n; ++i) {
        UINT8 ch = (UINT8)s[i];
        BOOLEAN is_sp = (ch <= 32);
        if (is_sp) {
            if (!prev_space) WBufPutc(wb, L' ');
            prev_space = TRUE;
        } else if (ch <= 126) {
            WBufPutc(wb, (CHAR16)ch);
            prev_space = FALSE;
        }
    }
}

static void WBufPutDec(WBuf *wb, UINT64 val) {
    CHAR16 tmp[24];
    UINTN n = 0;
    if (val == 0) {
        WBufPutc(wb, L'0');
        return;
    }
    while (val > 0 && n < sizeof(tmp) / sizeof(tmp[0])) {
        tmp[n++] = (CHAR16)(L'0' + (val % 10ULL));
        val /= 10ULL;
    }
    while (n > 0) {
        WBufPutc(wb, tmp[--n]);
    }
}

static void WBufPutDecPad(WBuf *wb, UINT64 val, UINTN width, CHAR16 pad) {
    CHAR16 tmp[24];
    UINTN n = 0;
    if (val == 0) {
        tmp[n++] = L'0';
    } else {
        while (val > 0 && n < sizeof(tmp) / sizeof(tmp[0])) {
            tmp[n++] = (CHAR16)(L'0' + (val % 10ULL));
            val /= 10ULL;
        }
    }
    while (n < width) {
        WBufPutc(wb, pad);
        --width;
    }
    while (n > 0) {
        WBufPutc(wb, tmp[--n]);
    }
}

static void WBufPutHex(WBuf *wb, UINT64 val, UINTN min_digits, BOOLEAN upper) {
    const CHAR16 *hex = upper ? L"0123456789ABCDEF" : L"0123456789abcdef";
    CHAR16 tmp[16];
    UINTN n = 0;
    if (val == 0) {
        tmp[n++] = L'0';
    } else {
        while (val > 0 && n < 16) {
            tmp[n++] = hex[val & 0xFULL];
            val >>= 4;
        }
    }
    while (n < min_digits && min_digits <= 16) {
        WBufPutc(wb, L'0');
        --min_digits;
    }
    while (n > 0) {
        WBufPutc(wb, tmp[--n]);
    }
}

/* Reboot directly into motherboard UEFI Firmware Setup via OsIndications */
static void TryRebootToFirmwareUI(void) {
    if (!gSys || !gSys->RuntimeServices || !gSys->RuntimeServices->SetVariable || !gSys->RuntimeServices->ResetSystem) {
        return;
    }
    EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
    UINT64 indications = 0;
    UINTN  data_size   = sizeof(indications);
    UINT32 attrs       = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

    if (gSys->RuntimeServices->GetVariable) {
        UINT32 existing_attrs = 0;
        if (!EFI_ERROR(gSys->RuntimeServices->GetVariable(L"OsIndications", &global_guid, &existing_attrs, &data_size, &indications))) {
            if (existing_attrs != 0) attrs = existing_attrs;
        } else {
            indications = 0;
        }
    }

    indications |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    EFI_STATUS st = gSys->RuntimeServices->SetVariable(
        L"OsIndications",
        &global_guid,
        attrs,
        sizeof(indications),
        &indications
    );
    if (EFI_ERROR(st)) {
        return;
    }

    if (gGop && gGop->Blt && gGop->Mode && gGop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
        gGop->Blt(gGop, &black, EfiBltVideoFill, 0, 0, 0, 0,
                 gGop->Mode->Info->HorizontalResolution,
                 gGop->Mode->Info->VerticalResolution, 0);
    }
    if (gCon) {
        gCon->SetAttribute(gCon, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
        gCon->ClearScreen(gCon);
        gCon->OutputString(gCon, L"[  OK  ] OsIndications set — rebooting into UEFI Firmware Setup...\r\n");
    }
    gBoot->Stall(250000U);
    gSys->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
}

static UserAction PollUserAction(void) {
    if (!gSys || !gSys->ConIn || !gSys->ConIn->ReadKeyStroke) {
        return ACT_NONE;
    }
    EFI_INPUT_KEY key;
    if (EFI_ERROR(gSys->ConIn->ReadKeyStroke(gSys->ConIn, &key))) {
        return ACT_NONE;
    }

    if (key.ScanCode == SCAN_F2 || key.ScanCode == SCAN_DELETE) {
        return ACT_REBOOT_BIOS;
    }
    if (key.ScanCode == SCAN_ESC ||
        key.UnicodeChar == L' '  ||
        key.UnicodeChar == L'\r' ||
        key.UnicodeChar == L'\n' ||
        key.UnicodeChar == L'q'  ||
        key.UnicodeChar == L'Q') {
        return ACT_SKIP_TO_OS;
    }
    if (key.UnicodeChar == L'\t') {
        return ACT_SKIP_INTRO;
    }
    if (key.UnicodeChar == L'p' || key.UnicodeChar == L'P') {
        return ACT_PAUSE_TOGGLE;
    }
    if (key.ScanCode == SCAN_LEFT) {
        return ACT_SEEK_BACK;
    }
    if (key.ScanCode == SCAN_RIGHT) {
        return ACT_SEEK_FWD;
    }
    if (key.UnicodeChar == L',' || key.UnicodeChar == L'<') {
        return ACT_STEP_BACK;
    }
    if (key.UnicodeChar == L'.' || key.UnicodeChar == L'>') {
        return ACT_STEP_FWD;
    }
    if (key.UnicodeChar == L'r' || key.UnicodeChar == L'R' || key.ScanCode == SCAN_HOME) {
        return ACT_REPLAY;
    }
    if (key.UnicodeChar == L's' || key.UnicodeChar == L'S') {
        return ACT_TOGGLE_SCANLINE;
    }
    return ACT_NONE;
}

static BOOLEAN StallWithInput(UINT32 total_us, BOOLEAN *skip_to_os, BOOLEAN *skip_intro) {
    UINT32 rem = total_us;
    while (rem > 0) {
        UserAction act = PollUserAction();
        if (act == ACT_REBOOT_BIOS) {
            TryRebootToFirmwareUI();
        } else if (act == ACT_SKIP_TO_OS) {
            if (skip_to_os) *skip_to_os = TRUE;
            return TRUE;
        } else if (act == ACT_SKIP_INTRO) {
            if (skip_intro) *skip_intro = TRUE;
            return TRUE;
        }
        UINT32 step = (rem > 10000U) ? 10000U : rem;
        gBoot->Stall(step);
        rem -= step;
    }
    return FALSE;
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
    EFI_GUID dp_guid  = EFI_DEVICE_PATH_PROTOCOL_GUID;
    gGop = NULL;
    UINT64  best_global_px = 0;
    BOOLEAN best_has_dp    = FALSE;

    EFI_HANDLE gop_handles[32];
    UINTN gop_bytes = sizeof(gop_handles);
    if (!EFI_ERROR(gBoot->LocateHandle(ByProtocol, &gop_guid, NULL, &gop_bytes, gop_handles))) {
        UINTN gop_count = gop_bytes / sizeof(EFI_HANDLE);
        for (UINTN h = 0; h < gop_count; ++h) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(gop_handles[h], &gop_guid, (VOID **)&gop)) || !gop || !gop->Mode) {
                continue;
            }
            VOID *dummy_dp = NULL;
            BOOLEAN has_dp = !EFI_ERROR(gBoot->HandleProtocol(gop_handles[h], &dp_guid, &dummy_dp)) && (dummy_dp != NULL);
            if (best_has_dp && !has_dp) {
                continue; /* Never let virtual ConSplitterDxe overwrite a physical GPU GOP handle */
            }

            /* Keep current firmware EDID mode if already >= 1920x1080 landscape; otherwise pick max landscape mode */
            UINT32 cur_w = gop->Mode->Info ? gop->Mode->Info->HorizontalResolution : 0;
            UINT32 cur_h = gop->Mode->Info ? gop->Mode->Info->VerticalResolution   : 0;
            BOOLEAN keep_edid = (cur_w >= 1920U && cur_h >= 1080U && cur_w > cur_h &&
                                 gop->Mode->Info->PixelFormat != PixelBltOnly);

            if (!keep_edid) {
                UINT32 best_mode = gop->Mode->Mode;
                UINT64 best_px = 0;
                for (UINT32 m = 0; m < gop->Mode->MaxMode; ++m) {
                    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
                    UINTN sz = 0;
                    if (!EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)) && info) {
                        UINT64 px = (UINT64)info->HorizontalResolution * info->VerticalResolution;
                        BOOLEAN landscape = (info->HorizontalResolution > info->VerticalResolution);
                        if (info->PixelFormat != PixelBltOnly && landscape && px >= best_px) {
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
            }

            if (gop->Blt && gop->Mode->Info) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
                gop->Blt(gop, &black, EfiBltVideoFill, 0, 0, 0, 0,
                         gop->Mode->Info->HorizontalResolution,
                         gop->Mode->Info->VerticalResolution, 0);
                UINT64 cur_px = (UINT64)gop->Mode->Info->HorizontalResolution *
                                gop->Mode->Info->VerticalResolution;
                if ((has_dp && !best_has_dp) || (has_dp == best_has_dp && cur_px > best_global_px)) {
                    best_global_px = cur_px;
                    best_has_dp    = has_dp;
                    gGop           = gop;
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
    EFI_GRAPHICS_OUTPUT_PROTOCOL *best_con_gop = NULL;
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
            EFI_GRAPHICS_OUTPUT_PROTOCOL *sto_gop = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(sto_handles[h], &gop_guid, (VOID **)&sto_gop)) || !sto_gop) {
                continue; /* Ignore serial/UART consoles that lack GOP */
            }
            VOID *sto_dp = NULL;
            BOOLEAN sto_has_dp = !EFI_ERROR(gBoot->HandleProtocol(sto_handles[h], &dp_guid, &sto_dp)) && (sto_dp != NULL);
            SIMPLE_TEXT_OUTPUT_INTERFACE *sto = NULL;
            if (EFI_ERROR(gBoot->HandleProtocol(sto_handles[h], &sto_guid, (VOID **)&sto)) ||
                !sto || !sto->Mode || !sto->QueryMode) {
                continue;
            }
            for (INT32 m = 0; m < sto->Mode->MaxMode; ++m) {
                UINTN cols = 0, rows = 0;
                if (!EFI_ERROR(sto->QueryMode(sto, (UINTN)m, &cols, &rows))) {
                    UINTN cells = cols * rows;
                    if ((cells > max_cells || (cells == max_cells && sto_has_dp && !best_con_gop)) &&
                        cols >= 80 && rows >= 25) {
                        max_cells    = cells;
                        best_mode    = (UINTN)m;
                        best_con     = sto;
                        if (sto_has_dp) {
                            best_con_gop = sto_gop;
                        }
                    }
                }
            }
        }
    }

    gCon = best_con;
    if (max_cells > 0 && gCon->SetMode) {
        gCon->SetMode(gCon, best_mode);
    }
    if (best_con_gop) {
        gGop = best_con_gop;
    }

    gCon->SetAttribute(gCon, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
    gCon->ClearScreen(gCon);
    gCon->EnableCursor(gCon, FALSE);
}

/*
 * Live Hardware Telemetry Probing (CPUID, TSC, UEFI SystemTable, SMBIOS/DMI, ACPI XSDT, e820, GOP)
 * Populates dynamic dmesg strings with real machine specifications; falls back safely on missing tables.
 */
typedef struct {
    CHAR16 e820_0[96];
    CHAR16 e820_1[96];
    CHAR16 e820_2[96];
    CHAR16 efi_line[120];
    CHAR16 smbios_line[96];
    CHAR16 dmi_line[128];
    CHAR16 tsc_line[96];
    CHAR16 rsdp_line[96];
    CHAR16 xsdt_line[96];
    CHAR16 fadt_line[96];
    CHAR16 mcfg_line[96];
    CHAR16 apic_line[96];
    CHAR16 iommu_subsys[16];
    CHAR16 iommu_tbl_line[96];
    CHAR16 iommu_init_line[96];
    CHAR16 zone_line[112];
    CHAR16 slub_line[96];
    CHAR16 rcu_line[96];
    CHAR16 smpboot_line[128];
    CHAR16 smp_line[96];
    CHAR16 efifb_line[128];
    CHAR16 cryptd_line[112];
    CHAR16 mem_summary_line[112];
} HardwareStrings;

static HardwareStrings gHwStr;

static const char *SmbiosGetString(const UINT8 *tbl, UINTN str_start, UINTN max_len, UINT8 idx) {
    if (idx == 0 || str_start >= max_len) return NULL;
    UINTN pos = str_start;
    UINT8 cur = 1;
    while (pos < max_len && tbl[pos] != 0) {
        if (cur == idx) {
            return (const char *)(tbl + pos);
        }
        while (pos < max_len && tbl[pos] != 0) ++pos;
        ++pos; /* skip NUL */
        ++cur;
    }
    return NULL;
}

static void ProbeHardwareForDmesg(void) {
    WBuf wb;

    /* 1. Calibrate TSC over 20 ms once for both dmesg MHz and 24 FPS video sync */
    UINT64 cal_t0 = ReadTsc64();
    gBoot->Stall(20000U);
    UINT64 ticks_20ms = ReadTsc64() - cal_t0;
    gTscPerUs = ticks_20ms / 20000ULL;
    if (gTscPerUs == 0) {
        gTscPerUs = 3000ULL;
        ticks_20ms = 60000000ULL;
    }
    UINT64 khz = ticks_20ms / 20ULL; /* ticks per millisecond = kHz */
    WBufInit(&wb, gHwStr.tsc_line, sizeof(gHwStr.tsc_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"Fast TSC calibration using UEFI Stall: ");
    WBufPutDec(&wb, khz / 1000ULL);
    WBufPutc(&wb, L'.');
    WBufPutDecPad(&wb, khz % 1000ULL, 3, L'0');
    WBufPuts(&wb, L" MHz");

    /* 2. CPUID: processor brand string, logical thread count, and crypto/SIMD flags */
    UINT32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    CpuIdEx(0, 0, &eax, &ebx, &ecx, &edx);
    UINT32 max_std = eax;
    BOOLEAN is_amd = (ebx == 0x68747541U); /* "Auth"enticAMD */

    UINT32 logical_cpus = 0;
    if (max_std >= 0x0BU) {
        UINT32 b0 = 0, b1 = 0, c1 = 0, d1 = 0, a1 = 0;
        CpuIdEx(0x0BU, 1U, &a1, &b1, &c1, &d1);
        logical_cpus = b1 & 0xFFFFU;
        if (logical_cpus == 0) {
            CpuIdEx(0x0BU, 0U, &a1, &b0, &c1, &d1);
            logical_cpus = b0 & 0xFFFFU;
        }
    }
    UINT32 feat_ecx1 = 0;
    if (max_std >= 1U) {
        CpuIdEx(1U, 0U, &eax, &ebx, &feat_ecx1, &edx);
        if (logical_cpus == 0) {
            logical_cpus = (ebx >> 16) & 0xFFU;
        }
    }
    if (logical_cpus == 0 || logical_cpus > 512U) {
        logical_cpus = 8U;
    }

    UINT32 feat_ebx7 = 0;
    if (max_std >= 7U) {
        CpuIdEx(7U, 0U, &eax, &feat_ebx7, &ecx, &edx);
    }

    char cpu_brand[49];
    memset(cpu_brand, 0, sizeof(cpu_brand));
    CpuIdEx(0x80000000U, 0U, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004U) {
        UINT32 *dst32 = (UINT32 *)cpu_brand;
        for (UINT32 leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
            CpuIdEx(leaf, 0U, &dst32[0], &dst32[1], &dst32[2], &dst32[3]);
            dst32 += 4;
        }
        cpu_brand[48] = 0;
    }

    WBufInit(&wb, gHwStr.slub_line, sizeof(gHwStr.slub_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"HWalign=64, Order=0-3, MinObjects=0, CPUs=");
    WBufPutDec(&wb, logical_cpus);
    WBufPuts(&wb, L", Nodes=1");

    WBufInit(&wb, gHwStr.rcu_line, sizeof(gHwStr.rcu_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"Hierarchical RCU implementation, restricting CPUs from NR_CPUS=512 to ");
    WBufPutDec(&wb, logical_cpus);

    WBufInit(&wb, gHwStr.smpboot_line, sizeof(gHwStr.smpboot_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"CPU0: ");
    if (cpu_brand[0]) {
        WBufPutAsciiTrim(&wb, cpu_brand, 48);
    } else {
        WBufPuts(&wb, is_amd ? L"AMD x86_64 Processor" : L"Intel x86_64 Processor");
    }

    WBufInit(&wb, gHwStr.smp_line, sizeof(gHwStr.smp_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"Brought up 1 node, ");
    WBufPutDec(&wb, logical_cpus);
    WBufPuts(&wb, L" CPUs — Total of ");
    WBufPutDec(&wb, logical_cpus);
    WBufPuts(&wb, L" processors activated");

    BOOLEAN has_aes    = (feat_ecx1 & (1U << 25)) != 0;
    BOOLEAN has_avx    = (feat_ecx1 & (1U << 28)) != 0;
    BOOLEAN has_avx2   = (feat_ebx7 & (1U << 5))  != 0;
    BOOLEAN has_avx512 = (feat_ebx7 & (1U << 16)) != 0;
    BOOLEAN has_shani  = (feat_ebx7 & (1U << 29)) != 0;

    WBufInit(&wb, gHwStr.cryptd_line, sizeof(gHwStr.cryptd_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"max_cpu_qlen set to 1000 (");
    BOOLEAN any_isa = FALSE;
    if (has_aes)    { WBufPuts(&wb, L"AES-NI"); any_isa = TRUE; }
    if (has_avx512) { WBufPuts(&wb, any_isa ? L", AVX-512" : L"AVX-512"); any_isa = TRUE; }
    else if (has_avx2) { WBufPuts(&wb, any_isa ? L", AVX2" : L"AVX2"); any_isa = TRUE; }
    else if (has_avx)  { WBufPuts(&wb, any_isa ? L", AVX" : L"AVX"); any_isa = TRUE; }
    if (has_shani)  { WBufPuts(&wb, any_isa ? L", SHA-NI" : L"SHA-NI"); any_isa = TRUE; }
    if (!any_isa)   { WBufPuts(&wb, L"SSE2"); }
    WBufPuts(&wb, L" hardware accel)");

    /* 3. Locate ACPI RSDP and SMBIOS entry points in EFI ConfigurationTable */
    EFI_GUID acpi20_guid  = EFI_ACPI_20_TABLE_GUID_CONST;
    EFI_GUID acpi10_guid  = EFI_ACPI_10_TABLE_GUID_CONST;
    EFI_GUID smbios3_guid = EFI_SMBIOS3_TABLE_GUID_CONST;
    EFI_GUID smbios2_guid = EFI_SMBIOS_TABLE_GUID_CONST;

    const UINT8 *rsdp_ptr    = NULL;
    const UINT8 *smbios3_ptr = NULL;
    const UINT8 *smbios2_ptr = NULL;

    if (gSys && gSys->ConfigurationTable) {
        for (UINTN i = 0; i < gSys->NumberOfTableEntries; ++i) {
            EFI_GUID *g = &gSys->ConfigurationTable[i].VendorGuid;
            VOID     *t = gSys->ConfigurationTable[i].VendorTable;
            if (!t) continue;
            if (GuidEq(g, &acpi20_guid)) {
                rsdp_ptr = (const UINT8 *)t;
            } else if (!rsdp_ptr && GuidEq(g, &acpi10_guid)) {
                rsdp_ptr = (const UINT8 *)t;
            } else if (GuidEq(g, &smbios3_guid)) {
                smbios3_ptr = (const UINT8 *)t;
            } else if (!smbios2_ptr && GuidEq(g, &smbios2_guid)) {
                smbios2_ptr = (const UINT8 *)t;
            }
        }
    }

    /* EFI vX.YY + FirmwareVendor + ACPI/SMBIOS pointers */
    UINT32 efi_rev = gSys ? gSys->Hdr.Revision : 0x00020046U;
    UINT32 efi_maj = efi_rev >> 16;
    UINT32 efi_min = efi_rev & 0xFFFFU;
    WBufInit(&wb, gHwStr.efi_line, sizeof(gHwStr.efi_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"EFI v");
    WBufPutDec(&wb, efi_maj);
    WBufPutc(&wb, L'.');
    if (efi_min % 10U == 0) {
        WBufPutDecPad(&wb, efi_min / 10U, 2, L'0');
    } else {
        WBufPutDec(&wb, efi_min);
    }
    WBufPuts(&wb, L" by ");
    if (gSys && gSys->FirmwareVendor && gSys->FirmwareVendor[0]) {
        for (UINTN k = 0; k < 32 && gSys->FirmwareVendor[k]; ++k) {
            WBufPutc(&wb, gSys->FirmwareVendor[k]);
        }
    } else {
        WBufPuts(&wb, L"UEFI Firmware");
    }
    if (rsdp_ptr) {
        WBufPuts(&wb, L": ACPI=0x");
        WBufPutHex(&wb, (UINT64)(UINTN)rsdp_ptr, 8, FALSE);
    }
    const UINT8 *smb_any = smbios3_ptr ? smbios3_ptr : smbios2_ptr;
    if (smb_any) {
        WBufPuts(&wb, L" SMBIOS=0x");
        WBufPutHex(&wb, (UINT64)(UINTN)smb_any, 8, FALSE);
    }

    /* 4. Parse SMBIOS version + DMI motherboard / system identity */
    UINT8  smb_maj = 3, smb_min = 6, smb_doc = 0;
    UINT64 smb_tbl_phys = 0;
    UINT32 smb_tbl_max  = 0;

    if (smbios3_ptr && BufferHasAscii(smbios3_ptr, 5, "_SM3_")) {
        smb_maj = smbios3_ptr[6];
        smb_min = smbios3_ptr[7];
        smb_doc = smbios3_ptr[8];
        memcpy(&smb_tbl_max,  smbios3_ptr + 12, sizeof(UINT32));
        memcpy(&smb_tbl_phys, smbios3_ptr + 16, sizeof(UINT64));
    } else if (smbios2_ptr && BufferHasAscii(smbios2_ptr, 4, "_SM_")) {
        smb_maj = smbios2_ptr[6];
        smb_min = smbios2_ptr[7];
        smb_doc = 0;
        UINT16 len16 = 0;
        UINT32 addr32 = 0;
        memcpy(&len16,  smbios2_ptr + 22, sizeof(UINT16));
        memcpy(&addr32, smbios2_ptr + 24, sizeof(UINT32));
        smb_tbl_max  = len16;
        smb_tbl_phys = addr32;
    }

    const char *bios_ver = NULL, *bios_date = NULL;
    const char *sys_mfg  = NULL, *sys_prod  = NULL;
    const char *brd_mfg  = NULL, *brd_prod  = NULL;
    UINTN smbios_structs = 0;
    UINTN smbios_bytes   = smb_tbl_max;

    if (smb_tbl_phys >= 0x1000ULL && smb_tbl_max >= 16U && smb_tbl_max <= 65536U) {
        const UINT8 *tbl = (const UINT8 *)(UINTN)smb_tbl_phys;
        UINTN off = 0;
        while (off + 4U <= smb_tbl_max && smbios_structs < 96U) {
            UINT8 type = tbl[off];
            UINT8 hlen = tbl[off + 1];
            if (hlen < 4U || off + hlen > smb_tbl_max) break;
            ++smbios_structs;

            UINTN str_base = off + hlen;
            if (type == 0 && hlen >= 9U) {
                bios_ver  = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 5]);
                bios_date = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 8]);
            } else if (type == 1 && hlen >= 6U) {
                sys_mfg  = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 4]);
                sys_prod = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 5]);
            } else if (type == 2 && hlen >= 6U) {
                brd_mfg  = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 4]);
                brd_prod = SmbiosGetString(tbl, str_base, smb_tbl_max, tbl[off + 5]);
            } else if (type == 127U) {
                UINTN p = str_base;
                while (p + 1U < smb_tbl_max && !(tbl[p] == 0 && tbl[p + 1] == 0)) ++p;
                smbios_bytes = (p + 2U <= smb_tbl_max) ? (p + 2U) : smb_tbl_max;
                break;
            }

            UINTN p = str_base;
            while (p + 1U < smb_tbl_max && !(tbl[p] == 0 && tbl[p + 1] == 0)) ++p;
            off = p + 2U;
        }
    }

    WBufInit(&wb, gHwStr.smbios_line, sizeof(gHwStr.smbios_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"SMBIOS ");
    WBufPutDec(&wb, smb_maj);
    WBufPutc(&wb, L'.');
    WBufPutDec(&wb, smb_min);
    if (smb_doc > 0) {
        WBufPutc(&wb, L'.');
        WBufPutDec(&wb, smb_doc);
    }
    WBufPuts(&wb, L" present, ");
    if (smbios_bytes > 0) {
        WBufPuts(&wb, L"structures occupy ");
        WBufPutDec(&wb, smbios_bytes);
        WBufPuts(&wb, L" bytes");
    } else {
        WBufPuts(&wb, L"DMI table active");
    }

    WBufInit(&wb, gHwStr.dmi_line, sizeof(gHwStr.dmi_line) / sizeof(CHAR16));
    const char *mfg_pick  = (brd_mfg && brd_mfg[0]) ? brd_mfg : sys_mfg;
    const char *prod_pick = (brd_prod && brd_prod[0]) ? brd_prod : sys_prod;
    if ((mfg_pick && mfg_pick[0]) || (prod_pick && prod_pick[0])) {
        if (mfg_pick && mfg_pick[0]) {
            WBufPutAsciiTrim(&wb, mfg_pick, 38);
            if (prod_pick && prod_pick[0]) WBufPutc(&wb, L' ');
        }
        if (prod_pick && prod_pick[0]) {
            WBufPutAsciiTrim(&wb, prod_pick, 40);
        }
        if (bios_ver && bios_ver[0]) {
            WBufPuts(&wb, L", BIOS ");
            WBufPutAsciiTrim(&wb, bios_ver, 24);
            if (bios_date && bios_date[0]) {
                WBufPutc(&wb, L' ');
                WBufPutAsciiTrim(&wb, bios_date, 16);
            }
        }
    } else {
        WBufPuts(&wb, L"UEFI x86_64 Standard PC Platform, BIOS ");
        if (gSys && gSys->FirmwareVendor) {
            for (UINTN k = 0; k < 24 && gSys->FirmwareVendor[k]; ++k) WBufPutc(&wb, gSys->FirmwareVendor[k]);
        } else {
            WBufPuts(&wb, L"EDK2");
        }
    }

    /* 5. Parse ACPI RSDP & XSDT/RSDT signatures and physical addresses */
    UINT64 rsdp_phys = (UINT64)(UINTN)rsdp_ptr;
    UINT8  rsdp_rev  = 2;
    UINT32 rsdp_len  = 0x24U;
    char   acpi_oem[6] = { 'A', 'L', 'A', 'S', 'K', 'A' };
    UINT64 xsdt_phys = 0;
    UINT32 xsdt_len  = 0xFCU;
    char   xsdt_oem_tbl[8] = { 'A', ' ', 'M', ' ', 'I', ' ', ' ', ' ' };

    UINT64 fadt_phys = 0, mcfg_phys = 0, apic_phys = 0, dmar_phys = 0, ivrs_phys = 0;
    UINT32 fadt_len = 0x114U, mcfg_len = 0x3CU, apic_len = 0x15CU, dmar_len = 0x88U, ivrs_len = 0xC0U;
    UINT8  fadt_rev = 6, mcfg_rev = 1, apic_rev = 5, dmar_rev = 1, ivrs_rev = 2;

    if (rsdp_ptr && BufferHasAscii(rsdp_ptr, 8, "RSD PTR ")) {
        memcpy(acpi_oem, rsdp_ptr + 9, 6);
        rsdp_rev = rsdp_ptr[15];
        UINT32 rsdt32 = 0;
        memcpy(&rsdt32, rsdp_ptr + 16, sizeof(UINT32));
        if (rsdp_rev >= 2U) {
            memcpy(&rsdp_len,  rsdp_ptr + 20, sizeof(UINT32));
            memcpy(&xsdt_phys, rsdp_ptr + 24, sizeof(UINT64));
        } else {
            rsdp_len = 0x14U;
        }

        const UINT8 *root_sdt = NULL;
        BOOLEAN is_64_ptrs = FALSE;
        if (xsdt_phys >= 0x1000ULL && BufferHasAscii((const UINT8 *)(UINTN)xsdt_phys, 4, "XSDT")) {
            root_sdt   = (const UINT8 *)(UINTN)xsdt_phys;
            is_64_ptrs = TRUE;
        } else if (rsdt32 >= 0x1000U && BufferHasAscii((const UINT8 *)(UINTN)rsdt32, 4, "RSDT")) {
            root_sdt   = (const UINT8 *)(UINTN)rsdt32;
            xsdt_phys  = rsdt32;
            is_64_ptrs = FALSE;
        }

        if (root_sdt) {
            memcpy(&xsdt_len, root_sdt + 4, sizeof(UINT32));
            memcpy(xsdt_oem_tbl, root_sdt + 16, 8);
            if (xsdt_len >= 36U && xsdt_len <= 16384U) {
                UINTN step = is_64_ptrs ? 8U : 4U;
                UINTN n_ent = (xsdt_len - 36U) / step;
                if (n_ent > 64U) n_ent = 64U;
                for (UINTN e = 0; e < n_ent; ++e) {
                    UINT64 entry_addr = 0;
                    if (is_64_ptrs) {
                        memcpy(&entry_addr, root_sdt + 36U + e * 8U, sizeof(UINT64));
                    } else {
                        UINT32 e32 = 0;
                        memcpy(&e32, root_sdt + 36U + e * 4U, sizeof(UINT32));
                        entry_addr = e32;
                    }
                    if (entry_addr < 0x1000ULL) continue;
                    const UINT8 *hdr = (const UINT8 *)(UINTN)entry_addr;
                    UINT32 tlen = 0;
                    memcpy(&tlen, hdr + 4, sizeof(UINT32));
                    UINT8 trev = hdr[8];
                    if (BufferHasAscii(hdr, 4, "FACP")) {
                        fadt_phys = entry_addr; fadt_len = tlen; fadt_rev = trev;
                    } else if (BufferHasAscii(hdr, 4, "MCFG")) {
                        mcfg_phys = entry_addr; mcfg_len = tlen; mcfg_rev = trev;
                    } else if (BufferHasAscii(hdr, 4, "APIC")) {
                        apic_phys = entry_addr; apic_len = tlen; apic_rev = trev;
                    } else if (BufferHasAscii(hdr, 4, "DMAR")) {
                        dmar_phys = entry_addr; dmar_len = tlen; dmar_rev = trev;
                    } else if (BufferHasAscii(hdr, 4, "IVRS")) {
                        ivrs_phys = entry_addr; ivrs_len = tlen; ivrs_rev = trev;
                    }
                }
            }
        }
    }

    if (rsdp_phys == 0) rsdp_phys = 0x7A9F2014ULL;
    if (xsdt_phys == 0) xsdt_phys = (rsdp_phys & ~0xFFFULL) - 0x1000ULL;
    if (fadt_phys == 0) fadt_phys = xsdt_phys - 0x8000ULL;
    if (mcfg_phys == 0) mcfg_phys = xsdt_phys - 0x4000ULL;
    if (apic_phys == 0) apic_phys = xsdt_phys - 0x5000ULL;

    WBufInit(&wb, gHwStr.rsdp_line, sizeof(gHwStr.rsdp_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"RSDP 0x");
    WBufPutHex(&wb, rsdp_phys, 16, TRUE);
    WBufPutc(&wb, L' ');
    WBufPutHex(&wb, rsdp_len, 6, TRUE);
    WBufPuts(&wb, L" (v");
    WBufPutDecPad(&wb, rsdp_rev, 2, L'0');
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, acpi_oem, 6);
    WBufPutc(&wb, L')');

    WBufInit(&wb, gHwStr.xsdt_line, sizeof(gHwStr.xsdt_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"XSDT 0x");
    WBufPutHex(&wb, xsdt_phys, 16, TRUE);
    WBufPutc(&wb, L' ');
    WBufPutHex(&wb, xsdt_len, 6, TRUE);
    WBufPuts(&wb, L" (v01 ");
    WBufPutAsciiN(&wb, acpi_oem, 6);
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, xsdt_oem_tbl, 8);
    WBufPutc(&wb, L')');

    WBufInit(&wb, gHwStr.fadt_line, sizeof(gHwStr.fadt_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"FADT 0x");
    WBufPutHex(&wb, fadt_phys, 16, TRUE);
    WBufPutc(&wb, L' ');
    WBufPutHex(&wb, fadt_len, 6, TRUE);
    WBufPuts(&wb, L" (v");
    WBufPutDecPad(&wb, fadt_rev, 2, L'0');
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, acpi_oem, 6);
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, xsdt_oem_tbl, 8);
    WBufPutc(&wb, L')');

    WBufInit(&wb, gHwStr.mcfg_line, sizeof(gHwStr.mcfg_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"MCFG 0x");
    WBufPutHex(&wb, mcfg_phys, 16, TRUE);
    WBufPutc(&wb, L' ');
    WBufPutHex(&wb, mcfg_len, 6, TRUE);
    WBufPuts(&wb, L" (v");
    WBufPutDecPad(&wb, mcfg_rev, 2, L'0');
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, acpi_oem, 6);
    WBufPutc(&wb, L')');

    WBufInit(&wb, gHwStr.apic_line, sizeof(gHwStr.apic_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"APIC 0x");
    WBufPutHex(&wb, apic_phys, 16, TRUE);
    WBufPutc(&wb, L' ');
    WBufPutHex(&wb, apic_len, 6, TRUE);
    WBufPuts(&wb, L" (v");
    WBufPutDecPad(&wb, apic_rev, 2, L'0');
    WBufPutc(&wb, L' ');
    WBufPutAsciiN(&wb, acpi_oem, 6);
    WBufPutc(&wb, L')');

    BOOLEAN use_amd_vi = (ivrs_phys != 0) || (dmar_phys == 0 && is_amd);
    WBufInit(&wb, gHwStr.iommu_subsys, sizeof(gHwStr.iommu_subsys) / sizeof(CHAR16));
    WBufPuts(&wb, use_amd_vi ? L"AMD-Vi" : L"DMAR");

    WBufInit(&wb, gHwStr.iommu_tbl_line, sizeof(gHwStr.iommu_tbl_line) / sizeof(CHAR16));
    if (use_amd_vi) {
        UINT64 addr = ivrs_phys ? ivrs_phys : (xsdt_phys - 0x2000ULL);
        WBufPuts(&wb, L"IVRS 0x");
        WBufPutHex(&wb, addr, 16, TRUE);
        WBufPutc(&wb, L' ');
        WBufPutHex(&wb, ivrs_len, 6, TRUE);
        WBufPuts(&wb, L" (v");
        WBufPutDecPad(&wb, ivrs_rev, 2, L'0');
        WBufPuts(&wb, L" AMD    IOMMU)");
    } else {
        UINT64 addr = dmar_phys ? dmar_phys : (xsdt_phys - 0x2000ULL);
        WBufPuts(&wb, L"DMAR 0x");
        WBufPutHex(&wb, addr, 16, TRUE);
        WBufPutc(&wb, L' ');
        WBufPutHex(&wb, dmar_len, 6, TRUE);
        WBufPuts(&wb, L" (v");
        WBufPutDecPad(&wb, dmar_rev, 2, L'0');
        WBufPuts(&wb, L" INTEL  VT-d)");
    }

    WBufInit(&wb, gHwStr.iommu_init_line, sizeof(gHwStr.iommu_init_line) / sizeof(CHAR16));
    if (use_amd_vi) {
        WBufPuts(&wb, L"IOMMUv2 enabled: Interrupt remapping & x2APIC support active");
    } else {
        WBufPuts(&wb, L"IOMMU enabled: Queued invalidation & IRQ remapping active");
    }

    /* 6. Real UEFI GetMemoryMap -> BIOS-e820 usable ranges & total physical RAM */
    UINT64 r_start[3] = { 0x00000000ULL, 0x00100000ULL, 0x100000000ULL };
    UINT64 r_end[3]   = { 0x0009FFFFULL, 0x7A8DEFFFULL, 0x47FFFFFFFULL };
    UINTN  r_found    = 0;
    UINT64 total_pages  = 0;
    UINT64 usable_pages = 0;
    UINT64 highest_end  = 0x47FFFFFFFULL;

    if (gBoot && gBoot->GetMemoryMap) {
        UINTN  map_size = 0, map_key = 0, desc_size = 0;
        UINT32 desc_ver = 0;
        EFI_STATUS mst = gBoot->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
        if (mst == EFI_BUFFER_TOO_SMALL && map_size > 0 && desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
            map_size += desc_size * 8U;
            UINT8 *map_buf = NULL;
            if (!EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, map_size, (VOID **)&map_buf)) && map_buf) {
                if (!EFI_ERROR(gBoot->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf, &map_key, &desc_size, &desc_ver))) {
                    UINTN count = map_size / desc_size;
                    UINT64 cur_s = 0, cur_e = 0;
                    BOOLEAN in_run = FALSE;

                    for (UINTN i = 0; i < count; ++i) {
                        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(map_buf + i * desc_size);
                        if (d->NumberOfPages == 0) continue;
                        UINT64 seg_s = d->PhysicalStart;
                        UINT64 seg_e = seg_s + d->NumberOfPages * 4096ULL - 1ULL;

                        BOOLEAN is_ram = (d->Type == EfiLoaderCode ||
                                          d->Type == EfiLoaderData ||
                                          d->Type == EfiBootServicesCode ||
                                          d->Type == EfiBootServicesData ||
                                          d->Type == EfiConventionalMemory ||
                                          d->Type == EfiRuntimeServicesCode ||
                                          d->Type == EfiRuntimeServicesData ||
                                          d->Type == EfiACPIReclaimMemory ||
                                          d->Type == EfiACPIMemoryNVS);
                        BOOLEAN is_os_usable = (d->Type == EfiLoaderCode ||
                                                d->Type == EfiLoaderData ||
                                                d->Type == EfiBootServicesCode ||
                                                d->Type == EfiBootServicesData ||
                                                d->Type == EfiConventionalMemory);
                        if (is_ram) {
                            total_pages += d->NumberOfPages;
                        }
                        if (is_os_usable) {
                            usable_pages += d->NumberOfPages;
                            if (seg_e > highest_end) highest_end = seg_e;
                            if (in_run && seg_s == cur_e + 1ULL) {
                                cur_e = seg_e;
                            } else {
                                if (in_run && r_found < 3U && (cur_e - cur_s) >= 0x40000ULL) {
                                    r_start[r_found] = cur_s;
                                    r_end[r_found]   = cur_e;
                                    ++r_found;
                                }
                                cur_s  = seg_s;
                                cur_e  = seg_e;
                                in_run = TRUE;
                            }
                        } else if (in_run) {
                            if (r_found < 3U && (cur_e - cur_s) >= 0x40000ULL) {
                                r_start[r_found] = cur_s;
                                r_end[r_found]   = cur_e;
                                ++r_found;
                            }
                            in_run = FALSE;
                        }
                    }
                    if (in_run && r_found < 3U && (cur_e - cur_s) >= 0x40000ULL) {
                        r_start[r_found] = cur_s;
                        r_end[r_found]   = cur_e;
                        ++r_found;
                    }
                }
                gBoot->FreePool(map_buf);
            }
        }
    }

    CHAR16 *e820_slots[3] = { gHwStr.e820_0, gHwStr.e820_1, gHwStr.e820_2 };
    for (UINTN k = 0; k < 3U; ++k) {
        WBufInit(&wb, e820_slots[k], 96);
        WBufPuts(&wb, L"[mem 0x");
        WBufPutHex(&wb, r_start[k], 16, FALSE);
        WBufPuts(&wb, L"-0x");
        WBufPutHex(&wb, r_end[k], 16, FALSE);
        WBufPuts(&wb, L"] usable");
    }

    WBufInit(&wb, gHwStr.zone_line, sizeof(gHwStr.zone_line) / sizeof(CHAR16));
    WBufPuts(&wb, L"DMA32 [mem 0x00001000-0xffffffff], Normal [mem 0x100000000-0x");
    WBufPutHex(&wb, highest_end > 0x100000000ULL ? highest_end : 0x47FFFFFFFULL, 1, FALSE);
    WBufPutc(&wb, L']');

    if (total_pages == 0) {
        total_pages  = 16ULL * 1024ULL * 256ULL; /* 16 GB fallback */
        usable_pages = total_pages - 65536ULL;
    }
    UINT64 usable_kb = usable_pages * 4ULL;
    UINT64 total_kb  = total_pages  * 4ULL;
    UINT64 total_mb  = (total_kb + 512ULL) / 1024ULL;
    WBufInit(&wb, gHwStr.mem_summary_line, sizeof(gHwStr.mem_summary_line) / sizeof(CHAR16));
    WBufPutDec(&wb, usable_kb);
    WBufPuts(&wb, L"K/");
    WBufPutDec(&wb, total_kb);
    WBufPuts(&wb, L"K available (");
    WBufPutDec(&wb, total_mb / 1024ULL);
    WBufPutc(&wb, L'.');
    WBufPutDec(&wb, ((total_mb % 1024ULL) * 10ULL) / 1024ULL);
    WBufPuts(&wb, L" GiB physical RAM)");

    /* 7. Real GOP Framebuffer geometry & physical BAR (efifb) */
    WBufInit(&wb, gHwStr.efifb_line, sizeof(gHwStr.efifb_line) / sizeof(CHAR16));
    if (gGop && gGop->Mode && gGop->Mode->Info) {
        UINT64 fb_base = (UINT64)gGop->Mode->FrameBufferBase;
        UINT64 fb_kb   = (UINT64)(gGop->Mode->FrameBufferSize / 1024U);
        UINT32 w       = gGop->Mode->Info->HorizontalResolution;
        UINT32 h       = gGop->Mode->Info->VerticalResolution;
        UINT32 stride  = gGop->Mode->Info->PixelsPerScanLine * 4U;
        WBufPuts(&wb, L"framebuffer at 0x");
        WBufPutHex(&wb, fb_base, 8, FALSE);
        WBufPuts(&wb, L", using ");
        WBufPutDec(&wb, fb_kb > 0 ? fb_kb : ((UINT64)w * h * 4ULL / 1024ULL));
        WBufPuts(&wb, L"k (");
        WBufPutDec(&wb, w);
        WBufPutc(&wb, L'x');
        WBufPutDec(&wb, h);
        WBufPuts(&wb, L"x32, stride=");
        WBufPutDec(&wb, stride);
        WBufPutc(&wb, L')');
    } else {
        WBufPuts(&wb, L"framebuffer active (VESA/GOP linear 32bpp mode)");
    }
}

/*
 * Stage 1: Rapid kernel dmesg burst (56 dense lines, ~1.2s total, populated with live hardware data)
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
    { L"    0.000104", L"BIOS-e820",                   gHwStr.e820_0, 0 },
    { L"    0.000118", L"BIOS-e820",                   gHwStr.e820_1, 0 },
    { L"    0.000131", L"BIOS-e820",                   gHwStr.e820_2, 0 },
    { L"    0.000412", L"NX (Execute Disable)",        L"protection: active", 0 },
    { L"    0.000689", L"efi",                         gHwStr.efi_line, 0 },
    { L"    0.001205", L"SMBIOS",                      gHwStr.smbios_line, 0 },
    { L"    0.001540", L"DMI",                         gHwStr.dmi_line, 0 },
    { L"    0.002018", L"tsc",                         gHwStr.tsc_line, 15 },
    { L"    0.003491", L"MTRR",                        L"default type: uncachable, fixed ranges enabled, variable ranges 10", 0 },
    { L"    0.004128", L"ACPI",                        L"Core revision 20240827", 0 },
    { L"    0.004510", L"ACPI",                        gHwStr.rsdp_line, 0 },
    { L"    0.004892", L"ACPI",                        gHwStr.xsdt_line, 0 },
    { L"    0.005314", L"ACPI",                        gHwStr.fadt_line, 0 },
    { L"    0.006040", L"ACPI",                        gHwStr.mcfg_line, 0 },
    { L"    0.006419", L"ACPI",                        gHwStr.apic_line, 0 },
    { L"    0.006875", L"ACPI",                        gHwStr.iommu_tbl_line, 0 },
    { L"    0.007912", L"Zone ranges",                 gHwStr.zone_line, 0 },
    { L"    0.008640", L"Memory",                      gHwStr.mem_summary_line, 0 },
    { L"    0.009480", L"SLUB",                        gHwStr.slub_line, 0 },
    { L"    0.010824", L"rcu",                         gHwStr.rcu_line, 0 },
    { L"    0.012309", L"APIC",                        L"Switched APIC routing to physical x2apic", 15 },
    { L"    0.014112", L"IOAPIC[0]",                   L"apic_id 2, version 32, address 0xfec00000, GSI 0-119", 0 },
    { L"    0.016044", L"smpboot",                     gHwStr.smpboot_line, 25 },
    { L"    0.017508", L"smp",                         gHwStr.smp_line, 20 },
    { L"    0.018203", L"devtmpfs",                    L"initialized", 0 },
    { L"    0.018940", L"pci 0000:00:01.0",            L"PME# supported from D0 D3hot D3cold", 0 },
    { L"    0.019612", L"ACPI",                        L"PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])", 20 },
    { L"    0.020488", L"pci 0000:00:00.0",            L"type 00 class 0x060000 Host bridge", 0 },
    { L"    0.021190", L"pci 0000:00:01.0",            L"PCI bridge to [bus 01] (PCIe Root Port x16)", 0 },
    { L"    0.022045", L"pci 0000:00:06.0",            L"PCI bridge to [bus 02] (PCIe Gen4 Root Port x4)", 0 },
    { L"    0.022871", L"pci 0000:00:14.0",            L"type 00 class 0x0c0330 USB xHCI Controller", 0 },
    { L"    0.024904", L"efifb",                       gHwStr.efifb_line, 25 },
    { L"    0.026318", gHwStr.iommu_subsys,            gHwStr.iommu_init_line, 15 },
    { L"    0.028102", L"PCI",                         L"Using configuration type 1 for base access & MMCONFIG [bus 00-ff]", 0 },
    { L"    0.030441", L"clocksource",                 L"Switched to clocksource tsc-early (mask: 0xffffffffffffffff)", 0 },
    { L"    0.032119", L"pnp",                         L"PnP ACPI init — system board resources parsed", 0 },
    { L"    0.034890", L"NET",                         L"Registered PF_INET, PF_INET6, PF_NETLINK, PF_UNIX protocol families", 0 },
    { L"    0.036712", L"PCI-DMA",                     L"Using software IO TLB [mem 0x74800000-0x78800000] (64MB)", 0 },
    { L"    0.038504", L"workingset",                  L"timestamp_bits=36 max_order=22 bucket_order=0", 0 },
    { L"    0.040192", L"cryptd",                      gHwStr.cryptd_line, 0 },
    { L"    0.042110", L"nvme nvme0",                  L"pci function 0000:02:00.0 (MSI-X: 17 vectors allocated)", 35 },
    { L"    0.045820", L"nvme nvme0",                  L"16/0/0 default/read/poll queues, NVMe 1.4 Namespace 1 ready", 25 },
    { L"    0.047302", L" nvme0n1",                    L"p1 p2 p3 p4", 15 },
    { L"    0.049811", L"xhci_hcd 0000:00:14.0",       L"xHCI Host Controller — new USB bus registered, assigned bus number 1", 25 },
    { L"    0.051290", L"xhci_hcd 0000:00:14.0",       L"Host supports USB 3.2 Enhanced SuperSpeed (20 Gbps)", 0 },
    { L"    0.053408", L"usb 1-2",                     L"new full-speed USB device number 2 using xhci_hcd", 20 },
    { L"    0.055114", L"input",                       L"USB HID Keyboard as /devices/pci0000:00/0000:00:14.0/usb1/1-2/input0", 0 },
    { L"    0.056890", L"usb 1-4",                     L"new full-speed USB device number 3 using xhci_hcd (HID Optical Mouse)", 15 },
    { L"    0.058920", L"tpm_tis MSFT0101:00",         L"2.0 TPM (device-id 0x1B, rev-id 22) CRB interface active", 20 },
    { L"    0.061304", L"efivars",                     L"Registered efivars operations on EFI Runtime Services", 0 },
    { L"    0.064118", L"EXT4-fs (nvme0n1p4)",         L"mounted filesystem a8f1c902-441b-4d0e-9a77 ordered data mode", 35 },
    { L"    0.066205", L"VFS",                         L"Mounted root (ext4 filesystem) readonly on device 259:4", 0 },
    { L"    0.068012", L"Freeing unused kernel image", L"(initmem) memory: 3148K", 0 },
    { L"    0.069440", L"Write protecting",            L"kernel read-only data: 26624k", 0 },
    { L"    0.071880", L"systemd[1]",                  L"systemd 256.8 running in system mode (+PAM +AUDIT +SECCOMP +OPENSSL +TPM2 +ZSTD)", 30 },
};

static void EmitDmesgStage(BOOLEAN *skip_to_os, BOOLEAN *skip_intro) {
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
        if (StallWithInput(delay, skip_to_os, skip_intro)) {
            return;
        }
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

static void EmitSystemdStage(BOOLEAN *skip_to_os, BOOLEAN *skip_intro) {
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

        UINT32 wait_us = 0;
        switch (r->pace) {
            case PACE_BURST:
                wait_us = RdtscJitterUs(SYSD_BURST_MIN, SYSD_BURST_MAX);
                break;
            case PACE_NORM:
                wait_us = RdtscJitterUs(SYSD_NORM_MIN, SYSD_NORM_MAX);
                break;
            case PACE_HITCH:
                wait_us = RdtscJitterUs(SYSD_NORM_MIN, SYSD_NORM_MAX) +
                          RdtscJitterUs(SYSD_HITCH_MIN, SYSD_HITCH_MAX);
                break;
            case PACE_FINAL:
                break;
        }
        if (wait_us > 0 && StallWithInput(wait_us, skip_to_os, skip_intro)) {
            return;
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

/* Apply a single frame's Temporal-RLE stream into the compact 1-byte/cell index grid */
static void ApplyFrameTokensToCells(UINT32 f, UINT8 *cells, UINTN total_cells) {
    UINT32 run_idx = kGopFrameOffsets[f];
    UINT32 run_end = kGopFrameOffsets[f + 1U];
    UINTN  pos = 0;
    while (run_idx < run_end && pos < total_cells) {
        UINT16 token     = kGopVideoRunsU16[run_idx++];
        UINT8  color_idx = (UINT8)(token & 0xFFU);
        UINTN  rl        = (UINTN)(token >> 8);
        if (pos + rl > total_cells) {
            rl = total_cells - pos;
        }
        if (color_idx < GOP_VIDEO_SKIP_INDEX) {
            memset(cells + pos, color_idx, rl);
        }
        pos += rl;
    }
}

/* Rasterize the full cell index grid into the scaled GOP pixel buffer and blit once */
static void RenderFullCellsToVideo(
    const UINT8  *cells,
    UINT32       *blt_u32,
    UINTN         render_w,
    UINTN         render_h,
    UINTN         dst_x,
    UINTN         dst_y,
    UINTN         scale,
    BOOLEAN       draw_scanline,
    const UINT32 *edge_palette32
) {
    for (UINTN gy = 0; gy < GOP_VIDEO_HEIGHT; ++gy) {
        UINTN px_y0 = gy * scale;
        if (px_y0 >= render_h) break;
        UINTN px_h = scale;
        if (px_y0 + px_h > render_h) px_h = render_h - px_y0;
        UINTN body_rows = (draw_scanline && px_h == scale) ? (px_h - 1U) : px_h;

        const UINT8 *row_cells = cells + gy * GOP_VIDEO_WIDTH;
        UINTN gx = 0;
        while (gx < GOP_VIDEO_WIDTH) {
            UINT8 cidx = row_cells[gx];
            UINTN span = 1U;
            while (gx + span < GOP_VIDEO_WIDTH && row_cells[gx + span] == cidx) {
                ++span;
            }
            if (cidx >= 255U) cidx = 0U;
            UINTN px_x0 = gx * scale;
            UINTN px_w  = span * scale;
            if (px_x0 < render_w) {
                if (px_x0 + px_w > render_w) px_w = render_w - px_x0;
                UINT32 main32 = kGopVideoPalette32[cidx];
                UINT32 edge32 = edge_palette32[cidx];
                UINT32 *row_ptr = blt_u32 + px_y0 * render_w + px_x0;
                for (UINTN sy = 0; sy < body_rows; ++sy) {
                    FillPixels32(row_ptr, px_w, main32);
                    row_ptr += render_w;
                }
                if (body_rows < px_h) {
                    FillPixels32(row_ptr, px_w, edge32);
                }
            }
            gx += span;
        }
    }

    gGop->Blt(
        gGop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)blt_u32,
        EfiBltBufferToVideo,
        0, 0,
        dst_x, dst_y,
        render_w, render_h,
        render_w * sizeof(UINT32)
    );
}

static void SeekCellsAndBlit(
    UINT32        cur_f,
    UINT32        target_f,
    UINT8        *cells,
    UINTN         total_cells,
    UINT32       *blt_u32,
    UINTN         render_w,
    UINTN         render_h,
    UINTN         dst_x,
    UINTN         dst_y,
    UINTN         scale,
    BOOLEAN       draw_scanline,
    const UINT32 *edge_palette32
) {
    if (target_f <= cur_f || target_f == 0U) {
        memset(cells, 0, total_cells);
        for (UINT32 k = 0; k <= target_f; ++k) {
            ApplyFrameTokensToCells(k, cells, total_cells);
        }
    } else {
        for (UINT32 k = cur_f + 1U; k <= target_f; ++k) {
            ApplyFrameTokensToCells(k, cells, total_cells);
        }
    }
    RenderFullCellsToVideo(cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
}

/*
 * Direct 24-bit TrueColor GOP->Blt() HD block video player (320x180, 255 RGB colors + temporal skip):
 * - Exact full-screen integer scaling on 16:9 displays (6x = 1920x1080, 8x = 2560x1440, 12x = 3840x2160)
 * - Uses 32-bit/64-bit word stores into offscreen UINT32 pixel buffer (zero byte-by-byte overhead)
 * - Blits only the active dirty scanline band in a single tear-free EfiBltBufferToVideo call per frame
 * - Smart hotkeys: Space/Enter/Esc/Q skip, P pause, Left/Right +-2s seek, ,/. +-1 frame step,
 *   R/Home replay, S scanlines toggle, F2/Del reboot straight to UEFI BIOS Setup
 */
static void PlayGopVideo(void) {
    if (!gGop || !gGop->Blt || !gGop->Mode || !gGop->Mode->Info || GOP_VIDEO_FRAME_COUNT == 0U) {
        return;
    }

    UINTN screen_w = gGop->Mode->Info->HorizontalResolution;
    UINTN screen_h = gGop->Mode->Info->VerticalResolution;
    if (screen_w == 0 || screen_h == 0) {
        return;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL black = { 0, 0, 0, 0 };
    gGop->Blt(gGop, &black, EfiBltVideoFill, 0, 0, 0, 0, screen_w, screen_h, 0);
    if (gCon && gCon->EnableCursor) {
        gCon->EnableCursor(gCon, FALSE);
    }

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

    const UINTN total_cells = (UINTN)GOP_VIDEO_WIDTH * (UINTN)GOP_VIDEO_HEIGHT;
    UINTN buf_bytes = render_w * render_h * sizeof(UINT32);
    UINT32 *blt_u32  = NULL;
    UINT8  *cell_idx = NULL;
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, buf_bytes, (VOID **)&blt_u32)) || !blt_u32) {
        return;
    }
    if (EFI_ERROR(gBoot->AllocatePool(EfiBootServicesData, total_cells, (VOID **)&cell_idx)) || !cell_idx) {
        gBoot->FreePool(blt_u32);
        return;
    }
    gBoot->SetMem(blt_u32, buf_bytes, 0);
    gBoot->SetMem(cell_idx, total_cells, 0);

    /* Precompute darkened scanline palette once (7/8 brightness in 32-bit BGR0) */
    UINT32 edge_palette32[255];
    for (UINTN i = 0; i < 255U; ++i) {
        UINT32 c = kGopVideoPalette32[i];
        UINT32 b = (((c >>  0) & 0xFFU) * 7U) >> 3;
        UINT32 g = (((c >>  8) & 0xFFU) * 7U) >> 3;
        UINT32 r = (((c >> 16) & 0xFFU) * 7U) >> 3;
        edge_palette32[i] = b | (g << 8) | (r << 16);
    }

    UINT64 tsc_per_us = gTscPerUs ? gTscPerUs : 3000ULL;
    BOOLEAN draw_scanline = (scale >= 4U);
    BOOLEAN paused = FALSE;
    const UINT32 last_frame = GOP_VIDEO_FRAME_COUNT - 1U;
    const UINT32 jump_frames = (GOP_VIDEO_FPS * 2U > 0U) ? (GOP_VIDEO_FPS * 2U) : 24U;

    for (UINT32 f = 0; f < GOP_VIDEO_FRAME_COUNT; ++f) {
        UINT64 frame_t0 = ReadTsc64();

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

            memset(cell_idx + cell_pos, color_idx, rl);
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

        /* Process input & interactive pause/seek loop */
        BOOLEAN exit_player = FALSE;
        for (;;) {
            UserAction act = PollUserAction();
            if (act == ACT_SKIP_TO_OS) {
                exit_player = TRUE;
                break;
            } else if (act == ACT_REBOOT_BIOS) {
                TryRebootToFirmwareUI();
            } else if (act == ACT_PAUSE_TOGGLE) {
                paused = !paused;
                frame_t0 = ReadTsc64();
            } else if (act == ACT_TOGGLE_SCANLINE && scale >= 2U) {
                draw_scanline = !draw_scanline;
                RenderFullCellsToVideo(cell_idx, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                frame_t0 = ReadTsc64();
            } else if (act == ACT_REPLAY) {
                SeekCellsAndBlit(f, 0U, cell_idx, total_cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                f = 0U;
                frame_t0 = ReadTsc64();
            } else if (act == ACT_SEEK_BACK) {
                UINT32 tgt = (f > jump_frames) ? (f - jump_frames) : 0U;
                SeekCellsAndBlit(f, tgt, cell_idx, total_cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                f = tgt;
                frame_t0 = ReadTsc64();
            } else if (act == ACT_SEEK_FWD) {
                UINT32 tgt = (f + jump_frames < last_frame) ? (f + jump_frames) : last_frame;
                SeekCellsAndBlit(f, tgt, cell_idx, total_cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                f = tgt;
                frame_t0 = ReadTsc64();
            } else if (act == ACT_STEP_BACK) {
                paused = TRUE;
                UINT32 tgt = (f > 0U) ? (f - 1U) : 0U;
                SeekCellsAndBlit(f, tgt, cell_idx, total_cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                f = tgt;
            } else if (act == ACT_STEP_FWD) {
                paused = TRUE;
                UINT32 tgt = (f < last_frame) ? (f + 1U) : last_frame;
                SeekCellsAndBlit(f, tgt, cell_idx, total_cells, blt_u32, render_w, render_h, dst_x, dst_y, scale, draw_scanline, edge_palette32);
                f = tgt;
            }

            if (paused) {
                gBoot->Stall(15000U);
                continue;
            }

            UINT64 elapsed_us = (ReadTsc64() - frame_t0) / tsc_per_us;
            if (elapsed_us >= GOP_VIDEO_FRAME_US) {
                break;
            }
            UINT64 rem_us = GOP_VIDEO_FRAME_US - elapsed_us;
            gBoot->Stall((UINTN)((rem_us > 8000ULL) ? 8000ULL : rem_us));
        }

        if (exit_player) {
            break;
        }
    }

    gBoot->FreePool(cell_idx);
    gBoot->FreePool(blt_u32);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    gSys  = system_table;
    gBoot = system_table->BootServices;

    PrepareMaxResolutionConsole();
    DrainBufferedKeys();
    ProbeHardwareForDmesg();

    BOOLEAN skip_to_os = FALSE;
    BOOLEAN skip_intro = FALSE;
#if ENABLE_LINUX_INTRO
    EmitDmesgStage(&skip_to_os, &skip_intro);
    if (!skip_to_os && !skip_intro) {
        EmitSystemdStage(&skip_to_os, &skip_intro);
    }
#endif
    if (!skip_to_os) {
        PlayGopVideo();
    }

    EFI_STATUS st = ChainloadNextLoader(image_handle);
    if (EFI_ERROR(st) && gCon) {
        gCon->SetAttribute(gCon, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
        gCon->OutputString(gCon, L"[FAILED] Unable to start bootmgfw.efi\r\n");
        gBoot->Stall(5000000U);
    }
    return st;
}
