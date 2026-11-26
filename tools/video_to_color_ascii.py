#!/usr/bin/env python3
"""
Encode an input video (or built-in procedural --demo animation) into a 255-color 24-bit RGB
Temporal-RLE C header for UEFI GOP->Blt() playback.

Usage examples:
  python tools/video_to_color_ascii.py input.mp4 --fps 24 --width 320 --height 180
  python tools/video_to_color_ascii.py --demo
"""

import argparse
import math
import os
import cv2
import numpy as np
from PIL import Image


def preprocess_and_sharpen(frame_rgb: np.ndarray, width: int, height: int) -> np.ndarray:
    """
    Area-downsample to (width, height), boost contrast slightly, and apply a thresholded
    unsharp mask so fine details (contours, eyes, subtitles) stay crisp without amplifying
    flat-surface compression noise.
    """
    small = cv2.resize(frame_rgb, (width, height), interpolation=cv2.INTER_AREA).astype(np.float32)
    contrasted = np.clip((small - 124.0) * 1.09 + 126.0, 0.0, 255.0)

    blurred = cv2.GaussianBlur(contrasted, (0, 0), sigmaX=0.85)
    detail = contrasted - blurred
    edge_mask = (np.abs(detail) >= 3.5).astype(np.float32)
    sharpened = np.clip(contrasted + detail * (0.60 * edge_mask), 0.0, 255.0)
    return sharpened.astype(np.uint8)


