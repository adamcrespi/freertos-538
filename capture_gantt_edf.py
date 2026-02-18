#!/usr/bin/env python3
"""
EDF Schedule Capture & Gantt Chart Generator
Captures GPIO signals from FreeRTOS EDF tasks via Analog Discovery 2
and generates a Gantt chart with deadline markers and release arrows.

Usage:
    python3 capture_gantt_edf.py                    # capture live from AD2
    python3 capture_gantt_edf.py --from-csv data.csv # replay from saved capture
    python3 capture_gantt_edf.py --save-csv data.csv # capture and save raw data
"""

import time
import argparse
import csv
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.lines import Line2D

# ── Task Set Configuration ─────────────────────────────────────
# Define your EDF task set here. Modify these for each test case.
# Times are in seconds.
#
# For the Q&A slides test case (times in ticks, 1 tick = 1ms):
#   τ1: C=2, D=4, T=6  → C=0.002, D=0.004, T=0.006
#
# For the LED test (current demo, times in seconds):
#   τ1: period=0.5 (250ms on, 250ms off)
#   τ2: period=1.0
#   τ3: period=2.0

TASK_SET = {
    0: {
        "name": "Red (τ1)",
        "color": "#e74c3c",
        "gpio": "GP16",
        "period": 0.4,
        "deadline": 0.2,
        "wcet": 0.08,
    },
    1: {
        "name": "Yellow (τ2)",
        "color": "#f1c40f",
        "gpio": "GP17",
        "period": 0.8,
        "deadline": 0.4,
        "wcet": 0.15,
    },
    2: {
        "name": "Green (τ3)",
        "color": "#2ecc71",
        "gpio": "GP18",
        "period": 1.6,
        "deadline": 1.0,
        "wcet": 0.4,
    },
}

# ── Capture Configuration ──────────────────────────────────────
SAMPLE_RATE = 10000.0       # Hz
CAPTURE_SECONDS = 5.0       # seconds
NUM_SAMPLES = int(SAMPLE_RATE * CAPTURE_SECONDS)

# ── Gantt Chart Configuration ──────────────────────────────────
SHOW_RELEASES = True        # Show release arrows (↑) at job start times
SHOW_DEADLINES = True       # Show deadline markers (↓) 
SHOW_DEADLINE_MISSES = True # Highlight deadline misses in red
ZOOM_START = 0.0            # Zoom into a time window (seconds)
ZOOM_END = CAPTURE_SECONDS  # Set both to 0/CAPTURE_SECONDS for full view


# ── Capture from AD2 ──────────────────────────────────────────
def capture_signals():
    """Capture digital signals from AD2 and return raw samples."""
    from pydwf import DwfLibrary, DwfAcquisitionMode
    from pydwf.utilities import openDwfDevice

    dwf = DwfLibrary()

    print("Opening Analog Discovery 2...")
    with openDwfDevice(dwf) as device:
        din = device.digitalIn

        print(f"Capturing {CAPTURE_SECONDS}s at {SAMPLE_RATE} Hz...")
        print(f"Monitoring channels: {', '.join(t['name'] for t in TASK_SET.values())}")

        din.sampleFormatSet(16)
        internal_freq = 100e6
        divider = int(internal_freq / SAMPLE_RATE)
        din.dividerSet(divider)
        din.acquisitionModeSet(DwfAcquisitionMode.Record)
        din.configure(False, True)

        print("Recording...", end="", flush=True)

        all_samples = []
        total_captured = 0

        while total_captured < NUM_SAMPLES:
            time.sleep(0.05)
            status = din.status(True)
            available, lost, corrupted = din.statusRecord()

            if available > 0:
                data = din.statusData(available)
                all_samples.extend(data.tolist())
                total_captured += available

            if lost > 0:
                print(f"\n  WARNING: {lost} samples lost!", flush=True)
            if corrupted > 0:
                print(f"\n  WARNING: {corrupted} samples corrupted!", flush=True)

            pct = min(100, int(100 * total_captured / NUM_SAMPLES))
            print(f"\rRecording... {pct}%", end="", flush=True)

        print(f"\rRecording... done! ({len(all_samples)} samples captured)")

    return all_samples


# ── CSV Save/Load ──────────────────────────────────────────────
def save_csv(samples, path):
    """Save raw samples to CSV for later replay."""
    with open(path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["sample_index", "value"])
        for i, s in enumerate(samples):
            writer.writerow([i, s])
    print(f"Saved {len(samples)} samples to {path}")


def load_csv(path):
    """Load raw samples from CSV."""
    samples = []
    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader)  # skip header
        for row in reader:
            samples.append(int(row[1]))
    print(f"Loaded {len(samples)} samples from {path}")
    return samples


