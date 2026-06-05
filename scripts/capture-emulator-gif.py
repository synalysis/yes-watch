#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EMULATOR_INFO = Path("/tmp/pb-emulator.json")


def qemu_monitor_command(port, command, timeout=1.0):
    with socket.create_connection(("127.0.0.1", int(port)), timeout=timeout) as sock:
        sock.sendall((command + "\n").encode("utf-8"))
        try:
            sock.recv(4096)
        except socket.timeout:
            pass


def get_monitor_port(platform):
    if not EMULATOR_INFO.exists():
        raise SystemExit(f"Emulator metadata not found: {EMULATOR_INFO}")

    data = json.loads(EMULATOR_INFO.read_text(encoding="utf-8"))
    platform_info = data.get(platform)
    if not platform_info:
        raise SystemExit(f"No emulator info for platform: {platform}")

    for version_info in platform_info.values():
        qemu = version_info.get("qemu", {})
        pid = qemu.get("pid")
        monitor = qemu.get("monitor")
        if pid and monitor and Path(f"/proc/{pid}").exists():
            return int(monitor)

    raise SystemExit(f"No running emulator with monitor port found for: {platform}")


def run_ffmpeg(args, step_name):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{step_name} failed: {result.stderr.strip()}")


def wait_for_cycle_boundary(cycle_sec):
    if cycle_sec <= 0:
        return
    now = time.time()
    rem = now % cycle_sec
    if rem > 0.05:
        wait = cycle_sec - rem
        print(f"Waiting {wait:.1f}s for complication cycle boundary...")
        time.sleep(wait)


def capture_gif(platform, output, delay_sec, duration_sec, fps, cycle_sec):
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is required for GIF capture")

    monitor_port = get_monitor_port(platform)
    qemu_monitor_command(monitor_port, "sendkey left")

    if delay_sec > 0:
        print(f"Waiting {delay_sec}s after app start before GIF capture...")
        time.sleep(delay_sec)

    wait_for_cycle_boundary(cycle_sec)

    qemu_monitor_command(monitor_port, "sendkey left")
    time.sleep(0.3)
    print(f"Recording {duration_sec:.0f}s GIF to capture multiple complication changes...")

    temp_dir = tempfile.mkdtemp(prefix="yes-watch-gif-")
    frame_paths = []
    try:
        frame_interval = 1.0 / float(fps)
        capture_start = time.perf_counter()
        next_capture = capture_start
        frame_index = 0

        while time.perf_counter() - capture_start < duration_sec:
            now = time.perf_counter()
            if now < next_capture:
                time.sleep(next_capture - now)

            ppm_path = os.path.join(temp_dir, f"frame_{frame_index:05d}.ppm")
            try:
                qemu_monitor_command(monitor_port, f"screendump {ppm_path}", timeout=2.0)
            except OSError as exc:
                print(f"screendump failed for frame {frame_index}: {exc}", file=sys.stderr)
                next_capture += frame_interval
                frame_index += 1
                continue

            deadline = time.time() + 0.5
            while time.time() < deadline:
                if os.path.exists(ppm_path) and os.path.getsize(ppm_path) > 0:
                    frame_paths.append(ppm_path)
                    break
                time.sleep(0.005)

            if frame_index % max(1, int(fps)) == 0:
                try:
                    qemu_monitor_command(monitor_port, "sendkey left", timeout=0.5)
                except OSError:
                    pass

            next_capture += frame_interval
            frame_index += 1

        if not frame_paths:
            raise SystemExit("No frames captured for GIF")

        seq_dir = os.path.join(temp_dir, "seq")
        os.makedirs(seq_dir, exist_ok=True)
        for idx, src in enumerate(frame_paths):
            os.rename(src, os.path.join(seq_dir, f"frame_{idx:05d}.ppm"))

        output_path = os.path.abspath(output)
        output_dir = os.path.dirname(output_path)
        if output_dir:
            os.makedirs(output_dir, exist_ok=True)

        seq_pattern = os.path.join(seq_dir, "frame_%05d.ppm")
        palette = os.path.join(temp_dir, "palette.png")

        print(f"Processing {len(frame_paths)} frames into GIF...")
        run_ffmpeg([
            "ffmpeg", "-framerate", str(fps), "-i", seq_pattern,
            "-vf", "palettegen=max_colors=64:reserve_transparent=0",
            "-y", palette, "-v", "error",
        ], "Palette generation")

        run_ffmpeg([
            "ffmpeg", "-framerate", str(fps), "-i", seq_pattern,
            "-i", palette,
            "-filter_complex", "[0:v]mpdecimate[v];[v][1:v]paletteuse=dither=none",
            "-y", output_path, "-v", "error",
        ], "GIF encoding")

        print(output_path)
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Capture a Pebble emulator GIF via QEMU monitor screendumps.")
    parser.add_argument("--platform", required=True, help="Emulator platform, e.g. basalt")
    parser.add_argument("--output", required=True, help="Output GIF path")
    parser.add_argument("--delay", type=float, default=5.0, help="Seconds to wait after app start before recording")
    parser.add_argument("--duration", type=float, default=None, help="Recording duration in seconds")
    parser.add_argument("--cycle-seconds", type=float, default=5.0,
                        help="Corner complication alternation period (default: 5)")
    parser.add_argument("--cycles", type=int, default=5,
                        help="Number of complication alternations to record (default: 5)")
    parser.add_argument("--fps", type=int, default=15, help="Capture frame rate")
    args = parser.parse_args()

    duration_sec = args.duration if args.duration is not None else args.cycles * args.cycle_seconds
    if duration_sec <= 0:
        raise SystemExit("Recording duration must be positive")
    if args.cycle_seconds <= 0:
        raise SystemExit("--cycle-seconds must be positive")
    if args.cycles <= 0:
        raise SystemExit("--cycles must be positive")
    if args.fps <= 0:
        raise SystemExit("--fps must be positive")

    capture_gif(args.platform, args.output, args.delay, duration_sec, args.fps, args.cycle_seconds)


if __name__ == "__main__":
    main()
