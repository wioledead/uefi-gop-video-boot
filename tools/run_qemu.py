#!/usr/bin/env python3
"""
Build a bootable MBR + FAT32 EFI System Partition image (efi/esp_qemu.img) in pure Python
and launch efi/bootx64.efi inside QEMU + OVMF at 1920x1080.

Why build a real FAT32 image instead of QEMU's virtual `fat:rw:...` driver?
  QEMU's vvfat driver emulates a small FAT16 CHS disk and frequently fails or asserts
  on Windows when reading multi-megabyte .efi binaries (such as 12-16 MB video builds).
  Generating a spec-compliant FAT32 ESP image in pure Python stdlib takes <50 ms,
  needs zero admin privileges or external tools, and handles arbitrary .efi sizes.

Usage:
  python tools/run_qemu.py                # Build ESP image and boot in QEMU
  python tools/run_qemu.py --build-only   # Only build & validate efi/esp_qemu.img
  python tools/run_qemu.py --res 1280x720 # Boot with a smaller window resolution
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys

SECTOR_SIZE = 512
PART_START_LBA = 2048  # 1 MiB alignment


def lfn_checksum(short11: bytes) -> int:
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def make_short_83(name: str, used: set[bytes]) -> tuple[bytes, bool]:
    """Return (11-byte ASCII 8.3 short name, needs_vfat_lfn)."""
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""

    clean_b = "".join(ch for ch in base.upper() if ch.isalnum() or ch in "_-")
    clean_e = "".join(ch for ch in ext.upper() if ch.isalnum() or ch in "_-")
    exact_fit = (
        base == clean_b
        and ext == clean_e
        and 1 <= len(clean_b) <= 8
        and len(clean_e) <= 3
        and ("." not in base)
    )

    if exact_fit:
        cand = clean_b.ljust(8).encode("ascii") + clean_e.ljust(3).encode("ascii")
        if cand not in used:
            used.add(cand)
            return cand, (name != name.upper())

    prefix = (clean_b[:6] or "FILE").upper()
    ext3 = clean_e[:3].ljust(3)
    for idx in range(1, 1000):
        tag = f"~{idx}"
        b8 = (prefix[: 8 - len(tag)] + tag).ljust(8)
        cand = (b8 + ext3).encode("ascii")
        if cand not in used:
            used.add(cand)
            return cand, True
    raise RuntimeError(f"Could not allocate 8.3 short name for {name}")


def pack_dir_entry_with_lfn(name: str, attr: int, first_cluster: int, size: int, used: set[bytes]) -> bytes:
    short11, need_lfn = make_short_83(name, used)
    out = bytearray()

    if need_lfn:
        chk = lfn_checksum(short11)
        u16 = [ord(c) for c in name]
        if len(u16) % 13 != 0:
            u16.append(0x0000)
            while len(u16) % 13 != 0:
                u16.append(0xFFFF)

        n_slots = len(u16) // 13
        for slot_idx in range(n_slots, 0, -1):
            chunk = u16[(slot_idx - 1) * 13 : slot_idx * 13]
            ord_byte = slot_idx | (0x40 if slot_idx == n_slots else 0x00)
            p1 = struct.pack("<5H", *chunk[0:5])
            p2 = struct.pack("<6H", *chunk[5:11])
            p3 = struct.pack("<2H", *chunk[11:13])
            out += struct.pack("<B10sBBB12sH4s", ord_byte, p1, 0x0F, 0x00, chk, p2, 0x0000, p3)

    out += struct.pack(
        "<11sBBBHHHHHHHI",
        short11,
        attr,
        0,
        0,
        0x0000,
        0x597C,
        0x597C,
        (first_cluster >> 16) & 0xFFFF,
        0x0000,
        0x597C,
        first_cluster & 0xFFFF,
        0 if (attr & 0x10) else size,
    )
    return bytes(out)


def pack_dot_entry(dot_name: bytes, cluster: int) -> bytes:
    return struct.pack(
        "<11sBBBHHHHHHHI",
        dot_name.ljust(11, b" "),
        0x10,
        0,
        0,
        0x0000,
        0x597C,
        0x597C,
        (cluster >> 16) & 0xFFFF,
        0x0000,
        0x597C,
        cluster & 0xFFFF,
        0,
    )


def build_fat32_esp_image(img_path: str, file_map: dict[str, bytes]) -> dict[str, int]:
    """
    Create an MBR-partitioned FAT32 EFI System Partition image containing `file_map`
    (keys = '/EFI/BOOT/BOOTX64.EFI' style paths, values = raw bytes).
    """
    payload_total = sum(len(v) for v in file_map.values())
    # Choose cluster size so CountOfClusters >= 66,000 (strictly FAT32 per Microsoft/EDK2 spec)
    if payload_total <= 56 * 1024 * 1024:
        sec_per_clus = 1
    elif payload_total <= 116 * 1024 * 1024:
        sec_per_clus = 2
    else:
        sec_per_clus = 4

    clus_bytes = SECTOR_SIZE * sec_per_clus
    needed_file_clusters = sum(max(1, math.ceil(len(v) / clus_bytes)) for v in file_map.values())
    data_clusters = max(66048, needed_file_clusters + 2048)

    rsvd_sectors = 32
    num_fats = 2
    fat_entries = data_clusters + 2
    fat_sz_sectors = math.ceil((fat_entries * 4) / SECTOR_SIZE)
    part_sectors = rsvd_sectors + num_fats * fat_sz_sectors + data_clusters * sec_per_clus
    total_disk_sectors = PART_START_LBA + part_sectors

    # Build directory tree in memory
    # Each dir node: {"children_dirs": dict, "files": dict[name, bytes], "cluster": int, "parent_cluster": int}
    root = {"dirs": {}, "files": {}, "cluster": 2, "parent_cluster": 0}
    for rel_path, data in file_map.items():
        parts = [p for p in rel_path.replace("\\", "/").strip("/").split("/") if p]
        cur = root
        for dname in parts[:-1]:
            if dname not in cur["dirs"]:
                cur["dirs"][dname] = {"dirs": {}, "files": {}, "cluster": 0, "parent_cluster": 0}
            cur = cur["dirs"][dname]
        cur["files"][parts[-1]] = data

    fat = [0] * fat_entries
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF

    next_cluster = 2
    cluster_blobs: list[tuple[int, bytes]] = []

    def alloc_chain(byte_len: int) -> int:
        nonlocal next_cluster
        n_clus = max(1, math.ceil(max(1, byte_len) / clus_bytes))
        start = next_cluster
        for i in range(n_clus):
            c = start + i
            fat[c] = (c + 1) if (i + 1 < n_clus) else 0x0FFFFFFF
        next_cluster += n_clus
        return start

    # Pre-order allocate directory clusters (root must be cluster 2)
    dir_order = []

    def assign_dirs(node, is_root: bool):
        if is_root:
            node["cluster"] = alloc_chain(clus_bytes)
            assert node["cluster"] == 2
        dir_order.append((node, is_root))
        for child in node["dirs"].values():
            child["parent_cluster"] = 0 if is_root else node["cluster"]
            child["cluster"] = alloc_chain(clus_bytes)
            assign_dirs(child, False)

    assign_dirs(root, True)

    # Allocate file clusters and serialize directory tables
    for node, is_root in dir_order:
        used_83: set[bytes] = set()
        buf = bytearray()
        if not is_root:
            buf += pack_dot_entry(b".", node["cluster"])
            buf += pack_dot_entry(b"..", node["parent_cluster"])

        for dname, child in node["dirs"].items():
            buf += pack_dir_entry_with_lfn(dname, 0x10, child["cluster"], 0, used_83)

        for fname, fbytes in node["files"].items():
            if len(fbytes) == 0:
                f_clus = 0
            else:
                f_clus = alloc_chain(len(fbytes))
                cluster_blobs.append((f_clus, fbytes))
            buf += pack_dir_entry_with_lfn(fname, 0x20, f_clus, len(fbytes), used_83)

        if len(buf) > clus_bytes:
            raise RuntimeError("Directory entries exceeded single cluster; increase cluster reserve.")
        cluster_blobs.append((node["cluster"], bytes(buf)))

    free_clusters = (data_clusters + 2) - next_cluster

    # Construct MBR at LBA 0
    mbr = bytearray(SECTOR_SIZE)
    mbr[446 : 446 + 16] = struct.pack(
        "<BBBBBBBBII",
        0x80,                 # Active/bootable
        0xFE, 0xFF, 0xFF,     # CHS start (LBA mode)
        0xEF,                 # Partition type 0xEF = EFI System Partition
        0xFE, 0xFF, 0xFF,     # CHS end
        PART_START_LBA,
        part_sectors,
    )
    mbr[510:512] = b"\x55\xAA"

    # Construct FAT32 VBR (Boot Sector) at Partition Sector 0
    vbr = bytearray(SECTOR_SIZE)
    vbr[0:3] = b"\xEB\x58\x90"
    vbr[3:11] = b"MSWIN4.1"
    struct.pack_into(
        "<HBHBHHBHHHIIIHHIHH12sBBBI11s8s",
        vbr,
        11,
        SECTOR_SIZE,          # BPB_BytsPerSec
        sec_per_clus,         # BPB_SecPerClus
        rsvd_sectors,         # BPB_RsvdSecCnt
        num_fats,             # BPB_NumFATs
        0,                    # BPB_RootEntCnt (0 for FAT32)
        0,                    # BPB_TotSec16   (0 for FAT32)
        0xF8,                 # BPB_Media
        0,                    # BPB_FATSz16    (0 for FAT32)
        63,                   # BPB_SecPerTrk
        255,                  # BPB_NumHeads
        PART_START_LBA,       # BPB_HiddSec
        part_sectors,         # BPB_TotSec32
        fat_sz_sectors,       # BPB_FATSz32
        0,                    # BPB_ExtFlags
        0,                    # BPB_FSVer
        2,                    # BPB_RootClus
        1,                    # BPB_FSInfo
        6,                    # BPB_BkBootSec
        b"\x00" * 12,         # BPB_Reserved
        0x80,                 # BS_DrvNum
        0,                    # BS_Reserved1
        0x29,                 # BS_BootSig
        0x20261128,           # BS_VolID
        b"UEFI_ESP   ",       # BS_VolLab (11 bytes)
        b"FAT32   ",          # BS_FilSysType (8 bytes)
    )
    vbr[510:512] = b"\x55\xAA"

    # Construct FSInfo at Partition Sector 1
    fsinfo = bytearray(SECTOR_SIZE)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, free_clusters)
    struct.pack_into("<I", fsinfo, 492, next_cluster)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    # Serialize FAT table
    fat_bytes = bytearray(fat_sz_sectors * SECTOR_SIZE)
    struct.pack_into(f"<{fat_entries}I", fat_bytes, 0, *fat)

    os.makedirs(os.path.dirname(img_path) or ".", exist_ok=True)
    total_bytes = total_disk_sectors * SECTOR_SIZE
    with open(img_path, "wb") as f:
        f.truncate(total_bytes)
        f.seek(0)
        f.write(mbr)

        part_base = PART_START_LBA * SECTOR_SIZE
        f.seek(part_base + 0 * SECTOR_SIZE)
        f.write(vbr)
        f.seek(part_base + 1 * SECTOR_SIZE)
        f.write(fsinfo)
        # Backup boot + FSInfo at sectors 6 & 7
        f.seek(part_base + 6 * SECTOR_SIZE)
        f.write(vbr)
        f.seek(part_base + 7 * SECTOR_SIZE)
        f.write(fsinfo)

        fat1_off = part_base + rsvd_sectors * SECTOR_SIZE
        fat2_off = fat1_off + len(fat_bytes)
        f.seek(fat1_off)
        f.write(fat_bytes)
        f.seek(fat2_off)
        f.write(fat_bytes)

        data_off = fat2_off + len(fat_bytes)
        for clus_idx, blob in cluster_blobs:
            f.seek(data_off + (clus_idx - 2) * clus_bytes)
            f.write(blob)

    return {
        "total_mb": round(total_bytes / (1024 * 1024), 2),
        "data_clusters": data_clusters,
        "used_clusters": next_cluster - 2,
    }


def find_qemu_binary(explicit: str | None) -> str | None:
    if explicit and os.path.isfile(explicit):
        return explicit
    env_q = os.environ.get("QEMU")
    if env_q and (os.path.isfile(env_q) or shutil.which(env_q)):
        return shutil.which(env_q) or env_q

    found = shutil.which("qemu-system-x86_64")
    if found:
        return found

    if os.name == "nt":
        user = os.environ.get("USERPROFILE", "")
        scoop = os.environ.get("SCOOP", os.path.join(user, "scoop"))
        win_candidates = [
            r"C:\Program Files\qemu\qemu-system-x86_64.exe",
            r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
            os.path.join(scoop, r"apps\qemu\current\qemu-system-x86_64.exe"),
            r"C:\ProgramData\chocolatey\bin\qemu-system-x86_64.exe",
            r"C:\msys64\ucrt64\bin\qemu-system-x86_64.exe",
            r"C:\msys64\mingw64\bin\qemu-system-x86_64.exe",
        ]
        for c in win_candidates:
            if os.path.isfile(c):
                return c
    return None


def find_ovmf_firmware(qemu_exe: str | None, explicit_code: str | None) -> tuple[str | None, str | None]:
    """Return (code_fd_path, vars_template_fd_path_or_None)."""
    if explicit_code and os.path.isfile(explicit_code):
        return explicit_code, None
    env_ovmf = os.environ.get("OVMF")
    if env_ovmf and os.path.isfile(env_ovmf):
        return env_ovmf, None

    pairs = []
    if qemu_exe:
        qdir = os.path.dirname(os.path.abspath(qemu_exe))
        for share_rel in ("share", os.path.join("..", "share", "qemu"), os.path.join("..", "share")):
            sdir = os.path.normpath(os.path.join(qdir, share_rel))
            pairs.append((
                os.path.join(sdir, "edk2-x86_64-code.fd"),
                os.path.join(sdir, "edk2-i386-vars.fd"),
            ))
            pairs.append((os.path.join(sdir, "OVMF.fd"), None))

    pairs.extend([
        ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
        ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
        ("/usr/share/edk2-ovmf/x64/OVMF_CODE.fd", "/usr/share/edk2-ovmf/x64/OVMF_VARS.fd"),
        ("/usr/share/qemu/edk2-x86_64-code.fd", "/usr/share/qemu/edk2-i386-vars.fd"),
        ("/usr/share/ovmf/OVMF.fd", None),
        ("/usr/share/qemu/OVMF.fd", None),
    ])

    for code_p, vars_p in pairs:
        if os.path.isfile(code_p):
            valid_vars = vars_p if (vars_p and os.path.isfile(vars_p)) else None
            return code_p, valid_vars
    return None, None


def detect_supported_accels(qemu_exe: str) -> set[str]:
    try:
        res = subprocess.run(
            [qemu_exe, "-accel", "help"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        return {line.strip().lower() for line in res.stdout.splitlines() if line.strip()}
    except Exception:
        return set()


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack efi/esp_qemu.img and boot uefi-gop-video-boot in QEMU + OVMF.")
    parser.add_argument("--efi", default="efi/bootx64.efi", help="Path to primary UEFI binary (default: efi/bootx64.efi)")
    parser.add_argument("--mock-ms", default="efi/mock_bootmgfw.efi", help="Optional mock bootmgfw.efi chainload stub")
    parser.add_argument("--img", default="efi/esp_qemu.img", help="Output FAT32 ESP image path")
    parser.add_argument("--res", default="1920x1080", help="Virtual GOP display resolution (default: 1920x1080)")
    parser.add_argument("--ram", default="1024M", help="Guest RAM size (default: 1024M)")
    parser.add_argument("--cpus", type=int, default=4, help="Virtual CPU count (default: 4)")
    parser.add_argument("--qemu", default=None, help="Explicit path to qemu-system-x86_64")
    parser.add_argument("--ovmf", default=None, help="Explicit path to OVMF_CODE.fd / edk2-x86_64-code.fd")
    parser.add_argument("--whpx", action="store_true", help="Enable Windows Hypervisor Platform (-accel whpx) on Windows")
    parser.add_argument("--build-only", action="store_true", help="Build FAT32 ESP image and exit without launching QEMU")
    args = parser.parse_args()

    if not os.path.isfile(args.efi):
        print(f"Error: '{args.efi}' not found. Run `mingw32-make` (or `make`) first.", file=sys.stderr)
        return 1

    with open(args.efi, "rb") as f:
        boot_efi_bytes = f.read()

    esp_files: dict[str, bytes] = {
        "/EFI/BOOT/BOOTX64.EFI": boot_efi_bytes,
    }
    if os.path.isfile("config.txt"):
        with open("config.txt", "rb") as f:
            esp_files["/EFI/HackBGRT/config.txt"] = f.read()

    if args.mock_ms and os.path.isfile(args.mock_ms):
        with open(args.mock_ms, "rb") as f:
            esp_files["/EFI/Microsoft/Boot/bootmgfw.efi"] = f.read()

    stats = build_fat32_esp_image(args.img, esp_files)
    print(
        f"[ESP] Built FAT32 image '{args.img}' ({stats['total_mb']} MiB, "
        f"{stats['used_clusters']}/{stats['data_clusters']} clusters, {len(esp_files)} files)"
    )

    if args.build_only:
        return 0

    qemu_exe = find_qemu_binary(args.qemu)
    if not qemu_exe:
        print("\n[!] QEMU is not installed or not in PATH.", file=sys.stderr)
        if os.name == "nt":
            print("    Install QEMU (includes OVMF UEFI firmware) with:", file=sys.stderr)
            print("      winget install SoftwareFreedomConservancy.QEMU", file=sys.stderr)
        else:
            print("    Install QEMU + OVMF with:", file=sys.stderr)
            print("      sudo apt install -y qemu-system-x86 ovmf", file=sys.stderr)
        return 2

    code_fd, vars_tmpl = find_ovmf_firmware(qemu_exe, args.ovmf)
    if not code_fd:
        print(f"\n[!] Found QEMU ({qemu_exe}), but OVMF UEFI firmware (.fd) was not found.", file=sys.stderr)
        print("    Pass --ovmf path/to/OVMF_CODE.fd or install the `ovmf` package.", file=sys.stderr)
        return 2

    try:
        xres_s, yres_s = args.res.lower().split("x", 1)
        xres, yres = int(xres_s), int(yres_s)
    except ValueError:
        xres, yres = 1920, 1080

    # Cap VGA VRAM cleanly to target resolution so Bochs/QemuVideoDxe picks exact --res (e.g. 1920x1080)
    # instead of overshooting to 4096x2160 under software emulation.
    fb_mb = math.ceil((xres * yres * 4) / (1024 * 1024))
    vgamem_mb = 16 if fb_mb <= 12 else (32 if fb_mb <= 24 else 64)

    accels = detect_supported_accels(qemu_exe)
    cmd = [
        qemu_exe,
        "-name", "uefi-gop-video-boot (QEMU + OVMF)",
        "-machine", "q35",
        "-m", args.ram,
        "-smp", str(max(1, args.cpus)),
        "-net", "none",
        "-display", "gtk,zoom-to-fit=on",
        "-vga", "none",
        "-device", f"VGA,vgamem_mb={vgamem_mb},edid=on,xres={xres},yres={yres}",
        "-usb",
        "-device", "usb-kbd",
    ]

    if os.name != "nt" and "kvm" in accels and os.access("/dev/kvm", os.R_OK | os.W_OK):
        cmd += ["-accel", "kvm", "-cpu", "host"]
    elif os.name == "nt" and args.whpx and "whpx" in accels:
        cmd += ["-accel", "whpx", "-cpu", "qemu64,+aes,+sse4.2,+avx,+avx2"]
    else:
        cmd += ["-accel", "tcg,thread=multi", "-cpu", "qemu64,+aes,+sse4.2,+avx,+avx2"]

    if vars_tmpl:
        vars_copy = os.path.join(os.path.dirname(args.img) or "efi", "ovmf_vars.fd")
        if not os.path.isfile(vars_copy) or os.path.getsize(vars_copy) != os.path.getsize(vars_tmpl):
            shutil.copyfile(vars_tmpl, vars_copy)
        cmd += [
            "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code_fd}",
            "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
        ]
    else:
        cmd += ["-bios", code_fd]

    cmd += [
        "-drive", f"if=none,id=esp,format=raw,file={args.img}",
        "-device", "nvme,serial=UEFIGOP01,drive=esp",
    ]

    print(f"[QEMU] Launching {qemu_exe} ({xres}x{yres} GOP, firmware: {os.path.basename(code_fd)})...")
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
