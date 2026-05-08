#!/usr/bin/env python3
"""
analyze_bench_e_x86.py — Benchmark E: Multi-Threaded NUMA Migration Analysis
Target: x86_64, Linux 6.1.4, numa=fake=2

Reads:
  bench_e_workers_x86_*.csv    — per-thread per-phase application stats
  bench_e_migrator_x86_*.csv   — migration throughput and failure stats
  bench_e_timing_x86_*.csv     — kernel ring buffer 5-stage timing (Phase 2 sample)
  timing_4kb_*.csv             — Bench A quiescent data (for stage comparison)

Produces the same 6 plots as analyze_bench_e.py but for x86_64 data.

x86_64 interpretation notes:
  - Both nodes share one physical DIMM (numa=fake=2). Phase 1 vs Phase 3
    throughput difference will be small — no real DRAM latency gap.
  - Unlike ARM64, Unmap dominates even quiescently (try_to_migrate with
    INVLPG + IPI shootdown costs ~2.68 µs vs ARM64's 0.38 µs). Under
    concurrent load, expect Unmap to remain dominant or increase further.
  - p999 spike in Phase 2 reflects migration-PTE stall (same mechanism as
    ARM64): faulting thread sleeps until migrator calls folio_unlock().
  - IPI shootdown cost scales with online CPU count on x86 — the stage
    comparison vs ARM64 is a direct measurement of this overhead.
  - Break-even time → ∞ with fake NUMA is expected and correct.

Usage:
    python3 analyze_bench_e_x86.py [--outdir DIR]
"""

import sys
import os
import glob
import argparse
import numpy as np
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("NOTE: matplotlib not found — tables only. pip install matplotlib")


# ─────────────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────────────

PHASE_KEYS   = ["baseline", "migration", "steady"]
PHASE_LABELS = ["Phase 1\nBaseline", "Phase 2\nMigration", "Phase 3\nSteady"]
PHASE_COLORS = ["#4c72b0", "#c44e52", "#55a868"]
STAGE_COLS   = ["lock_ns", "unmap_ns", "copy_ns", "remap_ns", "unlock_ns"]
STAGE_LABELS = ["Lock", "Unmap", "Copy", "Remap", "Unlock"]
STAGE_COLORS = ["#8172b3", "#c44e52", "#4c72b0", "#55a868", "#ccb974"]


# ─────────────────────────────────────────────────────────────────────────────
# Data loading
# ─────────────────────────────────────────────────────────────────────────────

def load_workers(pattern="bench_e_workers_x86_*.csv"):
    """
    Returns: {config -> {phase -> {column -> [float values]}}}
    Aggregates across all rows for each (config, phase) pair.
    """
    files = sorted(glob.glob(pattern))
    if not files:
        return {}

    result = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for path in files:
        with open(path) as f:
            lines = f.readlines()
        if len(lines) < 2:
            continue
        hdr = lines[0].strip().split(",")
        for ln in lines[1:]:
            ln = ln.strip()
            if not ln:
                continue
            parts = ln.split(",")
            if len(parts) != len(hdr):
                continue
            row = dict(zip(hdr, parts))
            cfg   = row["config"]
            phase = row["phase"]
            for k, v in row.items():
                if k in ("config", "phase"):
                    continue
                try:
                    result[cfg][phase][k].append(float(v))
                except ValueError:
                    pass
    return result


def load_migrators(pattern="bench_e_migrator_x86_*.csv"):
    """Returns: {config -> {column -> float}}"""
    files = sorted(glob.glob(pattern))
    result = {}
    for path in files:
        with open(path) as f:
            lines = f.readlines()
        if len(lines) < 2:
            continue
        hdr = lines[0].strip().split(",")
        for ln in lines[1:]:
            ln = ln.strip()
            if not ln:
                continue
            parts = ln.split(",")
            if len(parts) != len(hdr):
                continue
            row = dict(zip(hdr, parts))
            cfg = row["config"]
            result[cfg] = {k: (float(v) if k != "config" else v)
                           for k, v in row.items()}
    return result


