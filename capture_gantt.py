#!/usr/bin/env python3
"""
FreeRTOS Task Schedule Capture & Gantt Chart Generator
Uses Analog Discovery 2 to capture GPIO signals from Pico tasks
and generates a Gantt chart showing task execution over time.
"""

import time
import sys
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from ctypes import c_double, c_int, c_byte, byref, create_string_buffer
import pydwf

# ── Configuration ──────────────────────────────────────────────
CHANNELS = {
    0: {"name": "Red (τ1)",   "color": "#e74c3c", "gpio": "GP16"},
    1: {"name": "Yellow (τ2)", "color": "#f1c40f", "gpio": "GP17"},
    2: {"name": "Green (τ3)",  "color": "#2ecc71", "gpio": "GP18"},
}

SAMPLE_RATE = 10000.0   # 10 kHz — plenty for ms-level task switching
CAPTURE_SECONDS = 5.0   # How long to capture
NUM_SAMPLES = int(SAMPLE_RATE * CAPTURE_SECONDS)

# ── Capture ────────────────────────────────────────────────────
def capture_signals():
    """Capture digital signals from AD2 and return edge events."""

    lib = pydwf.DwfLibrary()

    print("Opening Analog Discovery 2...")
    device = pydwf.DwfDevice(lib)

    print(f"Capturing {CAPTURE_SECONDS}s at {SAMPLE_RATE} Hz...")
    print(f"Monitoring channels: {', '.join(ch['name'] for ch in CHANNELS.values())}")

    # Configure digital input
    din = device.digitalIn

    # Set clock divider for desired sample rate
    # AD2 internal clock is 100 MHz
    internal_freq = 100e6
    divider = int(internal_freq / SAMPLE_RATE)
    din.dividerSet(divider)

    # Set buffer size
    buf_size = min(NUM_SAMPLES, 16384)  # AD2 buffer limit
    din.bufferSizeSet(buf_size)

    # Set acquisition mode to record (for longer captures)
    din.acquisitionModeSet(pydwf.DwfAcquisitionMode.Record)

    # Configure which channels to sample (DIO 0, 1, 2)
    din.configure(False, True)  # start acquisition

    print("Recording...", end="", flush=True)

    all_samples = []
    total_captured = 0

    while total_captured < NUM_SAMPLES:
        time.sleep(0.1)

        record_status = din.status(True)
        available, lost, corrupted = din.statusRecord()

        if available > 0:
            data = din.statusData(available)
            all_samples.extend(data)
            total_captured += available

        if lost > 0:
            print(f"\n  WARNING: {lost} samples lost!", flush=True)
        if corrupted > 0:
            print(f"\n  WARNING: {corrupted} samples corrupted!", flush=True)

        # Progress
        pct = min(100, int(100 * total_captured / NUM_SAMPLES))
        print(f"\rRecording... {pct}%", end="", flush=True)

    print(f"\rRecording... done! ({len(all_samples)} samples captured)")

    device.close()

    return all_samples


def extract_edges(samples):
    """Extract rising/falling edges per channel from raw samples."""

    edges = {ch: [] for ch in CHANNELS}
    dt = 1.0 / SAMPLE_RATE

    for i in range(1, len(samples)):
        for ch in CHANNELS:
            mask = 1 << ch
            prev = (samples[i-1] & mask) != 0
            curr = (samples[i] & mask) != 0

            if curr and not prev:
                edges[ch].append(("rise", i * dt))
            elif not curr and prev:
                edges[ch].append(("fall", i * dt))

    return edges


def build_intervals(edges):
    """Convert edges to (start, end) intervals for Gantt chart."""

    intervals = {ch: [] for ch in CHANNELS}

    for ch in CHANNELS:
        ch_edges = edges[ch]
        i = 0
        while i < len(ch_edges):
            if ch_edges[i][0] == "rise":
                start = ch_edges[i][1]
                # Find matching fall
                end = None
                for j in range(i + 1, len(ch_edges)):
                    if ch_edges[j][0] == "fall":
                        end = ch_edges[j][1]
                        i = j + 1
                        break
                if end is None:
                    # Signal was still high at end of capture
                    end = len(edges) / SAMPLE_RATE
                    i = len(ch_edges)
                intervals[ch].append((start, end))
            else:
                i += 1

    return intervals


# ── Gantt Chart ────────────────────────────────────────────────
def plot_gantt(intervals, save_path="schedule_gantt.png"):
    """Generate a Gantt chart from task execution intervals."""

    fig, ax = plt.subplots(figsize=(14, 4))

    y_labels = []
    y_pos = []

    for idx, ch in enumerate(sorted(CHANNELS.keys())):
        cfg = CHANNELS[ch]
        y = len(CHANNELS) - idx - 1
        y_labels.append(f"{cfg['name']}\n({cfg['gpio']})")
        y_pos.append(y)

        for (start, end) in intervals[ch]:
            duration = end - start
            ax.barh(y, duration, left=start, height=0.6,
                    color=cfg["color"], edgecolor="black", linewidth=0.5,
                    alpha=0.85)

    ax.set_yticks(y_pos)
    ax.set_yticklabels(y_labels, fontsize=10)
    ax.set_xlabel("Time (seconds)", fontsize=12)
    ax.set_title("FreeRTOS Task Execution — Captured via Analog Discovery 2", fontsize=13, fontweight="bold")
    ax.set_xlim(0, CAPTURE_SECONDS)
    ax.grid(axis="x", alpha=0.3)
    ax.set_axisbelow(True)

    # Legend
    patches = [mpatches.Patch(color=CHANNELS[ch]["color"], label=CHANNELS[ch]["name"])
               for ch in sorted(CHANNELS.keys())]
    ax.legend(handles=patches, loc="upper right", fontsize=9)

    plt.tight_layout()
    plt.savefig(save_path, dpi=150)
    print(f"\nGantt chart saved to: {save_path}")
    plt.show()


# ── Main ───────────────────────────────────────────────────────
def main():
    print("=" * 50)
    print("FreeRTOS Schedule Capture Tool")
    print("=" * 50)

    # Step 1: Capture
    samples = capture_signals()

    # Step 2: Extract edges
    print("\nProcessing edges...")
    edges = extract_edges(samples)
    for ch in CHANNELS:
        n = len(edges[ch])
        print(f"  {CHANNELS[ch]['name']}: {n} edges detected")

    # Step 3: Build intervals
    intervals = build_intervals(edges)
    for ch in CHANNELS:
        n = len(intervals[ch])
        print(f"  {CHANNELS[ch]['name']}: {n} ON intervals")

    # Step 4: Plot
    print("\nGenerating Gantt chart...")
    plot_gantt(intervals)


if __name__ == "__main__":
    main()
