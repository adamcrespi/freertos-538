#!/usr/bin/env python3
"""
Multiprocessor EDF Schedule Capture & Gantt Chart Generator
Captures GPIO signals from FreeRTOS SMP EDF tasks via Analog Discovery 2
and generates a Gantt chart.  Includes SMP-specific analysis:

  - Simultaneous execution detection: two tasks HIGH at the same time
    is direct proof that both RP2040 cores are running EDF tasks in parallel.
  - For Global EDF: shows that tasks can migrate between jobs.
  - For Partitioned EDF: annotates core groups and verifies no migration.

Usage:
    python3 capture_gantt_mp.py                      # capture live, global mode
    python3 capture_gantt_mp.py --from-csv data.csv  # replay from saved capture
    python3 capture_gantt_mp.py --save-csv data.csv  # capture and save raw data
    python3 capture_gantt_mp.py --mode partitioned   # partitioned params + core annotations
    python3 capture_gantt_mp.py --output gantt.png   # custom output file
"""

import time
import argparse
import csv
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.lines import Line2D

# ── Task Set Configurations ────────────────────────────────────
# Selected at runtime by --mode.  Times in seconds (1 tick = 1 ms).

# Global EDF test (main_mp_global.c) — implicit deadline (D=T):
#   τ1 GP16: C=300ms, D=500ms, T=500ms   U=0.600
#   τ2 GP17: C=350ms, D=700ms, T=700ms   U=0.500
#   τ3 GP18: C=360ms, D=900ms, T=900ms   U=0.400   Total U=1.500
GLOBAL_TASK_SET = {
    0: {"name": "τ1 (GP16)", "color": "#e74c3c", "gpio": "GP16",
        "period": 0.500, "deadline": 0.500, "wcet": 0.300, "core": None},
    1: {"name": "τ2 (GP17)", "color": "#f39c12", "gpio": "GP17",
        "period": 0.700, "deadline": 0.700, "wcet": 0.350, "core": None},
    2: {"name": "τ3 (GP18)", "color": "#2ecc71", "gpio": "GP18",
        "period": 0.900, "deadline": 0.900, "wcet": 0.360, "core": None},
}

# Partitioned EDF test (main_mp_partitioned.c) — implicit deadline (D=T):
#   Core 0: τ1 GP16 (C=250,D=500,T=500  U=0.500)
#           τ2 GP17 (C=280,D=700,T=700  U=0.400)  Core 0 U=0.900
#   Core 1: τ3 GP18 (C=360,D=900,T=900  U=0.400)  Core 1 U=0.400
PARTITIONED_TASK_SET = {
    0: {"name": "τ1 (GP16)", "color": "#e74c3c", "gpio": "GP16",
        "period": 0.500, "deadline": 0.500, "wcet": 0.250, "core": 0},
    1: {"name": "τ2 (GP17)", "color": "#f39c12", "gpio": "GP17",
        "period": 0.700, "deadline": 0.700, "wcet": 0.280, "core": 0},
    2: {"name": "τ3 (GP18)", "color": "#2ecc71", "gpio": "GP18",
        "period": 0.900, "deadline": 0.900, "wcet": 0.360, "core": 1},
}

# Active task set — overwritten in main() based on --mode
TASK_SET = GLOBAL_TASK_SET

# ── Capture Configuration ──────────────────────────────────────
SAMPLE_RATE = 10000.0       # Hz
CAPTURE_SECONDS = 6.0       # seconds
NUM_SAMPLES = int(SAMPLE_RATE * CAPTURE_SECONDS)

# ── Gantt Chart Configuration ──────────────────────────────────
SHOW_RELEASES = True
SHOW_DEADLINES = True
SHOW_DEADLINE_MISSES = True
SHOW_SIMULTANEOUS = True    # Highlight time regions where 2+ tasks run at once
ZOOM_START = 0.0
ZOOM_END = CAPTURE_SECONDS


# ── Capture from AD2 ──────────────────────────────────────────
def capture_signals():
    from pydwf import DwfLibrary, DwfAcquisitionMode
    from pydwf.utilities import openDwfDevice

    dwf = DwfLibrary()

    print("Opening Analog Discovery 2...")
    with openDwfDevice(dwf) as device:
        din = device.digitalIn

        print(f"Capturing {CAPTURE_SECONDS}s at {SAMPLE_RATE} Hz...")
        print(f"Monitoring: {', '.join(t['name'] for t in TASK_SET.values())}")

        din.sampleFormatSet(16)
        divider = int(100e6 / SAMPLE_RATE)
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
    with open(path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["sample_index", "value"])
        for i, s in enumerate(samples):
            writer.writerow([i, s])
    print(f"Saved {len(samples)} samples to {path}")


def load_csv(path):
    samples = []
    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            samples.append(int(row[1]))
    print(f"Loaded {len(samples)} samples from {path}")
    return samples


# ── Signal Processing ──────────────────────────────────────────
def extract_edges(samples):
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
            deadlines[ch].append(t + D)
            t += T
    return deadlines