def load_timing_csv(pattern):
    """
    Load a ring-buffer CSV (bench_e_timing or timing_4kb).
    Returns: {label -> {column -> np.array}}
    """
    files = sorted(glob.glob(pattern))
    result = {}
    for path in files:
        # Derive a short label from the filename
        label = (os.path.basename(path)
                 .replace("bench_e_timing_", "")
                 .replace("timing_", "")
                 .replace(".csv", ""))
        with open(path) as f:
            lines = f.readlines()
        if len(lines) < 2:
            continue
        hdr  = lines[0].strip().split(",")
        rows = []
        for ln in lines[1:]:
            ln = ln.strip()
            if not ln:
                continue
            parts = ln.split(",")
            if len(parts) == len(hdr):
                rows.append(parts)
        if not rows:
            continue
        data = {}
        for ci, col in enumerate(hdr):
            raw = [r[ci] for r in rows]
            try:
                data[col] = np.array([int(v) for v in raw], dtype=np.float64)
            except ValueError:
                data[col] = np.array(raw)
        result[label] = data
    return result


# ─────────────────────────────────────────────────────────────────────────────
# Aggregation helpers
# ─────────────────────────────────────────────────────────────────────────────

def agg_phase(wdata_cfg, phase):
    """
    Aggregate worker stats across threads for one phase.

    Returns dict with:
      ops_s   — total ops/s (sum across threads, averaged over phase duration)
      gb_s    — total GB/s
      p50/p99/p999 — mean percentile across threads (ns)
      dur_ns  — mean phase duration (ns)
    """
    ph = wdata_cfg.get(phase, {})
    if not ph or not ph.get("ops"):
        return {"ops_s": 0, "gb_s": 0, "p50": 0, "p99": 0, "p999": 0,
                "dur_ns": 0, "nthreads": 0}

    total_ops  = sum(ph["ops"])
    avg_dur_ns = float(np.mean(ph.get("dur_ns", [1])))
    avg_dur_s  = avg_dur_ns / 1e9 if avg_dur_ns > 0 else 1.0
    ops_s      = total_ops / avg_dur_s
    gb_s       = ops_s * 8.0 / 1e9

    return {
        "ops_s":   ops_s,
        "gb_s":    gb_s,
        "p50":     float(np.mean(ph.get("p50_ns",  [0]))),
        "p95":     float(np.mean(ph.get("p95_ns",  [0]))),
        "p99":     float(np.mean(ph.get("p99_ns",  [0]))),
        "p999":    float(np.mean(ph.get("p999_ns", [0]))),
        "dur_ns":  avg_dur_ns,
        "nthreads": len(ph["ops"]),
    }


def break_even(p1_ops_s, p2_ops_s, p3_ops_s, phase2_dur_s):
    """
    Compute break-even time (seconds) after migration completes.

    lost_work  = (P1 - P2) * phase2_dur   [ops lost during migration]
    gain_rate  = P3 - P1                   [additional ops/s in steady state]
    break_even = lost_work / gain_rate

    Returns (break_even_s, gain_rate, lost_work) or None if not beneficial.
    """
    lost     = max(0.0, (p1_ops_s - p2_ops_s) * phase2_dur_s)
    gain     = p3_ops_s - p1_ops_s
    if gain <= 0:
        return None
    return lost / gain, gain, lost


def extract_thread_count(label):
    """Extract thread count from label like E_rand_rmw_t4_c512 → 4."""
    for part in label.split("_"):
        if part.startswith("t") and part[1:].isdigit():
            return int(part[1:])
    return None


def extract_chunk_size(label):
    """Extract chunk size from label like E_rand_rmw_t4_c512 → 512."""
    for part in label.split("_"):
        if part.startswith("c") and part[1:].isdigit():
            return int(part[1:])
    return None


