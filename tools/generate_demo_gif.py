#!/usr/bin/env python3
"""
Render a seamless 800x450 @ 20 FPS demonstration GIF (assets/demo.gif, <= 4.5 MB):
  - 0.0s .. 2.5s (50 frames): High-resolution UEFI console (rapid dmesg + colored systemd)
  - 2.5s .. 6.5s (80 frames): Full-screen 255-color GOP->Blt() 320x180 scanline video render
    sourced from local clip.mp4 (if available) or the procedural cyber-horizon scene.
"""

import argparse
import os
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT_W, OUT_H = 800, 450
FPS = 20
CONSOLE_FRAMES = 50   # 2.5s
VIDEO_FRAMES = 80     # 4.0s

DMESG_SAMPLE = [
    ("    0.000000", "Linux version 6.12.6-amd64", "(gcc 14.2.0, GNU ld 2.43.1) #1 SMP PREEMPT_DYNAMIC"),
    ("    0.000104", "BIOS-e820", "[mem 0x0000000000100000-0x000000087fffffff] usable"),
    ("    0.000689", "efi", "EFI v2.80 by American Megatrends: ACPI=0x7a9f2000 SMBIOS=0x7ab10000"),
    ("    0.002018", "tsc", "Fast TSC calibration using PIT: 3792.842 MHz"),
    ("    0.004128", "ACPI", "Core revision 20240827 — RSDP/XSDT/FADT/MCFG/DMAR tables parsed"),
    ("    0.007912", "Zone ranges", "DMA32 [mem 0x00001000-0xffffffff], Normal [mem 0x100000000-0x87fffffff]"),
    ("    0.009480", "SLUB", "HWalign=64, Order=0-3, MinObjects=0, CPUs=16, Nodes=1"),
    ("    0.012309", "APIC", "Switched APIC routing to physical x2apic (IOAPIC[0] GSI 0-119)"),
    ("    0.016044", "smpboot", "Brought up 1 node, 16 CPUs — Total of 16 processors activated"),
    ("    0.018940", "pci 0000:00:01.0", "PME# supported from D0 D3hot D3cold"),
    ("    0.022045", "pci 0000:00:06.0", "PCI bridge to [bus 02] (PCIe Gen4 Root Port x4)"),
    ("    0.024904", "pci 0000:01:00.0", "VGA compatible controller: BAR 0 [mem 0x6000000000-0x63ffffffff 64bit]"),
    ("    0.026318", "DMAR", "IOMMU enabled: Queued invalidation & IRQ remapping active"),
    ("    0.040192", "cryptd", "max_cpu_qlen set to 1000 (AES-NI, AVX2, SHA-NI hardware accel)"),
    ("    0.042110", "nvme nvme0", "pci function 0000:02:00.0 — 16/0/0 queues, NVMe 1.4 Namespace 1 ready"),
    ("    0.049811", "xhci_hcd 0000:00:14.0", "xHCI Host Controller — USB 3.2 Enhanced SuperSpeed (20 Gbps)"),
    ("    0.058920", "tpm_tis MSFT0101:00", "2.0 TPM (device-id 0x1B, rev-id 22) CRB interface active"),
    ("    0.064118", "EXT4-fs (nvme0n1p4)", "mounted root filesystem readonly, ordered data mode"),
    ("    0.071880", "systemd[1]", "systemd 256.8 running in system mode (+PAM +AUDIT +SECCOMP +TPM2 +ZSTD)"),
]

SYSTEMD_SAMPLE = [
    (True,  "Created slice ",  "system-modprobe.slice",       " — Slice /system/modprobe"),
    (True,  "Listening on ",   "/run/systemd/journal/socket", " — Journal Socket"),
    (True,  "Listening on ",   "/run/udev/control",           " — udev Control Socket"),
    (True,  "Mounted ",        "/sys/firmware/efi/efivars",   " — EFI Variable File System"),
    (True,  "Finished ",       "",                            "Load Kernel Modules (nvme, xhci_pci, vfat)"),
    (True,  "Started ",        "",                            "Journal Service (systemd-journald)"),
    (True,  "Started ",        "",                            "Rule-based Manager for Device Events and Files"),
    (True,  "Finished ",       "",                            "Coldplug All udev Devices"),
    (True,  "Found device ",   "/dev/nvme0n1p1",              " — EFI System Partition"),
    (True,  "Found device ",   "/dev/tpmrm0",                 " — TPM 2.0 Security Processor"),
    (True,  "Mounted ",        "/boot/efi",                   " — EFI System Partition (FAT32)"),
    (True,  "Reached target ", "",                            "Local File Systems"),
    (True,  "Finished ",       "",                            "Flush Journal to Persistent Storage"),
    (True,  "Found device ",   "enp4s0",                      " — PCIe 2.5GbE Ethernet Controller"),
    (True,  "Reached target ", "",                            "System Initialization"),
    (True,  "Started ",        "",                            "D-Bus System Message Bus"),
    (True,  "Started ",        "enp4s0",                      " — Network Configuration (systemd-networkd)"),
    (True,  "Started ",        "",                            "User Login Management (systemd-logind)"),
    (True,  "Reached target ", "",                            "Multi-User System"),
    (True,  "Reached target ", "",                            "Graphical Interface"),
    (False, "Starting ",       "GOP->Blt()",                  " — 24-bit 320x180 Temporal-RLE Stream..."),
]