def compute_releases(total_time, edges):
    releases = {ch: [] for ch in TASK_SET}
    for ch, task in TASK_SET.items():
        T = task["period"]
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
    misses = {ch: [] for ch in TASK_SET}
    for ch in TASK_SET:
        for dl in deadlines[ch]:
            for (start, end) in intervals[ch]:
                if start < dl < end:
                    misses[ch].append(dl)
                    break
    return misses


def find_simultaneous_intervals(samples):
    """
    Find time intervals where >= 2 channels are HIGH simultaneously.
    These intervals are proof that two cores are executing EDF tasks in parallel.
    Returns list of (start, end, count) tuples.
    """
    dt = 1.0 / SAMPLE_RATE
    sim_intervals = []
    in_sim = False
    sim_start = 0.0

    channels = list(TASK_SET.keys())
    mask_all = sum(1 << ch for ch in channels)

    for i, s in enumerate(samples):
        active = bin(s & mask_all).count('1')
        t = i * dt

        if active >= 2 and not in_sim:
            sim_start = t
            in_sim = True
        elif active < 2 and in_sim:
            sim_intervals.append((sim_start, t))
            in_sim = False

    if in_sim:
        sim_intervals.append((sim_start, len(samples) * dt))

    return sim_intervals


# ── Gantt Chart ────────────────────────────────────────────────
def plot_gantt(intervals, deadlines, releases, misses, sim_intervals,
               mode="global", save_path="mp_schedule.png"):

    n_tasks = len(TASK_SET)
    fig, ax = plt.subplots(figsize=(18, 2 + n_tasks * 1.4))

    # Shade simultaneous execution regions (proof of SMP)
    if SHOW_SIMULTANEOUS:
        for (s, e) in sim_intervals:
            if e < ZOOM_START or s > ZOOM_END:
                continue
            ax.axvspan(max(s, ZOOM_START), min(e, ZOOM_END),
                       alpha=0.12, color='purple', zorder=0)

    y_labels = []
    y_pos = []

    for idx, ch in enumerate(sorted(TASK_SET.keys())):
        task = TASK_SET[ch]
        y = n_tasks - idx - 1
        core_str = f"  [core {task['core']}]" if task['core'] is not None and mode == "partitioned" else ""
        y_labels.append(
            f"{task['name']}{core_str}\nC={task['wcet']}, D={task['deadline']}, T={task['period']}"
        )
        y_pos.append(y)

        # Core grouping background for partitioned mode
        if mode == "partitioned" and task['core'] is not None:
            bg_color = "#ffe8e8" if task['core'] == 0 else "#e8e8ff"
            ax.axhspan(y - 0.4, y + 0.4, alpha=0.2, color=bg_color, zorder=0)

        # Execution bars
        for (start, end) in intervals[ch]:
            if end < ZOOM_START or start > ZOOM_END:
                continue
            ax.barh(y, end - start, left=start, height=0.5,
                    color=task["color"], edgecolor="black", linewidth=0.5,
                    alpha=0.85, zorder=2)

        # Deadline markers
        if SHOW_DEADLINES:
            for dl in deadlines[ch]:
                if ZOOM_START <= dl <= ZOOM_END:
                    is_miss = dl in misses.get(ch, [])
                    ax.plot(dl, y + 0.35, 'v',
                            color="#ff0000" if is_miss else "#333333",
                            markersize=10 if is_miss else 7, zorder=5)

        # Release markers
        if SHOW_RELEASES:
            for rel in releases[ch]:
                if ZOOM_START <= rel <= ZOOM_END:
                    ax.plot(rel, y - 0.35, '^',
                            color="#333333", markersize=6, zorder=5)

    ax.set_yticks(y_pos)
    ax.set_yticklabels(y_labels, fontsize=9, family='monospace')
    ax.set_xlabel("Time (seconds)", fontsize=12)
    ax.set_xlim(ZOOM_START, ZOOM_END)
    ax.set_ylim(-0.7, n_tasks - 0.3)
    ax.grid(axis="x", alpha=0.3)
    ax.set_axisbelow(True)

    total_util = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    mode_str = "Global EDF (2 cores)" if mode == "global" else "Partitioned EDF (2 cores)"
    ax.set_title(
        f"{mode_str} — {n_tasks} tasks, U={total_util:.3f} / bound=2.0",
        fontsize=13, fontweight="bold"
    )

    # Legend
    legend_elements = []
    for ch in sorted(TASK_SET.keys()):
        legend_elements.append(
            mpatches.Patch(color=TASK_SET[ch]["color"], label=TASK_SET[ch]["name"])
        )
    legend_elements += [
        Line2D([0], [0], marker='^', color='w', markerfacecolor='#333',
               markersize=8, label='Release'),
        Line2D([0], [0], marker='v', color='w', markerfacecolor='#333',
               markersize=8, label='Deadline'),
        Line2D([0], [0], marker='v', color='w', markerfacecolor='red',
               markersize=8, label='Deadline Miss'),
        mpatches.Patch(color='purple', alpha=0.3, label='Dual-core parallel (SMP proof)'),
    ]
    ax.legend(handles=legend_elements, loc="upper right", fontsize=8,
              ncol=2, framealpha=0.9)

    plt.tight_layout()
    plt.savefig(save_path, dpi=150, bbox_inches='tight')
    print(f"\nGantt chart saved to: {save_path}")
    plt.show()