def stage_percentages(tdata):
    """
    Compute per-stage share of total migration time from ring buffer data.
    Returns dict {stage_col: mean_pct (0–100)}.
    """
    avail = [s for s in STAGE_COLS if s in tdata]
    if not avail:
        return {}
    n = len(tdata[avail[0]])
    if n == 0:
        return {}
    totals = sum(tdata[s] for s in avail)
    totals = np.where(totals > 0, totals, 1.0)
    return {s: float(np.mean(tdata[s] / totals) * 100) for s in avail}


# ─────────────────────────────────────────────────────────────────────────────
# Console tables
# ─────────────────────────────────────────────────────────────────────────────

def print_throughput_table(workers):
    print("\n" + "═" * 100)
    print("  BENCHMARK E (x86_64) — THROUGHPUT SUMMARY")
    print("═" * 100)
    print(f"  {'Config':<34} {'Phase':>10} {'Threads':>8} "
          f"{'Ops/s':>12} {'GB/s':>8} {'p50 ns':>9} "
          f"{'p99 ns':>9} {'p999 ns':>10}")
    print("  " + "─" * 98)

    for cfg in sorted(workers.keys()):
        first = True
        for ph in PHASE_KEYS:
            a   = agg_phase(workers[cfg], ph)
            pfx = cfg if first else ""
            first = False
            print(f"  {pfx:<34} {ph:>10} {a['nthreads']:>8} "
                  f"{a['ops_s']:>12.3e} {a['gb_s']:>8.4f} "
                  f"{a['p50']:>9.0f} {a['p99']:>9.0f} {a['p999']:>10.0f}")
        print()


def print_breakeven_table(workers, migrators):
    print("\n" + "═" * 100)
    print("  BENCHMARK E (x86_64) — BREAK-EVEN TIME")
    print("  Break-even = time after migration completes to recover disruption cost")
    print("  Note: with numa=fake=2 there is no real latency gain → "
          "break-even → ∞ is expected")
    print("═" * 100)
    print(f"  {'Config':<34} {'P1 ops/s':>12} {'P2 ops/s':>12} "
          f"{'P3 ops/s':>12} {'DF (P2/P1)':>11} {'Benefit (P3/P1)':>16} "
          f"{'Break-even':>12}")
    print("  " + "─" * 98)

    for cfg in sorted(workers.keys()):
        a1 = agg_phase(workers[cfg], "baseline")
        a2 = agg_phase(workers[cfg], "migration")
        a3 = agg_phase(workers[cfg], "steady")
        mig = migrators.get(cfg, {})
        p2_dur = mig.get("dur_ns", 30e9) / 1e9   # fallback: 30 s

        df      = a2["ops_s"] / a1["ops_s"] if a1["ops_s"] > 0 else 0
        benefit = a3["ops_s"] / a1["ops_s"] if a1["ops_s"] > 0 else 0

        res = break_even(a1["ops_s"], a2["ops_s"], a3["ops_s"], p2_dur)
        if res is None:
            be_str = "∞ (no gain)"
        else:
            be_s, _, _ = res
            be_str = f"{be_s:.1f} s"

        print(f"  {cfg:<34} {a1['ops_s']:>12.3e} {a2['ops_s']:>12.3e} "
              f"{a3['ops_s']:>12.3e} {df:>11.3f} {benefit:>16.3f} {be_str:>12}")


def print_migration_table(migrators):
    print("\n" + "═" * 100)
    print("  BENCHMARK E (x86_64) — MIGRATION STATISTICS")
    print("═" * 100)
    print(f"  {'Config':<34} {'Attempted':>10} {'Succeeded':>10} "
          f"{'Failed':>8} {'Pages/s':>10} {'Fail %':>8} {'Dur (ms)':>10}")
    print("  " + "─" * 98)

    for cfg in sorted(migrators.keys()):
        r = migrators[cfg]
        print(f"  {cfg:<34} "
              f"{r.get('attempted', 0):>10.0f} "
              f"{r.get('succeeded', 0):>10.0f} "
              f"{r.get('failed', 0):>8.0f} "
              f"{r.get('pages_per_sec', 0):>10.0f} "
              f"{r.get('failure_rate', 0) * 100:>8.1f} "
              f"{r.get('dur_ns', 0) / 1e6:>10.1f}")