def load_mono_font(size: int = 12):
    candidates = [
        r"C:\Windows\Fonts\consola.ttf",
        r"C:\Windows\Fonts\CascadiaMono.ttf",
        r"C:\Windows\Fonts\lucon.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    ]
    for p in candidates:
        if os.path.exists(p):
            return ImageFont.truetype(p, size=size)
    return ImageFont.load_default()


def render_console_frames() -> list[Image.Image]:
    font = load_mono_font(12)
    line_h = 14
    max_vis_lines = (OUT_H - 16) // line_h  # ~31 visible rows

    total_events = len(DMESG_SAMPLE) + len(SYSTEMD_SAMPLE)
    frames = []

    for f in range(CONSOLE_FRAMES):
        # Smooth ease-in-out reveal of all 40 boot log lines across 50 frames (holds last 5 frames)
        prog = min(1.0, f / float(CONSOLE_FRAMES - 6))
        visible_count = max(1, int(round(prog * total_events)))

        img = Image.new("RGB", (OUT_W, OUT_H), (0, 0, 0))
        draw = ImageDraw.Draw(img)

        start_idx = max(0, visible_count - max_vis_lines)
        y = 8
        for idx in range(start_idx, visible_count):
            x = 10
            if idx < len(DMESG_SAMPLE):
                ts, sub, msg = DMESG_SAMPLE[idx]
                s1 = f"[{ts}] "
                draw.text((x, y), s1, fill=(180, 180, 180), font=font)
                x += int(draw.textlength(s1, font=font))
                s2 = f"{sub}: "
                draw.text((x, y), s2, fill=(255, 255, 255), font=font)
                x += int(draw.textlength(s2, font=font))
                draw.text((x, y), msg, fill=(185, 185, 185), font=font)
            else:
                is_ok, verb, cyan, white = SYSTEMD_SAMPLE[idx - len(DMESG_SAMPLE)]
                if is_ok:
                    draw.text((x, y), "[  ", fill=(185, 185, 185), font=font)
                    x += int(draw.textlength("[  ", font=font))
                    draw.text((x, y), "OK", fill=(85, 255, 85), font=font)
                    x += int(draw.textlength("OK", font=font))
                    draw.text((x, y), "  ] ", fill=(185, 185, 185), font=font)
                    x += int(draw.textlength("  ] ", font=font))
                else:
                    pad = "         "
                    x += int(draw.textlength(pad, font=font))

                draw.text((x, y), verb, fill=(190, 190, 190), font=font)
                x += int(draw.textlength(verb, font=font))
                if cyan:
                    draw.text((x, y), cyan, fill=(85, 255, 255), font=font)
                    x += int(draw.textlength(cyan, font=font))
                if white:
                    suffix = f"{white}." if is_ok else white
                    draw.text((x, y), suffix, fill=(255, 255, 255), font=font)
            y += line_h

        frames.append(img)
    return frames


def stylize_gop_320x180(rgb_frame: np.ndarray, pal_quant: Image.Image | None) -> Image.Image:
    """Apply our exact 320x180 sharpen + 255-color quantization + scanline GOP look at 800x450."""
    small = cv2.resize(rgb_frame, (320, 180), interpolation=cv2.INTER_AREA).astype(np.float32)
    contrasted = np.clip((small - 124.0) * 1.09 + 126.0, 0.0, 255.0)
    blurred = cv2.GaussianBlur(contrasted, (0, 0), sigmaX=0.85)
    detail = contrasted - blurred
    edge_mask = (np.abs(detail) >= 3.5).astype(np.float32)
    sharp_u8 = np.clip(contrasted + detail * (0.60 * edge_mask), 0.0, 255.0).astype(np.uint8)

    pil_small = Image.fromarray(sharp_u8, mode="RGB")
    if pal_quant is not None:
        pil_small = pil_small.quantize(palette=pal_quant, dither=Image.Dither.NONE).convert("RGB")

    # Upscale crisp blocks to 800x450 via NEAREST and add subtle GOP scanline shading
    up = np.array(pil_small.resize((OUT_W, OUT_H), resample=Image.Resampling.NEAREST), dtype=np.uint16)
    up[2::3, :, :] = (up[2::3, :, :] * 7) >> 3  # 12.5% scanline every 3rd row
    return Image.fromarray(up.astype(np.uint8), mode="RGB")


def auto_crop_letterbox_16x9(frames: list[np.ndarray]) -> list[np.ndarray]:
    """Detect baked-in black letterbox bars across sampled frames and crop to clean 16:9."""
    sample = np.max(np.stack(frames[::4], axis=0), axis=0)
    row_luma = np.mean(sample, axis=(1, 2))
    active_rows = np.where(row_luma > 10.0)[0]
    h, w, _ = frames[0].shape
    if len(active_rows) < h // 3:
        return frames
    y0, y1 = int(active_rows[0]), int(active_rows[-1]) + 1
    crop_h = y1 - y0
    crop_w = min(w, int(round(crop_h * (16.0 / 9.0))))
    x0 = (w - crop_w) // 2
    if crop_h < 80 or crop_w < 140:
        return frames
    return [f[y0:y1, x0 : x0 + crop_w] for f in frames]


def extract_video_segment(clip_path: str) -> list[np.ndarray]:
    if os.path.exists(clip_path):
        cap = cv2.VideoCapture(clip_path)
        src_fps = cap.get(cv2.CAP_PROP_FPS) or 24.0
        raw = []
        while True:
            ok, bgr = cap.read()
            if not ok:
                break
            raw.append(bgr[:, :, ::-1])
        cap.release()
        if len(raw) >= 20:
            cropped = auto_crop_letterbox_16x9(raw)
            dur_s = len(cropped) / src_fps
            win_s = VIDEO_FRAMES / float(FPS)
            # Pick the 4.0s window with highest brightness + contrast energy
            best_t = 0.0
            best_score = -1.0
            t = 0.0
            while t + win_s <= dur_s:
                i0 = int(t * src_fps)
                i1 = min(len(cropped), int((t + win_s) * src_fps))
                sub = cropped[i0:i1:6]
                if sub:
                    arr = np.stack(sub, axis=0).astype(np.float32)
                    score = float(np.mean(arr)) + 1.4 * float(np.std(arr))
                    if score > best_score:
                        best_score = score
                        best_t = t
                t += 0.5

            out = []
            for i in range(VIDEO_FRAMES):
                ts = best_t + (i / float(FPS))
                s_idx = min(int(round(ts * src_fps)), len(cropped) - 1)
                out.append(cropped[s_idx])
            return out

    # Procedural fallback if clip.mp4 is absent
    from video_to_color_ascii import generate_procedural_demo_frames
    return generate_procedural_demo_frames(320, 180, FPS, seconds=VIDEO_FRAMES / float(FPS))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clip", default="clip.mp4")
    parser.add_argument("--out", default="assets/demo.gif")
    parser.add_argument("--max-mb", type=float, default=4.5)
    args = parser.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    console_imgs = render_console_frames()
    raw_vid = extract_video_segment(args.clip)

    # Build shared 255-color palette for the video stage
    sample_strip = np.vstack([cv2.resize(f, (320, 180), interpolation=cv2.INTER_AREA) for f in raw_vid[::4]])
    pal_quant = Image.fromarray(sample_strip, mode="RGB").quantize(
        colors=255, method=Image.Quantize.MEDIANCUT, kmeans=3
    )
    video_imgs = [stylize_gop_320x180(f, pal_quant) for f in raw_vid]

    all_rgb = console_imgs + video_imgs
    max_bytes = int(args.max_mb * 1024 * 1024)

    # Save static high-contrast composite cover (<1 MB) as well for Habr feed cover if needed
    cover_path = os.path.join(os.path.dirname(args.out) or ".", "cover.png")
    split = Image.new("RGB", (OUT_W, OUT_H))
    split.paste(console_imgs[-2].crop((0, 0, OUT_W // 2, OUT_H)), (0, 0))
    split.paste(video_imgs[len(video_imgs) // 2].crop((OUT_W // 2, 0, OUT_W, OUT_H)), (OUT_W // 2, 0))
    draw_c = ImageDraw.Draw(split)
    draw_c.line([(OUT_W // 2, 0), (OUT_W // 2, OUT_H)], fill=(85, 255, 160), width=2)
    split.save(cover_path, format="PNG", optimize=True)

    for n_colors in (192, 144, 112, 88, 64):
        # Quantize frames with a unified global palette so GIF LZW + frame deltas compress tightly
        strip_all = np.vstack([np.array(im)[::2, ::2] for im in all_rgb[::3]])
        master_pal = Image.fromarray(strip_all, mode="RGB").quantize(
            colors=n_colors, method=Image.Quantize.MEDIANCUT, kmeans=2
        )
        p_frames = [
            im.quantize(palette=master_pal, dither=Image.Dither.NONE)
            for im in all_rgb
        ]
        p_frames[0].save(
            args.out,
            save_all=True,
            append_images=p_frames[1:],
            duration=int(1000 / FPS),
            loop=0,
            optimize=True,
        )
        sz = os.path.getsize(args.out)
        print(f"[colors={n_colors}] {args.out}: {sz / (1024 * 1024):.2f} MB ({len(p_frames)} frames @ {FPS} FPS)")
        if sz <= max_bytes:
            break

    print(f"Cover image: {cover_path} ({os.path.getsize(cover_path) / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
