#!/usr/bin/env python3
"""
Live 24 FPS ANSI TrueColor preview of clip.mp4 in Windows Terminal using half-blocks (▀).

Controls during playback:
  Space / Esc / Enter / Q : Quit immediately
  R                       : Restart from frame 0
"""

import argparse
import ctypes
import os
import shutil
import sys
import time
import cv2
import numpy as np

if os.name == "nt":
    import msvcrt
else:
    msvcrt = None


def enable_windows_vt_mode():
    if os.name != "nt":
        return
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
    mode = ctypes.c_uint32()
    if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
        # ENABLE_PROCESSED_OUTPUT (0x1) | ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x4)
        kernel32.SetConsoleMode(handle, mode.value | 0x0005)


def render_halfblock_ansi(rgb_even: np.ndarray) -> str:
    """
    Convert an (H, W, 3) uint8 RGB image (with even H) into a single ANSI TrueColor
    string using '▀' (top pixel = fg 38;2;r;g;b, bottom pixel = bg 48;2;r;g;b).
    Emits escape sequences only when fg or bg color changes along a row.
    """
    h, w, _ = rgb_even.shape
    rows_out = []

    top_rows = rgb_even[0::2]
    bot_rows = rgb_even[1::2]
    char_h = h // 2

    for y in range(char_h):
        t_row = top_rows[y]
        b_row = bot_rows[y]
        parts = []
        last_fg = None
        last_bg = None

        t_list = t_row.tolist()
        b_list = b_row.tolist()

        for x in range(w):
            fg = tuple(t_list[x])
            bg = tuple(b_list[x])
            if fg != last_fg and bg != last_bg:
                parts.append(f"\x1b[38;2;{fg[0]};{fg[1]};{fg[2]};48;2;{bg[0]};{bg[1]};{bg[2]}m▀")
                last_fg = fg
                last_bg = bg
            elif fg != last_fg:
                parts.append(f"\x1b[38;2;{fg[0]};{fg[1]};{fg[2]}m▀")
                last_fg = fg
            elif bg != last_bg:
                parts.append(f"\x1b[48;2;{bg[0]};{bg[1]};{bg[2]}m▀")
                last_bg = bg
            else:
                parts.append("▀")
        parts.append("\x1b[0m")
        rows_out.append("".join(parts))

    return rows_out


def poll_key():
    if msvcrt and msvcrt.kbhit():
        ch = msvcrt.getwch()
        if ch in ("\x00", "\xe0") and msvcrt.kbhit():
            msvcrt.getwch()
            return "OTHER"
        return ch
    return None


def main():
    parser = argparse.ArgumentParser(description="Play a video clip in Windows Terminal via ANSI TrueColor half-blocks.")
    parser.add_argument("video", nargs="?", default=None, help="Input video file (default: clip.mp4)")
    parser.add_argument("-i", "--input", dest="input_opt", default=None, help="Path to MP4 clip")
    parser.add_argument("--max-width", type=int, default=320, help="Max horizontal block resolution (default: 320)")
    parser.add_argument("--frames", type=int, default=0, help="Optional frame limit for quick smoke test (0 = full clip)")
    args = parser.parse_args()
    video_path = args.video or args.input_opt or "clip.mp4"

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", newline="")
    enable_windows_vt_mode()

    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"Error: cannot open {video_path}", file=sys.stderr)
        return 1

    fps = cap.get(cv2.CAP_PROP_FPS) or 24.0
    raw_frames = []
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        raw_frames.append(frame[:, :, ::-1])  # BGR -> RGB
        if args.frames > 0 and len(raw_frames) >= args.frames:
            break
    cap.release()

    if not raw_frames:
        print("Error: clip has 0 frames", file=sys.stderr)
        return 1

    total = len(raw_frames)
    src_h, src_w, _ = raw_frames[0].shape
    aspect = src_w / float(src_h)
    frame_period = 1.0 / fps

    # Switch to alternate screen buffer, clear, hide cursor
    sys.stdout.write("\x1b[?1049h\x1b[2J\x1b[?25l")
    sys.stdout.flush()

    try:
        idx = 0
        start_wall = time.perf_counter()
        last_term_size = None

        while idx < total:
            k = poll_key()
            if k is not None:
                if k in ("r", "R"):
                    idx = 0
                    start_wall = time.perf_counter()
                    sys.stdout.write("\x1b[2J")
                    continue
                break

            term = shutil.get_terminal_size((160, 50))
            if term != last_term_size:
                sys.stdout.write("\x1b[2J")
                last_term_size = term

            avail_cols = max(40, min(args.max_width, term.columns - 2))
            avail_px_h = max(24, (term.lines - 2) * 2)

            target_w = avail_cols
            target_px_h = int(round(target_w / aspect))
            if target_px_h > avail_px_h:
                target_px_h = avail_px_h
                target_w = max(40, int(round(target_px_h * aspect)))
            target_px_h = max(2, (target_px_h // 2) * 2)  # Must be even for '▀' pairs
            char_rows = target_px_h // 2

            pad_x = max(1, (term.columns - target_w) // 2 + 1)
            pad_y = max(1, (term.lines - 1 - char_rows) // 2 + 1)

            small = cv2.resize(raw_frames[idx], (target_w, target_px_h), interpolation=cv2.INTER_AREA).astype(np.float32)
            contrasted = np.clip((small - 124.0) * 1.09 + 126.0, 0.0, 255.0)
            blurred = cv2.GaussianBlur(contrasted, (0, 0), sigmaX=0.85)
            detail = contrasted - blurred
            resized = np.clip(contrasted + detail * (0.60 * (np.abs(detail) >= 3.5)), 0.0, 255.0).astype(np.uint8)
            ansi_lines = render_halfblock_ansi(resized)

            out_chunks = []
            for r_i, line in enumerate(ansi_lines):
                out_chunks.append(f"\x1b[{pad_y + r_i};{pad_x}H{line}")

            elapsed = time.perf_counter() - start_wall
            cur_sec = idx / fps
            tot_sec = total / fps
            status = (
                f"\x1b[{term.lines};1H\x1b[38;2;180;180;180;48;2;0;0;0m"
                f" UEFI GOP Preview [{target_w}x{target_px_h} TrueColor ▀] | "
                f"{cur_sec:05.2f}s / {tot_sec:05.2f}s (Frame {idx + 1}/{total} @ {fps:.0f} FPS) | "
                f"[Space/Esc/Q] Exit  [R] Replay\x1b[K\x1b[0m"
            )
            out_chunks.append(status)

            sys.stdout.write("".join(out_chunks))
            sys.stdout.flush()

            idx += 1
            next_due = start_wall + idx * frame_period
            now = time.perf_counter()
            if next_due > now:
                time.sleep(next_due - now)
            elif now - next_due > 0.25:
                # Re-sync clock if terminal window was dragged/paused
                start_wall = now - idx * frame_period

    finally:
        sys.stdout.write("\x1b[0m\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