def print_stage_table(timing_e, timing_a=None):
    print("\n" + "═" * 100)
    print("  BENCHMARK E (x86_64) — RING BUFFER STAGE STATISTICS (Phase 2 sample)")
    print("  Note: ring buffer wraps during 30 s Phase 2. "
          "Stage data is a statistical sample, not a complete record.")
    print("═" * 100)

    all_data = {}
    for label, data in timing_e.items():
        all_data[f"BenchE/{label}"] = data
    if timing_a:
        for label, data in timing_a.items():
            all_data[f"BenchA/{label}"] = data

    for label, data in sorted(all_data.items()):
        avail = [s for s in STAGE_COLS if s in data and len(data[s]) > 0]
        if not avail:
            continue
        n = len(data[avail[0]])
        print(f"\n  {label}  ({n} records)")
        print(f"  {'Stage':<10} {'Mean ns':>10} {'Median ns':>10} "
              f"{'P99 ns':>10} {'% of total':>12}")
        print(f"  {'─────':<10} {'───────':>10} {'─────────':>10} "
              f"{'──────':>10} {'──────────':>12}")

        pcts = stage_percentages(data)
        for s, sl in zip(STAGE_COLS, STAGE_LABELS):
            if s not in data or len(data[s]) == 0:
                continue
            v = data[s]
            print(f"  {sl:<10} {np.mean(v):>10.0f} {np.median(v):>10.0f} "
                  f"{np.percentile(v, 99):>10.0f} {pcts.get(s, 0):>11.1f}%")


# ─────────────────────────────────────────────────────────────────────────────
# Plots
# ─────────────────────────────────────────────────────────────────────────────