def generate_procedural_demo_frames(width: int = 320, height: int = 180, fps: int = 24, seconds: float = 3.0):
    """
    Generate a clean 3-second procedural retro-horizon & progress HUD boot animation using
    horizontal scanline runs so src/demo_frames.h stays ultra-compact (<60 KB) for git clone.
    """
    n_frames = max(1, int(round(fps * seconds)))
    frames = []
    horizon = height // 2

    y_coords = np.arange(height, dtype=np.float32)[:, None]
    x_coords = np.arange(width, dtype=np.float32)[None, :]

    for f in range(n_frames):
        t = f / float(n_frames)
        img = np.zeros((height, width, 3), dtype=np.uint8)

        # Banded night sky gradient (pure horizontal runs -> minimal RLE size)
        sky_y = np.arange(horizon, dtype=np.int32)
        band = (sky_y // 8) * 8
        img[:horizon, :, 0] = (10 + band // 4)[:, None]
        img[:horizon, :, 1] = (14 + band // 5)[:, None]
        img[:horizon, :, 2] = (34 + band // 2)[:, None]

        # Static retro sun disc with scrolling horizontal scan-cutouts
        cx, cy, rad = width // 2, horizon - 22, 26
        sun_mask = ((x_coords - cx) ** 2 + (y_coords[:horizon] - cy) ** 2) <= (rad * rad)
        cutout = ((np.arange(horizon)[:, None] + (f // 2)) % 6) != 0
        img[:horizon][sun_mask & cutout] = [255, 196, 48]

        # Perspective scrolling horizontal floor bars
        floor_h = height - horizon
        fy = np.arange(1, floor_h + 1, dtype=np.float32)
        z_phase = ((36.0 / fy) + t * 3.0) % 1.0
        floor = np.empty((floor_h, width, 3), dtype=np.uint8)
        floor[:, :] = [10, 16, 28]
        floor[z_phase < 0.16, :] = [0, 205, 235]
        # Center runway lane
        lane_half = (fy * 0.55).astype(np.int32)
        mid = width // 2
        for r_i in range(floor_h):
            lh = lane_half[r_i]
            floor[r_i, max(0, mid - lh - 2) : max(0, mid - lh)] = [255, 196, 48]
            floor[r_i, min(width, mid + lh) : min(width, mid + lh + 2)] = [255, 196, 48]
        img[horizon:] = floor

        # Horizon glow & bottom boot progress bar
        img[horizon - 1 : horizon + 1, :] = [120, 248, 255]
        bar_x0, bar_w, bar_y0 = 40, width - 80, height - 18
        img[bar_y0 - 1 : bar_y0 + 7, bar_x0 - 1 : bar_x0 + bar_w + 1] = [70, 90, 115]
        img[bar_y0 : bar_y0 + 6, bar_x0 : bar_x0 + bar_w] = [16, 22, 34]
        fill_w = int(round(bar_w * ((f + 1) / float(n_frames))))
        if fill_w > 0:
            img[bar_y0 : bar_y0 + 6, bar_x0 : bar_x0 + fill_w] = [72, 240, 138]

        frames.append(img)

    return frames


def build_global_palette(processed_frames, n_colors: int = 255) -> np.ndarray:
    """Sample up to 80 frames across the clip to build an optimal 255-color 24-bit RGB palette."""
    step = max(1, len(processed_frames) // 80)
    samples = [processed_frames[i] for i in range(0, len(processed_frames), step)]
    strip = np.vstack(samples)
    pil_strip = Image.fromarray(strip, mode="RGB")
    quant = pil_strip.quantize(colors=n_colors, method=Image.Quantize.MEDIANCUT, kmeans=4)
    raw_pal = quant.getpalette() or []
    raw_pal = raw_pal[: n_colors * 3]
    pal_rgb = np.array(raw_pal, dtype=np.uint8).reshape(-1, 3)
    if len(pal_rgb) < n_colors:
        pad = np.zeros((n_colors - len(pal_rgb), 3), dtype=np.uint8)
        pal_rgb = np.vstack([pal_rgb, pad])
    return pal_rgb


def map_frames_to_palette(processed_frames, pal_rgb: np.ndarray):
    """Vectorized nearest-neighbor RGB mapping into 0..254."""
    pal_f = pal_rgb.astype(np.float32)
    pal_sq = np.sum(pal_f * pal_f, axis=1)[None, :]
    indexed = []

    for small in processed_frames:
        flat = small.reshape(-1, 3).astype(np.float32)
        dist_partial = pal_sq - 2.0 * (flat @ pal_f.T)
        best_idx = np.argmin(dist_partial, axis=1).astype(np.uint8)
        indexed.append(best_idx)

    return indexed


def rle_encode_1d(arr: np.ndarray) -> list[int]:
    """
    Fast NumPy run-length encoder returning packed UINT16 ints: color_idx | (run_len << 8),
    splitting runs > 255 into chunks of <= 255.
    """
    n = arr.shape[0]
    if n == 0:
        return []
    changes = np.flatnonzero(arr[1:] != arr[:-1]) + 1
    starts = np.empty(changes.shape[0] + 1, dtype=np.int32)
    starts[0] = 0
    starts[1:] = changes

    lengths = np.empty_like(starts)
    lengths[:-1] = changes - starts[:-1]
    lengths[-1] = n - starts[-1]
    values = arr[starts]

    packed_out = []
    for val, ln in zip(values.tolist(), lengths.tolist()):
        while ln > 255:
            packed_out.append(val | (255 << 8))
            ln -= 255
        packed_out.append(val | (ln << 8))
    return packed_out


def encode_temporal_rle(indexed_frames, processed_frames, pal_rgb: np.ndarray, diff_tol: int):
    """
    Encode frames into packed UINT16 tokens (low byte = color_idx, high byte = run_length).
    color_idx == 255 means keep `run_length` cells unchanged from the previous frame.
    """
    n_cells = indexed_frames[0].shape[0]
    screen_idx = np.zeros(n_cells, dtype=np.uint8)
    all_runs_u16 = []
    offsets = [0]

    for f_i, (idx_arr, tgt_rgb_2d) in enumerate(zip(indexed_frames, processed_frames)):
        if f_i == 0:
            emit = idx_arr
            screen_idx[:] = idx_arr
        else:
            cur_rgb = pal_rgb[screen_idx].astype(np.int16)
            tgt_rgb = tgt_rgb_2d.reshape(-1, 3).astype(np.int16)
            max_diff = np.max(np.abs(tgt_rgb - cur_rgb), axis=1)

            unchanged = (idx_arr == screen_idx) | (max_diff <= diff_tol)
            emit = np.where(unchanged, 255, idx_arr).astype(np.uint8)
            changed = ~unchanged
            screen_idx[changed] = idx_arr[changed]

        frame_tokens = rle_encode_1d(emit)
        all_runs_u16.extend(frame_tokens)
        offsets.append(len(all_runs_u16))

    return all_runs_u16, offsets


def write_gop_header(out_path: str, pal_rgb: np.ndarray, runs_u16, offsets, width: int, height: int, fps: int):
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    frame_us = int(round(1_000_000.0 / fps))
    num_frames = len(offsets) - 1
    num_runs = len(runs_u16)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write("#include \"uefi_compat.h\"\n\n")
        f.write(f"#define GOP_VIDEO_WIDTH       {width}U\n")
        f.write(f"#define GOP_VIDEO_HEIGHT      {height}U\n")
        f.write(f"#define GOP_VIDEO_FPS         {fps}U\n")
        f.write(f"#define GOP_VIDEO_FRAME_US    {frame_us}U\n")
        f.write(f"#define GOP_VIDEO_FRAME_COUNT {num_frames}U\n")
        f.write(f"#define GOP_VIDEO_RUN_COUNT   {num_runs}U\n")
        f.write("#define GOP_VIDEO_SKIP_INDEX  255U\n\n")

        f.write("/* 255-color 24-bit TrueColor palette packed as 32-bit BGR0 (0x00RRGGBB little-endian) */\n")
        f.write("static const UINT32 kGopVideoPalette32[255] = {\n    ")
        for i in range(255):
            r, g, b = int(pal_rgb[i, 0]), int(pal_rgb[i, 1]), int(pal_rgb[i, 2])
            bgr0 = b | (g << 8) | (r << 16)
            f.write(f"0x{bgr0:06X}U,")
            if (i + 1) % 8 == 0:
                f.write("\n    ")
            else:
                f.write(" ")
        f.write("\n};\n\n")

        f.write("static const UINT32 kGopFrameOffsets[GOP_VIDEO_FRAME_COUNT + 1] = {\n    ")
        for i, off in enumerate(offsets):
            f.write(f"{off}U,")
            if (i + 1) % 16 == 0:
                f.write("\n    ")
            else:
                f.write(" ")
        f.write("\n};\n\n")

        f.write("/* Packed RLE tokens: low byte = color_idx (0..254 or 255=skip), high byte = run_length */\n")
        f.write("static const UINT16 kGopVideoRunsU16[GOP_VIDEO_RUN_COUNT] = {\n    ")
        chunks = []
        for i in range(0, num_runs, 24):
            row_slice = runs_u16[i : i + 24]
            chunks.append(",".join(f"0x{v:04X}U" for v in row_slice))
        f.write(",\n    ".join(chunks))
        f.write("\n};\n")


def main():
    parser = argparse.ArgumentParser(
        description="Convert any video (or procedural --demo) into a 255-color UEFI GOP Temporal-RLE C header."
    )
    parser.add_argument("video", nargs="?", default=None, help="Input video file (e.g. input.mp4)")
    parser.add_argument("-i", "--input", dest="input_opt", default=None, help="Input video file (optional flag form)")
    parser.add_argument("-o", "--output", default=None, help="Output C header path (default: src/video_frames.h or src/demo_frames.h)")
    parser.add_argument("--width", type=int, default=320, help="Horizontal tile resolution (default: 320)")
    parser.add_argument("--height", type=int, default=180, help="Vertical tile resolution (default: 180)")
    parser.add_argument("--fps", type=int, default=24, help="Target playback FPS (default: 24)")
    parser.add_argument("--max-mb", type=float, default=12.8, help="Target max RLE payload in MB (default: 12.8)")
    parser.add_argument("--demo", action="store_true", help="Generate lightweight 3-second procedural src/demo_frames.h")
    args = parser.parse_args()

    if args.demo:
        out_path = args.output or "src/demo_frames.h"
        print(f"Generating 3-second procedural {args.width}x{args.height} @ {args.fps} FPS demo...")
        processed = generate_procedural_demo_frames(args.width, args.height, args.fps, seconds=3.0)
        pal_rgb = build_global_palette(processed, n_colors=255)
        indexed = map_frames_to_palette(processed, pal_rgb)
        runs_u16, offsets = encode_temporal_rle(indexed, processed, pal_rgb, diff_tol=4)
        payload = len(runs_u16) * 2 + len(offsets) * 4 + 255 * 4
        write_gop_header(out_path, pal_rgb, runs_u16, offsets, args.width, args.height, args.fps)
        print(
            f"Saved demo header {out_path} ({len(runs_u16):,} runs, "
            f"~{payload / 1024:.1f} KB binary, {os.path.getsize(out_path) / 1024:.1f} KB C source)"
        )
        return

    in_video = args.video or args.input_opt or "clip.mp4"
    out_path = args.output or "src/video_frames.h"

    cap = cv2.VideoCapture(in_video)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open video '{in_video}'. Pass a video path or run with --demo.")
    src_fps = cap.get(cv2.CAP_PROP_FPS) or 24.0
    raw_rgb = []
    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        raw_rgb.append(bgr[:, :, ::-1])
    cap.release()

    if not raw_rgb:
        raise RuntimeError(f"No video frames decoded from '{in_video}'.")

    dur = len(raw_rgb) / src_fps
    n_out = max(1, int(round(dur * args.fps)))
    print(f"Preprocessing & sharpening {n_out} frames ({dur:.2f}s) from '{in_video}' to {args.width}x{args.height}...")
    processed = [
        preprocess_and_sharpen(
            raw_rgb[min(int((i / float(args.fps)) * src_fps), len(raw_rgb) - 1)],
            args.width,
            args.height,
        )
        for i in range(n_out)
    ]

    print(f"Computing 255-color 24-bit RGB palette across sharpened {args.width}x{args.height} frames...")
    pal_rgb = build_global_palette(processed, n_colors=255)
    indexed = map_frames_to_palette(processed, pal_rgb)

    max_bytes = int(args.max_mb * 1024 * 1024)
    chosen = None
    for tol in (6, 8, 10, 12, 14, 16, 19, 22):
        runs_u16, offsets = encode_temporal_rle(indexed, processed, pal_rgb, diff_tol=tol)
        payload = len(runs_u16) * 2 + len(offsets) * 4 + 255 * 4
        print(f"  [diff_tol={tol:2d}] runs={len(runs_u16):,}, payload={payload / (1024 * 1024):.2f} MB")
        chosen = (runs_u16, offsets, payload, tol)
        if payload <= max_bytes:
            break

    runs_u16, offsets, payload, final_tol = chosen
    write_gop_header(out_path, pal_rgb, runs_u16, offsets, args.width, args.height, args.fps)
    print(
        f"Saved {out_path} (tol={final_tol}, ~{payload / (1024 * 1024):.2f} MB compiled .rdata, "
        f"{os.path.getsize(out_path) / (1024 * 1024):.2f} MB C source)"
    )


if __name__ == "__main__":
    main()