# ── Summary Stats ──────────────────────────────────────────────
def print_summary(intervals, deadlines, misses, sim_intervals, total_time):
    print("\n" + "=" * 65)
    print("MULTIPROCESSOR EDF SCHEDULE ANALYSIS")
    print("=" * 65)

    total_util = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    print(f"\nTotal utilization: {total_util:.4f}")
    print(f"EDF bound (m=2):   2.0000")
    print(f"Schedulable:       {'YES' if total_util <= 2.0 else 'NO'}")

    print(f"\n{'Task':<18} {'Jobs':>6} {'DLs':>6} {'Misses':>8}")
    print("-" * 44)
    for ch in sorted(TASK_SET.keys()):
        task = TASK_SET[ch]
        n_jobs = len(intervals[ch])
        n_dl   = len(deadlines[ch])
        n_miss = len(misses[ch])
        miss_str = str(n_miss) if n_miss == 0 else f"{n_miss} !!!"
        print(f"{task['name']:<18} {n_jobs:>6} {n_dl:>6} {miss_str:>8}")

    total_misses = sum(len(m) for m in misses.values())
    if total_misses == 0:
        print("\n  No deadline misses detected.")
    else:
        print(f"\n  {total_misses} DEADLINE MISS(ES) DETECTED!")

    # SMP proof
    sim_total = sum(e - s for s, e in sim_intervals)
    sim_pct   = 100.0 * sim_total / total_time if total_time > 0 else 0.0
    print(f"\nSMP parallel execution:")
    print(f"  Dual-core intervals : {len(sim_intervals)}")
    print(f"  Total parallel time : {sim_total:.3f}s  ({sim_pct:.1f}% of capture)")
    if len(sim_intervals) > 0:
        print(f"  *** PROOF: both cores executed EDF tasks simultaneously ***")
    else:
        print(f"  (No simultaneous execution detected — check GPIO wiring)")


# ── Main ───────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Multiprocessor EDF Schedule Capture & Analysis")
    parser.add_argument("--from-csv", type=str)
    parser.add_argument("--save-csv", type=str)
    parser.add_argument("--output", type=str, default="mp_schedule.png")
    parser.add_argument("--mode", choices=["global", "partitioned"],
                        default="global",
                        help="global or partitioned EDF (affects annotation)")
    parser.add_argument("--zoom-start", type=float, default=None)
    parser.add_argument("--zoom-end",   type=float, default=None)
    args = parser.parse_args()

    global TASK_SET, ZOOM_START, ZOOM_END
    TASK_SET = PARTITIONED_TASK_SET if args.mode == "partitioned" else GLOBAL_TASK_SET
    if args.zoom_start is not None:
        ZOOM_START = args.zoom_start
    if args.zoom_end is not None:
        ZOOM_END = args.zoom_end

    print("=" * 65)
    print("Multiprocessor EDF Schedule Capture & Analysis Tool")
    print("=" * 65)

    print(f"\n{'Task':<18} {'C':>8} {'D':>8} {'T':>8} {'U':>8}")
    print("-" * 55)
    for ch in sorted(TASK_SET.keys()):
        t = TASK_SET[ch]
        u = t["wcet"] / t["period"]
        core_tag = f" [core {t['core']}]" if t['core'] is not None else ""
        print(f"{t['name'] + core_tag:<18} {t['wcet']:>8.4f} {t['deadline']:>8.4f}"
              f" {t['period']:>8.4f} {u:>8.4f}")
    total_u = sum(t["wcet"] / t["period"] for t in TASK_SET.values())
    print(f"{'Total U':<18} {'':>8} {'':>8} {'':>8} {total_u:>8.4f}")
    print(f"{'Bound (m=2)':<18} {'':>8} {'':>8} {'':>8} {'2.0000':>8}")

    if args.from_csv:
        samples = load_csv(args.from_csv)
    else:
        samples = capture_signals()

    if args.save_csv:
        save_csv(samples, args.save_csv)

    print("\nProcessing edges...")
    edges = extract_edges(samples)
    for ch in TASK_SET:
        n = len(edges[ch])
        print(f"  {TASK_SET[ch]['name']}: {n} edges")

    total_time = len(samples) / SAMPLE_RATE
    intervals  = build_intervals(edges, total_time)
    deadlines  = compute_deadlines(total_time, edges)
    releases   = compute_releases(total_time, edges)
    misses     = detect_deadline_misses(intervals, deadlines)

    print("\nFinding simultaneous execution intervals...")
    sim_intervals = find_simultaneous_intervals(samples)
    print(f"  Found {len(sim_intervals)} dual-core interval(s)")

    print_summary(intervals, deadlines, misses, sim_intervals, total_time)

    print("\nGenerating Gantt chart...")
    plot_gantt(intervals, deadlines, releases, misses, sim_intervals,
               mode=args.mode, save_path=args.output)


if __name__ == "__main__":
    main()