def _save(fig, path, outdir):
    full = os.path.join(outdir, path)
    fig.savefig(full, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Plot → {full}")


def plot_throughput(workers, outdir):
    """
    Plot 1: Three-phase aggregate throughput (GB/s) — all configs side-by-side.
    Grouped bars: Baseline / Migration / Steady.
    """
    cfgs = sorted(workers.keys())
    if not cfgs:
        return

    x = np.arange(len(cfgs))
    w = 0.24

    fig, ax = plt.subplots(figsize=(max(10, len(cfgs) * 1.8), 5))
    for pi, (phase, label, color) in enumerate(
            zip(PHASE_KEYS, PHASE_LABELS, PHASE_COLORS)):
        vals = [agg_phase(workers[cfg], phase)["gb_s"] for cfg in cfgs]
        bars = ax.bar(x + (pi - 1) * w, vals, w,
                      label=label.replace("\n", " "),
                      color=color, edgecolor="white", linewidth=0.6)
        for b, v in zip(bars, vals):
            if v > 0.0005:
                ax.text(b.get_x() + b.get_width() / 2,
                        b.get_height() + 0.0002,
                        f"{v:.3f}", ha="center", va="bottom", fontsize=7)

    ax.set_xticks(x)
    ax.set_xticklabels([c.replace("E_", "").replace("_", "\n")
                        for c in cfgs], fontsize=8)
    ax.set_ylabel("Aggregate Application Throughput (GB/s)")
    ax.set_title("Benchmark E (x86_64): Three-Phase Application Throughput\n"
                 "(total across all worker threads)")
    ax.legend(loc="upper right")
    ax.set_ylim(bottom=0)
    ax.grid(axis="y", alpha=0.3)
    ax.tick_params(axis="x", labelsize=8)

    # Annotate disruption factor on Phase 2 bars
    for pi_cfg, cfg in enumerate(cfgs):
        a1 = agg_phase(workers[cfg], "baseline")["ops_s"]
        a2 = agg_phase(workers[cfg], "migration")["ops_s"]
        if a1 > 0:
            df = a2 / a1
            bar_x = x[pi_cfg] + 0 * w   # Phase 2 bar is at offset 0
            ax.annotate(f"DF={df:.2f}",
                        xy=(bar_x, 0), xytext=(bar_x, -0.005),
                        ha="center", va="top", fontsize=6.5, color="#c44e52")

    _save(fig, "bench_e_x86_throughput.png", outdir)


def plot_latency_percentiles(workers, outdir):
    """
    Plot 2: p50, p99, p999 mean latency per phase — all configs.
    Three subplots (one per percentile), x-axis = config.
    """
    cfgs = sorted(workers.keys())
    if not cfgs:
        return

    x = np.arange(len(cfgs))
    w = 0.24
    pcts = [("p50_ns", "p50"), ("p99_ns", "p99"), ("p999_ns", "p999")]

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    for ai, (col, ptitle) in enumerate(pcts):
        ax = axes[ai]
        for pi, (phase, label, color) in enumerate(
                zip(PHASE_KEYS, PHASE_LABELS, PHASE_COLORS)):
            vals = []
            for cfg in cfgs:
                ph = workers[cfg].get(phase, {})
                v  = float(np.mean(ph.get(col, [0]))) if ph.get(col) else 0
                vals.append(v)
            ax.bar(x + (pi - 1) * w, vals, w,
                   label=label.replace("\n", " "),
                   color=color, edgecolor="white", linewidth=0.6)

        ax.set_xticks(x)
        ax.set_xticklabels([c.replace("E_", "").replace("_", "\n")
                            for c in cfgs], fontsize=7)
        ax.set_ylabel(f"Mean {ptitle} latency (ns)")
        ax.set_title(f"{ptitle} per Phase")
        ax.legend(loc="upper right", fontsize=7)
        ax.grid(axis="y", alpha=0.3)
        ax.set_ylim(bottom=0)

    fig.suptitle("Benchmark E (x86_64): Access Latency Percentiles by Phase\n"
                 "(mean across worker threads — p999 spike in Phase 2 "
                 "reflects migration-PTE stalls)",
                 fontsize=11)
    plt.tight_layout()
    _save(fig, "bench_e_x86_latency.png", outdir)


def plot_p999_vs_threads(workers, outdir):
    """
    Plot 3: p999 tail latency vs worker thread count.
    One line per phase; uses configs with varying thread counts (t1/t4/t8)
    at fixed rand/rmw/c512 parameters.
    """
    # Collect (thread_count, config) for the rand/rmw/c512 sweep
    thread_map = {}
    for cfg in sorted(workers.keys()):
        tc = extract_thread_count(cfg)
        if (tc is not None
                and "rand" in cfg
                and "rmw" in cfg
                and "c512" in cfg):
            thread_map[tc] = cfg

    if len(thread_map) < 2:
        print("  NOTE: ≥2 thread-count configs needed for p999-vs-threads plot"
              " (need E_rand_rmw_t1/t4/t8_c512)")
        return

    tcs = sorted(thread_map.keys())
    fig, ax = plt.subplots(figsize=(7, 5))

    for pi, (phase, label, color) in enumerate(
            zip(PHASE_KEYS, PHASE_LABELS, PHASE_COLORS)):
        vals = []
        for tc in tcs:
            cfg = thread_map[tc]
            a   = agg_phase(workers[cfg], phase)
            vals.append(a["p999"])
        ax.plot(tcs, vals, "o-", color=color,
                label=label.replace("\n", " "),
                linewidth=2, markersize=7)
        # Annotate absolute values
        for tc, v in zip(tcs, vals):
            if v > 0:
                ax.annotate(f"{v:.0f}",
                            xy=(tc, v), xytext=(2, 4),
                            textcoords="offset points", fontsize=7)

    ax.set_xlabel("Worker Thread Count")
    ax.set_ylabel("Mean p999 Latency (ns)")
    ax.set_title("Benchmark E (x86_64): Tail Latency vs Thread Count\n"
                 "(rand/RMW, 512-page chunks)\n"
                 "Phase 2 spike = migration-PTE stall; scaling = IPI shootdown pressure")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_xticks(tcs)
    _save(fig, "bench_e_x86_p999_threads.png", outdir)


def plot_chunk_effect(workers, migrators, outdir):
    """
    Plot 4: Effect of chunk size on disruption factor and migration failure rate.
    Uses configs E_rand_rmw_t4_c{1,64,512}.
    """
    chunk_data = {}
    for cfg in sorted(workers.keys()):
        cc = extract_chunk_size(cfg)
        if (cc is not None
                and "rand" in cfg
                and "rmw" in cfg
                and "t4" in cfg):
            a1 = agg_phase(workers[cfg], "baseline")["ops_s"]
            a2 = agg_phase(workers[cfg], "migration")["ops_s"]
            df = a2 / a1 if a1 > 0 else 0
            fr = migrators.get(cfg, {}).get("failure_rate", 0) * 100
            chunk_data[cc] = {"df": df, "fr": fr}

    if len(chunk_data) < 2:
        print("  NOTE: ≥2 chunk-size configs needed for chunk-effect plot "
              "(need E_rand_rmw_t4_c1/c64/c512)")
        return

    chunks = sorted(chunk_data.keys())
    df_vals = [chunk_data[c]["df"] for c in chunks]
    fr_vals = [chunk_data[c]["fr"] for c in chunks]
    xlabels = [f"{c}\npages" for c in chunks]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 5))

    bars1 = ax1.bar(xlabels, df_vals, color=PHASE_COLORS[1],
                    edgecolor="white", linewidth=0.6)
    ax1.axhline(1.0, color="black", linewidth=1, linestyle="--",
                label="No disruption (DF=1)")
    for b, v in zip(bars1, df_vals):
        ax1.text(b.get_x() + b.get_width() / 2,
                 b.get_height() + 0.005,
                 f"{v:.3f}", ha="center", va="bottom", fontsize=9)
    ax1.set_ylabel("Disruption Factor (Phase2 / Phase1 throughput)")
    ax1.set_title("Disruption Factor vs Chunk Size\n"
                  "(closer to 1.0 = less disruption)")
    ax1.set_ylim(0, max(1.1, max(df_vals) * 1.15))
    ax1.legend()
    ax1.grid(axis="y", alpha=0.3)

    bars2 = ax2.bar(xlabels, fr_vals, color="#8172b3",
                    edgecolor="white", linewidth=0.6)
    for b, v in zip(bars2, fr_vals):
        ax2.text(b.get_x() + b.get_width() / 2,
                 b.get_height() + 0.05,
                 f"{v:.1f}%", ha="center", va="bottom", fontsize=9)
    ax2.set_ylabel("Migration Failure Rate (%)")
    ax2.set_title("Failure Rate vs Chunk Size\n"
                  "(failures = page busy/locked during move_pages)")
    ax2.set_ylim(bottom=0)
    ax2.grid(axis="y", alpha=0.3)

    fig.suptitle("Benchmark E (x86_64): Chunk Size Effect\n"
                 "(rand/RMW, 4 workers, node0→node1)",
                 fontsize=11)
    plt.tight_layout()
    _save(fig, "bench_e_x86_chunk_effect.png", outdir)