# ── Signal Processing ──────────────────────────────────────────
def extract_edges(samples):
    """Extract rising/falling edges per channel."""
    edges = {ch: [] for ch in TASK_SET}
    dt = 1.0 / SAMPLE_RATE

    for i in range(1, len(samples)):
        for ch in TASK_SET:
            mask = 1 << ch
            prev = (samples[i-1] & mask) != 0
            curr = (samples[i] & mask) != 0

            if curr and not prev:
                edges[ch].append(("rise", i * dt))
            elif not curr and prev:
                edges[ch].append(("fall", i * dt))

    return edges


def build_intervals(edges, total_time):
    """Convert edges to (start, end) execution intervals."""
    intervals = {ch: [] for ch in TASK_SET}

    for ch in TASK_SET:
        ch_edges = edges[ch]
        i = 0
        while i < len(ch_edges):
            if ch_edges[i][0] == "rise":
                start = ch_edges[i][1]
                end = None
                for j in range(i + 1, len(ch_edges)):
                    if ch_edges[j][0] == "fall":
                        end = ch_edges[j][1]
                        i = j + 1
                        break
                if end is None:
                    end = total_time
                    i = len(ch_edges)
                intervals[ch].append((start, end))
            else:
                i += 1

    return intervals


def compute_deadlines(total_time, edges):
    """Compute deadlines aligned to actual first edge."""
    deadlines = {ch: [] for ch in TASK_SET}

    for ch, task in TASK_SET.items():
        T = task["period"]
        D = task["deadline"]
        first_release = 0.0
        for edge_type, t in edges.get(ch, []):
            if edge_type == "rise":
                first_release = t
                break

        t = first_release
        while t < total_time + T:
            abs_deadline = t + D
            deadlines[ch].append(abs_deadline)
            t += T

    return deadlines


def compute_releases(total_time, edges):
    """Compute release times aligned to actual first edge."""
    releases = {ch: [] for ch in TASK_SET}

    for ch, task in TASK_SET.items():
        T = task["period"]
        # Use first rising edge as actual first release
        first_release = 0.0
        for edge_type, t in edges.get(ch, []):
            if edge_type == "rise":
                first_release = t
                break

        t = first_release
        while t <= total_time:
            releases[ch].append(t)
            t += T

    return releases


def detect_deadline_misses(intervals, deadlines):
    """Check if any job was still executing past its deadline."""
    misses = {ch: [] for ch in TASK_SET}

    for ch in TASK_SET:
        dl_list = deadlines[ch]
        intv_list = intervals[ch]

        for dl in dl_list:
            # Check if any execution interval overlaps past this deadline
            for (start, end) in intv_list:
                if start < dl and end > dl:
                    misses[ch].append(dl)
                    break

    return misses


# ── Gantt Chart ────────────────────────────────────────────────
def plot_gantt(intervals, deadlines, releases, misses,
               save_path="edf_schedule.png"):
    """Generate EDF Gantt chart with deadline markers and releases."""

    n_tasks = len(TASK_SET)
    fig, ax = plt.subplots(figsize=(16, 2 + n_tasks * 1.2))

    y_labels = []
    y_pos = []

    for idx, ch in enumerate(sorted(TASK_SET.keys())):
        task = TASK_SET[ch]
        y = n_tasks - idx - 1
        y_labels.append(f"{task['name']}\nC={task['wcet']}, D={task['deadline']}, T={task['period']}")
        y_pos.append(y)

        # Execution bars
        for (start, end) in intervals[ch]:
            if end < ZOOM_START or start > ZOOM_END:
                continue
            duration = end - start
            ax.barh(y, duration, left=start, height=0.5,
                    color=task["color"], edgecolor="black", linewidth=0.5,
                    alpha=0.85)

        # Deadline markers (downward triangles)
        if SHOW_DEADLINES:
            for dl in deadlines[ch]:
                if ZOOM_START <= dl <= ZOOM_END:
                    is_miss = dl in misses.get(ch, [])
                    marker_color = "#ff0000" if is_miss else "#333333"
                    marker_size = 10 if is_miss else 7
                    ax.plot(dl, y + 0.35, 'v',
                            color=marker_color, markersize=marker_size,
                            zorder=5)
                    if is_miss and SHOW_DEADLINE_MISSES:
                        ax.axvline(x=dl, ymin=(y - 0.3) / n_tasks,
                                   ymax=(y + 0.7) / n_tasks,
                                   color='red', linestyle='--',
                                   linewidth=1.5, alpha=0.5)

        # Release markers (upward triangles)
        if SHOW_RELEASES:
            for rel in releases[ch]:
                if ZOOM_START <= rel <= ZOOM_END:
                    ax.plot(rel, y - 0.35, '^',
                            color="#333333", markersize=6, zorder=5)

    # Axes
    ax.set_yticks(y_pos)
    ax.set_yticklabels(y_labels, fontsize=9, family='monospace')
    ax.set_xlabel("Time (seconds)", fontsize=12)
    ax.set_xlim(ZOOM_START, ZOOM_END)
    ax.set_ylim(-0.7, n_tasks - 0.3)
    ax.grid(axis="x", alpha=0.3, linestyle='-')
    ax.set_axisbelow(True)

    # Title with utilization
    total_util = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    ax.set_title(
        f"EDF Schedule — {n_tasks} tasks, U = {total_util:.3f}"
        f" ({'schedulable' if total_util <= 1.0 else 'OVERLOADED'})",
        fontsize=13, fontweight="bold"
    )

    # Legend
    legend_elements = []
    for ch in sorted(TASK_SET.keys()):
        legend_elements.append(
            mpatches.Patch(color=TASK_SET[ch]["color"], label=TASK_SET[ch]["name"])
        )
    legend_elements.append(
        Line2D([0], [0], marker='^', color='w', markerfacecolor='#333',
               markersize=8, label='Release')
    )
    legend_elements.append(
        Line2D([0], [0], marker='v', color='w', markerfacecolor='#333',
               markersize=8, label='Deadline')
    )
    legend_elements.append(
        Line2D([0], [0], marker='v', color='w', markerfacecolor='red',
               markersize=8, label='Deadline Miss')
    )
    ax.legend(handles=legend_elements, loc="upper right", fontsize=8,
              ncol=2, framealpha=0.9)

    plt.tight_layout()
    plt.savefig(save_path, dpi=150, bbox_inches='tight')
    print(f"\nGantt chart saved to: {save_path}")
    plt.show()