def plot_stage_comparison(timing_e, timing_a, outdir):
    """
    Plot 5: Stage % breakdown — Bench E (under load) vs Bench A (quiescent).

    The primary Bench E config is E_rand_rmw_t4_c512.
    Bench A quiescent data comes from timing_4kb_*.csv.

    x86_64 expectation:
      - Unmap dominates quiescently (41-44%) and likely increases further
        under load. Copy is a smaller fraction than ARM64.
      - IPI shootdown cost scales with online CPU count — concurrent workers
        generating TLB pressure forces more pages into the IPI broadcast path.
      - Lock contention may be visible if workers faulting on migration PTEs
        cause brief folio_lock() re-contention.
    """
    datasets = {}

    primary = "E_rand_rmw_t4_c512"
    if primary in timing_e:
        datasets[f"Bench E (under load)\n{primary}"] = stage_percentages(timing_e[primary])
    elif timing_e:
        first = next(iter(timing_e))
        datasets[f"Bench E (under load)\n{first}"] = stage_percentages(timing_e[first])

    if timing_a:
        for label in sorted(timing_a.keys()):
            pcts = stage_percentages(timing_a[label])
            if pcts:
                datasets[f"Bench A (quiescent)\n{label}"] = pcts
                break   # one quiescent reference is sufficient

    if not datasets:
        print("  NOTE: no stage data available for comparison plot")
        return

    x   = np.arange(len(STAGE_LABELS))
    w   = 0.8 / max(len(datasets), 1)
    fig, ax = plt.subplots(figsize=(9, 5))
    colors = ["#c44e52", "#4c72b0", "#55a868", "#8172b3"]

    for di, (label, pcts) in enumerate(datasets.items()):
        vals = [pcts.get(s, 0) for s in STAGE_COLS]
        offset = (di - len(datasets) / 2 + 0.5) * w
        bars = ax.bar(x + offset, vals, w, label=label,
                      color=colors[di % len(colors)],
                      edgecolor="white", linewidth=0.5)
        for b, v in zip(bars, vals):
            if v > 0.5:
                ax.text(b.get_x() + b.get_width() / 2,
                        b.get_height() + 0.3,
                        f"{v:.1f}%", ha="center", va="bottom", fontsize=7)

    ax.set_xticks(x)
    ax.set_xticklabels(STAGE_LABELS, fontsize=11)
    ax.set_ylabel("Stage share of total migration time (%)")
    ax.set_title("Benchmark E (x86_64): Stage Breakdown Under Load vs Quiescent\n"
                 "Unmap expected to dominate; IPI shootdown increases under worker pressure")
    ax.legend(loc="upper right", fontsize=9)
    ax.set_ylim(bottom=0)
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    _save(fig, "bench_e_x86_stage_comparison.png", outdir)