# ── Summary Stats ──────────────────────────────────────────────
def print_summary(intervals, deadlines, misses):
    """Print summary statistics."""

    print("\n" + "=" * 60)
    print("EDF SCHEDULE ANALYSIS")
    print("=" * 60)

    total_util = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    print(f"\nTotal utilization: {total_util:.4f}")
    print(f"EDF schedulable (LL bound): {'YES' if total_util <= 1.0 else 'NO'}")

    print(f"\n{'Task':<20} {'Jobs':>6} {'Deadlines':>10} {'Misses':>8}")
    print("-" * 50)
    for ch in sorted(TASK_SET.keys()):
        task = TASK_SET[ch]
        n_jobs = len(intervals[ch])
        n_dl = len(deadlines[ch])
        n_miss = len(misses[ch])
        miss_str = f"{n_miss}" if n_miss == 0 else f"{n_miss} !!!"
        print(f"{task['name']:<20} {n_jobs:>6} {n_dl:>10} {miss_str:>8}")

    total_misses = sum(len(m) for m in misses.values())
    if total_misses == 0:
        print("\n✓ No deadline misses detected!")
    else:
        print(f"\n✗ {total_misses} DEADLINE MISS(ES) DETECTED!")


# ── Main ───────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="EDF Schedule Capture & Analysis")
    parser.add_argument("--from-csv", type=str, help="Load samples from CSV instead of capturing")
    parser.add_argument("--save-csv", type=str, help="Save captured samples to CSV")
    parser.add_argument("--output", type=str, default="edf_schedule.png",
                        help="Output image path (default: edf_schedule.png)")
    parser.add_argument("--zoom-start", type=float, default=None,
                        help="Zoom window start time (seconds)")
    parser.add_argument("--zoom-end", type=float, default=None,
                        help="Zoom window end time (seconds)")
    args = parser.parse_args()

    global ZOOM_START, ZOOM_END
    if args.zoom_start is not None:
        ZOOM_START = args.zoom_start
    if args.zoom_end is not None:
        ZOOM_END = args.zoom_end

    print("=" * 60)
    print("EDF Schedule Capture & Analysis Tool")
    print("=" * 60)

    # Print task set
    print(f"\n{'Task':<20} {'C':>8} {'D':>8} {'T':>8} {'U':>8}")
    print("-" * 55)
    for ch in sorted(TASK_SET.keys()):
        t = TASK_SET[ch]
        u = t["wcet"] / t["period"]
        print(f"{t['name']:<20} {t['wcet']:>8.4f} {t['deadline']:>8.4f} {t['period']:>8.4f} {u:>8.4f}")
    total_u = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    print(f"{'Total U':<20} {'':>8} {'':>8} {'':>8} {total_u:>8.4f}")

    # Capture or load
    if args.from_csv:
        samples = load_csv(args.from_csv)
    else:
        samples = capture_signals()

    if args.save_csv:
        save_csv(samples, args.save_csv)

    # Process
    print("\nProcessing edges...")
    edges = extract_edges(samples)
    for ch in TASK_SET:
        n = len(edges[ch])
        print(f"  {TASK_SET[ch]['name']}: {n} edges detected")

    total_time = len(samples) / SAMPLE_RATE
    intervals = build_intervals(edges, total_time)

    # Compute theoretical deadlines and releases
    deadlines = compute_deadlines(total_time, edges)
    releases = compute_releases(total_time, edges)

    # Detect deadline misses
    misses = detect_deadline_misses(intervals, deadlines)

    # Summary
    print_summary(intervals, deadlines, misses)

    # Plot
    print("\nGenerating Gantt chart...")
    plot_gantt(intervals, deadlines, releases, misses, save_path=args.output)


if __name__ == "__main__":
    main()