def plot_disruption_factor_overview(workers, outdir):
    """
    Extra: disruption factor and migration benefit bar chart for all configs.
    DF = Phase2/Phase1 throughput (< 1 = disrupted)
    MB = Phase3/Phase1 throughput (> 1 = beneficial)
    """
    cfgs = sorted(workers.keys())
    if not cfgs:
        return

    dfs, mbs = [], []
    for cfg in cfgs:
        a1 = agg_phase(workers[cfg], "baseline")["ops_s"]
        a2 = agg_phase(workers[cfg], "migration")["ops_s"]
        a3 = agg_phase(workers[cfg], "steady")["ops_s"]
        dfs.append(a2 / a1 if a1 > 0 else 0)
        mbs.append(a3 / a1 if a1 > 0 else 0)

    x = np.arange(len(cfgs))
    w = 0.35
    fig, ax = plt.subplots(figsize=(max(10, len(cfgs) * 1.5), 5))
    b1 = ax.bar(x - w/2, dfs, w, label="Disruption Factor (P2/P1)",
                color=PHASE_COLORS[1], edgecolor="white", linewidth=0.6)
    b2 = ax.bar(x + w/2, mbs, w, label="Migration Benefit (P3/P1)",
                color=PHASE_COLORS[2], edgecolor="white", linewidth=0.6)
    for bars in (b1, b2):
        for b in bars:
            v = b.get_height()
            ax.text(b.get_x() + b.get_width() / 2,
                    v + 0.005, f"{v:.3f}",
                    ha="center", va="bottom", fontsize=7)

    ax.axhline(1.0, color="black", linewidth=1.2, linestyle="--",
               label="Baseline = 1.0")
    ax.set_xticks(x)
    ax.set_xticklabels([c.replace("E_", "").replace("_", "\n")
                        for c in cfgs], fontsize=8)
    ax.set_ylabel("Throughput ratio (relative to Phase 1 baseline)")
    ax.set_title("Benchmark E (x86_64): Disruption Factor and Migration Benefit\n"
                 "(DF < 1 = application hurt by migration; "
                 "MB > 1 = locality improvement achieved)")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    ax.set_ylim(bottom=0)
    plt.tight_layout()
    _save(fig, "bench_e_x86_disruption_benefit.png", outdir)


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Analyze Benchmark E (x86_64): Multi-Threaded NUMA Migration")
    parser.add_argument("--outdir", default=".",
                        help="Directory for output plots (default: .)")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    print("╔══════════════════════════════════════════════════════╗")
    print("║  Benchmark E Analysis: Multi-Threaded NUMA Migration ║")
    print("║  x86_64 / Linux 6.1.4 / numa=fake=2                 ║")
    print("╚══════════════════════════════════════════════════════╝\n")

    # ── Load data ──
    workers   = load_workers("bench_e_workers_x86_*.csv")
    migrators = load_migrators("bench_e_migrator_x86_*.csv")
    timing_e  = load_timing_csv("bench_e_timing_x86_*.csv")
    timing_a  = load_timing_csv("timing_4kb_*.csv")    # Bench A quiescent

    if not workers:
        print("ERROR: no bench_e_workers_x86_*.csv files found.")
        print("  Run: sudo ./mig_bench_e_x86  (or ./mig_bench_e_x86 <config_index>)")
        sys.exit(1)

    print(f"  Worker CSVs   : {len(workers)} configs")
    print(f"  Migrator CSVs : {len(migrators)} configs")
    print(f"  Ring buffer E : {len(timing_e)} files")
    print(f"  Ring buffer A : {len(timing_a)} files (quiescent reference)")
    print()

    # ── Console tables ──
    print_throughput_table(workers)
    print_breakeven_table(workers, migrators)
    print_migration_table(migrators)
    print_stage_table(timing_e, timing_a)

    # ── Plots ──
    if not HAS_MPL:
        print("\n  Install matplotlib for plots: pip install matplotlib")
        return

    print("\n  Generating plots ...")
    plot_throughput(workers, args.outdir)
    plot_latency_percentiles(workers, args.outdir)
    plot_p999_vs_threads(workers, args.outdir)
    plot_chunk_effect(workers, migrators, args.outdir)
    plot_stage_comparison(timing_e, timing_a, args.outdir)
    plot_disruption_factor_overview(workers, args.outdir)

    print(f"\n  All plots saved to: {os.path.abspath(args.outdir)}/")
    print("\n  Interpretation notes:")
    print("  ┌─ Phase 2 p999 spike → migration-PTE stall on worker threads")
    print("  │    Cross-reference with ring buffer Unmap+Copy+Remap window")
    print("  ├─ Disruption Factor < 1 → migration hurts application throughput")
    print("  ├─ Break-even → ∞ with numa=fake=2 (no real DRAM latency gain)")
    print("  ├─ Unmap expected to dominate (IPI shootdown per PTE, ~2.68 µs)")
    print("  │    Compare stage breakdown vs ARM64 to quantify IPI overhead")
    print("  ├─ chunk=1 vs chunk=512: syscall overhead dominates at chunk=1;")
    print("  │    kernel IPI batching saturates at chunk≥64 same as ARM64.")
    print("  └─ Ring buffer wraps ~8×/pass during 30 s Phase 2 — treat as sample")


if __name__ == "__main__":
    main()

