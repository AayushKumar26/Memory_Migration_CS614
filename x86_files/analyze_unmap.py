#!/usr/bin/env python3
"""
analyze_unmap.py — Unmap stage sub-component analysis for CS614
================================================================
Implements Tests T2–T5 from the micro-analysis document.

Tests:
  T2  Cache thrash detection    — try_migrate_ns vs seq quintile
  T3  rwsem outlier detection   — kswapd contention burst pattern
  T4  Load-delta decomposition  — quiescent vs under-load comparison
  T5  Full decomposition        — rwsem/ptl/ipi breakdown (needs patch)

Usage examples
--------------
# T2 — cache thrash (single CSV, ≥ 512 records recommended):
  python3 analyze_unmap.py --t2 timing_4kb_2048.csv

# T3 — rwsem outliers (single CSV, optionally with vmstat log):
  python3 analyze_unmap.py --t3 timing_4kb_2048.csv [--vmstat vmstat.log]

# T4 — load delta (quiescent CSV vs under-load CSV):
  python3 analyze_unmap.py --t4 bench_a_quiescent.csv bench_e_loaded.csv

# T5 — full sub-component decomposition (CSV with rmap patch fields):
  python3 analyze_unmap.py --t5 timing_4kb_512_patched.csv

# T1 summary plot (from run_t1.sh output):
  python3 analyze_unmap.py --t1 t1_results/t1_summary.csv

# Run all tests in sequence:
  python3 analyze_unmap.py --all \
      --csv timing_4kb_2048.csv \
      --loaded bench_e_loaded.csv \
      --t1-summary t1_results/t1_summary.csv
"""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# CSV loading
# ---------------------------------------------------------------------------

REQUIRED_COLS = {"seq", "unmap_ns", "try_migrate_ns"}
RMAP_PATCH_COLS = {"rwsem_wait_ns", "ptl_wait_ns", "ipi_wait_ns", "online_cpus"}


def load_csv(path: str) -> list[dict]:
    """Load mig_timing CSV. Returns list of dicts with numeric values."""
    rows = []
    path = Path(path)
    if not path.exists():
        sys.exit(f"ERROR: file not found: {path}")

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            sys.exit(f"ERROR: empty or headerless CSV: {path}")

        missing = REQUIRED_COLS - set(reader.fieldnames)
        if missing:
            sys.exit(f"ERROR: CSV missing required columns: {missing}\n"
                     f"       Available: {set(reader.fieldnames)}")

        for i, row in enumerate(reader):
            try:
                numeric = {k: int(v) if v.lstrip('-').isdigit() else v
                           for k, v in row.items()}
                rows.append(numeric)
            except Exception as e:
                print(f"WARNING: skipping row {i}: {e}", file=sys.stderr)

    print(f"Loaded {len(rows)} records from {path}")
    return rows


def has_rmap_patch(rows: list[dict]) -> bool:
    if not rows:
        return False
    return all(col in rows[0] for col in RMAP_PATCH_COLS)


# ---------------------------------------------------------------------------
# Utility statistics helpers
# ---------------------------------------------------------------------------

def percentile(data, p):
    """p-th percentile of sorted or unsorted data."""
    s = sorted(data)
    idx = (len(s) - 1) * p / 100
    lo, hi = int(idx), min(int(idx) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (idx - lo)


def summarise(label: str, values: list[int | float]):
    if not values:
        print(f"  {label}: no data")
        return
    mean = statistics.mean(values)
    med  = statistics.median(values)
    std  = statistics.stdev(values) if len(values) > 1 else 0.0
    cv   = std / mean if mean else 0.0
    p99  = percentile(values, 99)
    print(f"  {label}:")
    print(f"    n={len(values):6d}  mean={mean:9.1f} ns  "
          f"median={med:9.1f} ns  std={std:9.1f} ns  CV={cv:.3f}  p99={p99:9.1f} ns")


def divline(char="=", width=72):
    print(char * width)


# ---------------------------------------------------------------------------
# T1 — IPI isolation (summary CSV from run_t1.sh)
# ---------------------------------------------------------------------------

def test_t1(summary_csv: str):
    divline()
    print("T1: IPI Isolation via CPU Count (H1 / H5)")
    divline("-")

    path = Path(summary_csv)
    if not path.exists():
        sys.exit(f"ERROR: T1 summary CSV not found: {path}")

    from collections import defaultdict
    by_cpu: dict[int, list[float]] = defaultdict(list)
    by_cpu_tmig: dict[int, list[float]] = defaultdict(list)

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                ncpu  = int(row["online_cpus"])
                umean = float(row["mean_unmap_ns"])
                tmean = float(row["mean_try_migrate_ns"])
                by_cpu[ncpu].append(umean)
                by_cpu_tmig[ncpu].append(tmean)
            except (KeyError, ValueError):
                pass

    if not by_cpu:
        print("ERROR: no valid rows in T1 summary CSV.")
        return

    print(f"\n{'online_cpus':>12}  {'mean_unmap_ns':>16}  {'mean_try_migrate_ns':>22}  {'ratio_vs_max_cpu':>18}")
    sorted_cpus = sorted(by_cpu.keys(), reverse=True)
    baseline_unmap = statistics.mean(by_cpu[sorted_cpus[0]])

    for ncpu in sorted_cpus:
        u = statistics.mean(by_cpu[ncpu])
        t = statistics.mean(by_cpu_tmig[ncpu])
        ratio = u / baseline_unmap
        print(f"  {ncpu:10d}  {u:16.1f}  {t:22.1f}  {ratio:18.3f}")

    # Linearity check: regress mean_unmap_ns on online_cpus
    xs = [ncpu for ncpu in sorted_cpus]
    ys = [statistics.mean(by_cpu[ncpu]) for ncpu in sorted_cpus]
    if len(xs) >= 3:
        xbar, ybar = statistics.mean(xs), statistics.mean(ys)
        ss_xx = sum((x - xbar)**2 for x in xs)
        ss_xy = sum((x - xbar)*(y - ybar) for x, y in zip(xs, ys))
        slope = ss_xy / ss_xx if ss_xx else 0
        r2 = (ss_xy**2 / (ss_xx * sum((y - ybar)**2 for y in ys))
              if ss_xx and sum((y - ybar)**2 for y in ys) else 0)
        print(f"\n  Linear regression: slope={slope:+.1f} ns/CPU  R²={r2:.3f}")
        if r2 > 0.85:
            print("  INTERPRETATION: Strong linear relationship (R²>0.85).")
            print("  → H1 SUPPORTED: IPI roundtrip to N CPUs dominates unmap_ns.")
            print("    Batched TLB shootdown optimisation is well-motivated.")
        elif r2 < 0.3:
            print("  INTERPRETATION: Weak/no linear relationship (R²<0.3).")
            print("  → H1 NOT SUPPORTED: structural cost (rmap walk, page-table walk)")
            print("    dominates.  CPU count has little effect.")
        else:
            print("  INTERPRETATION: Moderate relationship (0.3≤R²≤0.85).")
            print("  → Mixed: IPI contributes but is not the sole component.")
            print("    Apply rmap_x86_patch.diff and run T5 for sub-isolation.")
    else:
        print("\n  Only 2 CPU configurations — need ≥3 points for regression.")
        if len(xs) == 2:
            ratio_2pt = ys[1] / ys[0] if ys[0] else 1.0
            expected_ipi = xs[1] / xs[0]
            print(f"  2-point ratio: {ratio_2pt:.3f}  (expected if pure IPI: {expected_ipi:.3f})")
            if abs(ratio_2pt - expected_ipi) < 0.15 * expected_ipi:
                print("  → Consistent with IPI-linear hypothesis.")
            else:
                print("  → Inconsistent with pure IPI scaling.")


# ---------------------------------------------------------------------------
# T2 — Cache thrash detection
# ---------------------------------------------------------------------------

def test_t2(rows: list[dict]):
    divline()
    print("T2: Cache Thrash Detection (H4)")
    divline("-")
    print("  Hypothesis: anon_vma tree and page-table nodes are cold after")
    print("  32+ migrations, causing try_migrate_ns to rise with seq number.")
    print()

    vals = [(r["seq"], r["try_migrate_ns"]) for r in rows
            if r.get("page_was_mapped", 1)]
    if len(vals) < 50:
        print(f"  WARNING: only {len(vals)} mapped records — need ≥50 for quintile analysis.")
        return

    vals.sort(key=lambda x: x[0])
    n = len(vals)
    q_size = n // 5
    quintiles = [vals[i*q_size:(i+1)*q_size] for i in range(5)]
    # absorb remainder into Q5
    quintiles[4].extend(vals[5*q_size:])

    q_means   = [statistics.mean(v for _, v in q) for q in quintiles]
    q_medians = [statistics.median(v for _, v in q) for q in quintiles]
    q_stds    = [statistics.stdev(v for _, v in q) if len(q) > 1 else 0
                 for q in quintiles]

    print(f"  {'Quintile':>10}  {'seq range':>16}  {'n':>6}  "
          f"{'mean ns':>10}  {'median ns':>10}  {'std ns':>10}")
    for i, (q, qm, qmed, qstd) in enumerate(zip(quintiles, q_means, q_medians, q_stds), 1):
        seq_lo = q[0][0]
        seq_hi = q[-1][0]
        print(f"  {'Q'+str(i):>10}  {seq_lo:>7}–{seq_hi:>7}  {len(q):>6}"
              f"  {qm:>10.1f}  {qmed:>10.1f}  {qstd:>10.1f}")

    ratio_q5_q1 = q_means[4] / q_means[0] if q_means[0] else 1.0
    # Monotonicity check: count how many consecutive quintile pairs rise
    rising = sum(q_means[i] < q_means[i+1] for i in range(4))

    print(f"\n  Q5/Q1 mean ratio   : {ratio_q5_q1:.3f}")
    print(f"  Monotonically rising quintile pairs: {rising}/4")

    if ratio_q5_q1 > 1.20 and rising >= 3:
        print("\n  RESULT → H4 SUPPORTED: Q5 > Q1 by >20% with monotonic rise.")
        print("  Cache thrashing on anon_vma tree / page-table entries is real.")
        print("  Recommendation: migrate pages in ascending VA order (address-ordered)")
        print("  to improve page-table walk locality.")
    elif ratio_q5_q1 < 1.10:
        print("\n  RESULT → H4 NOT SUPPORTED: Q5/Q1 ratio < 1.10.")
        print("  No significant cache thrash signal.  IPI or spinlock likely dominant.")
    else:
        print(f"\n  RESULT → WEAK H4 signal (ratio={ratio_q5_q1:.2f}, monotone={rising}/4).")
        print("  Cache thrash may contribute at the margin.  Run with 2048+ pages")
        print("  for a stronger signal.")

    # Early vs late batch 32-row comparison
    if n >= 64:
        early = [v for _, v in vals[:32]]
        late  = [v for _, v in vals[-32:]]
        early_mean = statistics.mean(early)
        late_mean  = statistics.mean(late)
        print(f"\n  First-32 mean : {early_mean:.1f} ns")
        print(f"  Last-32 mean  : {late_mean:.1f} ns")
        print(f"  Ratio last/first: {late_mean/early_mean:.3f}")


# ---------------------------------------------------------------------------
# T3 — rwsem outlier / kswapd contention detection
# ---------------------------------------------------------------------------

def test_t3(rows: list[dict], vmstat_log: str | None = None):
    divline()
    print("T3: rwsem Contention / kswapd Outlier Detection (H3)")
    divline("-")
    print("  Hypothesis: kswapd write-locks anon_vma->rwsem during LRU reclaim,")
    print("  causing outlier spikes in try_migrate_ns that cluster in seq-time.")
    print()

    mapped = [r for r in rows if r.get("page_was_mapped", 1)]
    if not mapped:
        print("  ERROR: no page_was_mapped=1 records.")
        return

    tm_vals = [r["try_migrate_ns"] for r in mapped]
    um_vals = [r["unmap_ns"] for r in mapped]

    median_tm = statistics.median(tm_vals)
    threshold = median_tm * 5
    outliers  = [r for r in mapped if r["try_migrate_ns"] > threshold]

    print(f"  Total mapped records : {len(mapped)}")
    print(f"  Median try_migrate_ns: {median_tm:.1f} ns")
    print(f"  Outlier threshold    : {threshold:.1f} ns  (5 × median)")
    print(f"  Outlier count        : {len(outliers)} ({100*len(outliers)/len(mapped):.1f}%)")

    if not outliers:
        print("\n  RESULT → H3 NOT SUPPORTED: no outliers above 5× median.")
        print("  rwsem contention from kswapd is not visible in this run.")
        return

    out_seqs = [r["seq"] for r in outliers]
    seq_min, seq_max = min(out_seqs), max(out_seqs)
    all_seqs = [r["seq"] for r in mapped]
    full_range = max(all_seqs) - min(all_seqs) if len(all_seqs) > 1 else 1
    outlier_span = seq_max - seq_min

    print(f"\n  Outlier seq range    : {seq_min} – {seq_max}  (span={outlier_span})")
    print(f"  Full seq range       : {min(all_seqs)} – {max(all_seqs)}")
    print(f"  Outlier seq fraction : {outlier_span/full_range:.3f} of full range")

    print(f"\n  Top-10 outlier records (seq, try_migrate_ns, unmap_ns):")
    top10 = sorted(outliers, key=lambda r: r["try_migrate_ns"], reverse=True)[:10]
    for r in top10:
        print(f"    seq={r['seq']:6d}  try_migrate_ns={r['try_migrate_ns']:10d}  "
              f"unmap_ns={r['unmap_ns']:10d}")

    # Cluster check: are outliers concentrated in a narrow seq window?
    concentration = outlier_span / full_range
    if concentration < 0.15 and len(outliers) >= 3:
        print(f"\n  RESULT → H3 SUPPORTED: outliers concentrated in "
              f"{concentration*100:.1f}% of seq range.")
        print("  Burst pattern is consistent with kswapd write-lock contention.")
        if vmstat_log:
            print(f"  Cross-check with {vmstat_log} — look for 'si' or 'so' spikes")
            print("  in the time window corresponding to the outlier seq range.")
    else:
        print(f"\n  RESULT → H3 AMBIGUOUS: outlier span = {concentration*100:.1f}% of range.")
        print("  Outliers are spread out — more consistent with IPI variance than")
        print("  kswapd contention (which would cluster in a burst).")

    # CV as a discriminator
    cv = statistics.stdev(tm_vals) / statistics.mean(tm_vals) if statistics.mean(tm_vals) else 0
    print(f"\n  try_migrate_ns CV    : {cv:.3f}")
    if cv > 1.0:
        print("  CV > 1.0 — high variance consistent with intermittent lock contention.")
    else:
        print("  CV ≤ 1.0 — moderate variance; IPI cost is bounded and consistent.")

    # vmstat cross-reference (best-effort, no timestamps in CSV)
    if vmstat_log:
        vmstat_path = Path(vmstat_log)
        if vmstat_path.exists():
            print(f"\n  vmstat log provided: {vmstat_log}")
            with open(vmstat_path) as f:
                lines = [l.strip() for l in f if l.strip() and not l.startswith("procs")]
            # Count lines with non-zero 'si' (swap-in from kswapd)
            swapping_lines = 0
            for line in lines:
                parts = line.split()
                if len(parts) >= 8:
                    try:
                        si = int(parts[6])
                        so = int(parts[7])
                        if si > 0 or so > 0:
                            swapping_lines += 1
                    except ValueError:
                        pass
            print(f"  vmstat lines with swap activity: {swapping_lines}/{len(lines)}")
            if swapping_lines > 0:
                print("  → kswapd was active during the run. Cross-references H3.")
        else:
            print(f"  WARNING: vmstat log not found: {vmstat_log}")


# ---------------------------------------------------------------------------
# T4 — Load-delta decomposition
# ---------------------------------------------------------------------------

def test_t4(quiescent_rows: list[dict], loaded_rows: list[dict]):
    divline()
    print("T4: Load-Delta Decomposition (H1 vs H2 under Benchmark E load)")
    divline("-")
    print("  Decomposes the unmap_ns jump from quiescent to under-load into")
    print("  H1 (more IPIs to worker CPUs) vs H2 (spinlock contention from")
    print("  worker fault handlers holding pte_lockptr).")
    print()

    def stats(rows, field):
        vals = [r[field] for r in rows if r.get("page_was_mapped", 1)]
        if not vals:
            return None, None, None
        return statistics.mean(vals), statistics.median(vals), \
               statistics.stdev(vals) if len(vals) > 1 else 0.0

    for label, rows in [("Quiescent (Bench A)", quiescent_rows),
                         ("Under load (Bench E)", loaded_rows)]:
        print(f"  {label}  (n={len(rows)} total records)")
        for field in ("unmap_ns", "try_migrate_ns", "copy_ns"):
            mn, med, std = stats(rows, field)
            if mn is not None:
                cv = std / mn if mn else 0
                print(f"    {field:22s}: mean={mn:9.1f}  median={med:9.1f}  "
                      f"std={std:9.1f}  CV={cv:.3f}")
        print()

    q_um  = statistics.mean(r["unmap_ns"] for r in quiescent_rows if r.get("page_was_mapped", 1))
    l_um  = statistics.mean(r["unmap_ns"] for r in loaded_rows    if r.get("page_was_mapped", 1))
    delta = l_um - q_um

    q_tm  = statistics.mean(r["try_migrate_ns"] for r in quiescent_rows if r.get("page_was_mapped", 1))
    l_tm  = statistics.mean(r["try_migrate_ns"] for r in loaded_rows    if r.get("page_was_mapped", 1))
    delta_tm = l_tm - q_tm

    print(f"  Unmap load delta     : +{delta:,.1f} ns  ({l_um:.1f} − {q_um:.1f})")
    print(f"  try_migrate delta    : +{delta_tm:,.1f} ns")
    print(f"  Gap delta (overhead) : +{delta - delta_tm:,.1f} ns")

    # Check whether delta is chunk-size invariant (already known from Table 1)
    print()
    print("  Invariance check:")
    print("  If delta is constant across chunk sizes (c1, c64, c512),")
    print("  the load-dependent cost is per-page not per-batch → H1 (IPI")
    print("  to worker CPUs) not H2 (spinlock contention scaling with batch).")
    print("  If delta rises with thread count (t1 → t2 → t4),")
    print("  H2 (spinlock contention from concurrent fault handlers) is real.")
    print()

    # Check copy_ns — the key asymmetry diagnostic from the report
    q_cp  = statistics.mean(r["copy_ns"] for r in quiescent_rows)
    l_cp  = statistics.mean(r["copy_ns"] for r in loaded_rows)
    copy_delta = l_cp - q_cp
    print(f"  copy_ns quiescent : {q_cp:,.1f} ns")
    print(f"  copy_ns under load: {l_cp:,.1f} ns  (delta={copy_delta:+,.1f} ns)")

    if copy_delta < 0:
        print()
        print("  DIAGNOSTIC: copy_ns FALLS under load.")
        print("  This is the critical x86 finding: worker threads are")
        print("  fault-faulting and accessing pages, but because the migration")
        print("  PTE blocks them before copy begins, the copy window sees a")
        print("  quieter bus (workers stalled, not issuing memory traffic).")
        print("  This rules out memory-bandwidth contention as the cause of")
        print("  the unmap spike — the bottleneck is purely in the TLB/IPI path.")

    if delta > 1000:
        print()
        if delta_tm / delta > 0.8:
            print("  RESULT: >80% of load delta is inside try_migrate_ns.")
            print("  → Both H1 and H2 candidates. Apply rmap patch (T5) to isolate.")
        else:
            print(f"  RESULT: only {100*delta_tm/delta:.0f}% of load delta is inside try_migrate_ns.")
            print("  → Significant overhead outside try_migrate() — check observer cost")
            print("    or unexpected pre/post-call work in __unmap_and_move().")


# ---------------------------------------------------------------------------
# T5 — Full sub-component decomposition (requires rmap patch)
# ---------------------------------------------------------------------------

def test_t5(rows: list[dict]):
    divline()
    print("T5: Full Sub-Component Decomposition (rmap patch fields)")
    divline("-")

    if not has_rmap_patch(rows):
        print("  ERROR: CSV does not contain rmap patch columns.")
        print("  Required: rwsem_wait_ns, ptl_wait_ns, ipi_wait_ns, online_cpus")
        print()
        print("  Apply rmap_x86_patch.diff to mm/rmap.c, rebuild kernel,")
        print("  re-run Benchmark A, then run T5.")
        return

    mapped = [r for r in rows if r.get("page_was_mapped", 1)]
    if not mapped:
        print("  ERROR: no page_was_mapped=1 records.")
        return

    # Check if patch was actually active (non-zero fields)
    nonzero_ipi = sum(1 for r in mapped if r.get("ipi_wait_ns", 0) > 0)
    if nonzero_ipi == 0:
        print("  WARNING: all ipi_wait_ns values are zero.")
        print("  The CSV has the patch columns but they are all zero.")
        print("  Verify that CONFIG_MIG_RMAP_TIMING=1 is set in mm/rmap.c")
        print("  and that the kernel was rebuilt after patching.")
        return

    print(f"  Records with non-zero ipi_wait_ns: {nonzero_ipi}/{len(mapped)}")
    print()

    fields = [
        ("try_migrate_ns",  "Total try_migrate()"),
        ("rwsem_wait_ns",   "H3 rwsem acquisition"),
        ("ptl_wait_ns",     "H2 page-table spinlock"),
        ("ipi_wait_ns",     "H1 ptep_clear_flush (IPI)"),
    ]

    means = {}
    for col, label in fields:
        vals = [r[col] for r in mapped if col in r]
        if vals:
            means[col] = statistics.mean(vals)
            summarise(label, vals)

    print()
    tm    = means.get("try_migrate_ns", 1)
    rwsem = means.get("rwsem_wait_ns", 0)
    ptl   = means.get("ptl_wait_ns", 0)
    ipi   = means.get("ipi_wait_ns", 0)

    # -----------------------------------------------------------------------
    # NESTING CORRECTION
    # The three brackets are NOT independent flat slices of try_migrate_ns.
    # They are nested:
    #
    #  try_migrate_ns
    #  ├─ rwsem_ns       : rmap_walk_anon_lock() — lock acquisition only
    #  ├─ [rmap tree walk + page_vma_mapped_walk (4-level table + ptl spin)]
    #  ├─ ptl_ns         : entire PTE body while ptl held
    #  │   ├─ ipi_ns     : ptep_clear_flush() IPI roundtrip (INSIDE ptl)
    #  │   └─ pte_work   : page_remove_rmap, make_migration_entry, etc.
    #  └─ [folio_put + up_read + misc]
    #
    # Correct non-overlapping decomposition:
    #   pte_work_ns  = ptl_ns - ipi_ns    (PTE body minus the IPI itself)
    #   outside_ns   = tm - rwsem - ptl   (rmap walk + page table walk + overhead)
    #   (NOT tm - rwsem - ptl - ipi, which would subtract IPI twice)
    # -----------------------------------------------------------------------
    pte_work  = max(ptl - ipi, 0)         # non-IPI PTE body (LOCK DEC etc.)
    outside   = max(tm - rwsem - ptl, 0)  # rmap tree walk + 4-level table walk
                                           # + page_vma_mapped_walk + folio_put

    print("  Nesting note: ipi_ns is measured INSIDE the ptl bracket.")
    print("  ptl_ns = ipi_ns + other-PTE-work.  outside = tm - rwsem - ptl.")
    print()

    # ── Median-based values (PREFERRED — robust to VM preemption outliers) ──
    def med(field):
        vals = sorted(r[field] for r in mapped if field in r)
        if not vals: return 0.0
        mid = len(vals) // 2
        return (vals[mid-1] + vals[mid]) / 2 if len(vals) % 2 == 0 else float(vals[mid])

    tm_med    = med("try_migrate_ns")
    rwsem_med = med("rwsem_wait_ns")
    ptl_med   = med("ptl_wait_ns")
    ipi_med   = med("ipi_wait_ns")

    pte_work_med = max(ptl_med - ipi_med, 0)
    outside_med  = max(tm_med - rwsem_med - ptl_med, 0)
    total_med_check = rwsem_med + ipi_med + pte_work_med + outside_med

    print("  ── Median-based breakdown (USE THIS — robust to VM/kswapd outliers) ──")
    print(f"    {'Component':<38}  {'median ns':>10}  {'%':>8}")
    print(f"    {'─'*38}  {'─'*10}  {'─'*8}")
    for name, val in [
        ("H3  rwsem (down_read acquisition)",   rwsem_med),
        ("H1  ipi   (flush_tlb_others)",         ipi_med),
        ("    other PTE body (page_remove_rmap)", pte_work_med),
        ("    outside-ptl (rmap/ptbl walk)",      outside_med),
    ]:
        print(f"    {name:<38}  {val:>10.1f}  {100*val/tm_med if tm_med else 0:>7.1f}%")
    print(f"    {'─'*38}  {'─'*10}")
    print(f"    {'try_migrate_ns (median)':<38}  {tm_med:>10.1f}  100.0%")

    # CV warning — flag fields with extreme variance
    def cv(field):
        vals = [r[field] for r in mapped if field in r]
        if len(vals) < 2: return 0.0
        mn = statistics.mean(vals)
        return statistics.stdev(vals) / mn if mn else 0.0
    print()
    for field, label in [("rwsem_wait_ns","rwsem"), ("ptl_wait_ns","ptl"),
                          ("ipi_wait_ns","ipi"), ("try_migrate_ns","try_migrate")]:
        c = cv(field)
        flag = " ← MEAN UNRELIABLE — use median" if c > 3.0 else ""
        print(f"  CV {label:<14}: {c:.2f}{flag}")
    print()

    # ── Flat table (raw bracket values, for reference) ─────────────────────
    print("  Raw bracket values (mean-based, nested — do not sum to 100%):")
    print(f"    {'Bracket':<32}  {'mean ns':>10}  {'% of try_migrate':>18}")
    print(f"    {'─'*32}  {'─'*10}  {'─'*18}")
    for name, val in [
        ("try_migrate_ns  (total)",  tm),
        ("  rwsem_ns      (H3)",   rwsem),
        ("  ptl_ns        (H2+H1)", ptl),
        ("    ipi_ns      (H1)",    ipi),
    ]:
        print(f"    {name:<32}  {val:>10.1f}  {100*val/tm if tm else 0:>17.1f}%")

    print()
    # ── Non-overlapping breakdown ───────────────────────────────────────────
    print("  Non-overlapping breakdown (sums to 100%):")
    print(f"    {'Component':<38}  {'mean ns':>10}  {'%':>8}")
    print(f"    {'─'*38}  {'─'*10}  {'─'*8}")
    breakdown = [
        ("H3  rwsem (uncontended down_read)",   rwsem),
        ("H1  ipi   (flush_tlb_others)",         ipi),
        ("    other PTE body (page_remove_rmap)", pte_work),
        ("    outside-ptl (rmap/ptbl walk)",      outside),
    ]
    for name, val in breakdown:
        print(f"    {name:<38}  {val:>10.1f}  {100*val/tm if tm else 0:>7.1f}%")
    print(f"    {'─'*38}  {'─'*10}")
    total_check = rwsem + ipi + pte_work + outside
    print(f"    {'total':<38}  {total_check:>10.1f}  {100*total_check/tm if tm else 0:>7.1f}%")

    # ── Verdict ─────────────────────────────────────────────────────────────
    print()
    print("  ── Verdict ─────────────────────────────────────────────────")

    ipi_pct     = 100 * ipi     / tm
    pte_pct     = 100 * pte_work / tm
    outside_pct = 100 * outside  / tm
    rwsem_pct   = 100 * rwsem   / tm

    # H2 contention only visible under load — warn if this is a quiescent run
    print(f"  H1 IPI        : {ipi_pct:.1f}%  (flush_tlb_others roundtrip)")
    print(f"  H2 ptl-hold   : {pte_pct:.1f}%  (legitimate PTE work in quiescent run;")
    print(f"                          contention only appears under Bench-E load)")
    print(f"  H3 rwsem      : {rwsem_pct:.1f}%  (uncontended down_read cost)")
    print(f"  structural    : {outside_pct:.1f}%  (rmap tree walk + 4-level page table walk)")
    print()

    if ipi_pct > 40:
        print("  DOMINANT: H1 IPI shootdown.")
        print("  Optimisation: defer flush_tlb_others() across a batch of pages")
        print("  (batch-N deferred TLB shootdown). Send one IPI per batch, not per page.")
    elif outside_pct > 40:
        print("  DOMINANT: structural walk cost (rmap tree + page table).")
        print("  Optimisation: migrate pages in VA order for page-table locality;")
        print("  pre-fault pages with mlock() before migration.")
    elif pte_pct > 40:
        print("  DOMINANT: non-IPI PTE body work (page_remove_rmap atomic ops).")
        print("  NOTE: in a quiescent run this is NOT spinlock contention.")
        print("  Run T4 with Bench-E loaded CSV to measure actual H2 contention.")
    else:
        print("  Cost is roughly evenly distributed across IPI, PTE work,")
        print("  and structural walk. All three sub-components are worth optimising.")

    print()
    print("  NOTE: H2 (actual pte_lockptr *contention*) is only visible under")
    print("  concurrent load. Run: python3 analyze_unmap.py --t4 quiescent.csv loaded.csv")
    print("  where loaded.csv comes from a Bench-E run.")

    # online_cpus correlation
    if "online_cpus" in mapped[0]:
        by_ncpu: dict[int, list[int]] = {}
        for r in mapped:
            nc = r.get("online_cpus", 0)
            by_ncpu.setdefault(nc, []).append(r.get("ipi_wait_ns", 0))
        if len(by_ncpu) > 1:
            print()
            print("  IPI cost by online_cpus (H5 check):")
            for nc in sorted(by_ncpu):
                mean_ipi = statistics.mean(by_ncpu[nc])
                print(f"    online_cpus={nc}: mean ipi_wait_ns={mean_ipi:.1f} ns  (n={len(by_ncpu[nc])})")


# ---------------------------------------------------------------------------
# Argument parsing and dispatch
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="NUMA migration unmap-stage analysis T1–T5 (x86_64 / Linux 6.1.4)")

    parser.add_argument("--csv",     metavar="FILE",
                        help="Primary timing CSV (used by T2, T3, T5)")
    parser.add_argument("--loaded",  metavar="FILE",
                        help="Under-load CSV for T4 comparison")
    parser.add_argument("--t1-summary", metavar="FILE",
                        help="T1 summary CSV from run_t1.sh")
    parser.add_argument("--vmstat",  metavar="FILE",
                        help="vmstat log for T3 cross-reference")

    parser.add_argument("--t1",  nargs="?", const=True, metavar="FILE",
                        help="Run T1 (pass summary CSV or use --t1-summary)")
    parser.add_argument("--t2",  nargs="?", const=True, metavar="FILE",
                        help="Run T2 (cache thrash)")
    parser.add_argument("--t3",  nargs="?", const=True, metavar="FILE",
                        help="Run T3 (rwsem outliers)")
    parser.add_argument("--t4",  nargs="*", metavar="FILE",
                        help="Run T4: --t4 quiescent.csv loaded.csv")
    parser.add_argument("--t5",  nargs="?", const=True, metavar="FILE",
                        help="Run T5 (full decomposition, rmap patch required)")
    parser.add_argument("--all", action="store_true",
                        help="Run all available tests using --csv / --loaded / --t1-summary")

    args = parser.parse_args()

    if len(sys.argv) == 1:
        parser.print_help()
        sys.exit(0)

    # Resolve test targets
    run_t1 = args.t1 is not None or (args.all and args.t1_summary)
    run_t2 = args.t2 is not None or args.all
    run_t3 = args.t3 is not None or args.all
    run_t4 = args.t4 is not None or (args.all and args.loaded)
    run_t5 = args.t5 is not None or args.all

    # Resolve CSV paths
    def csv_path_for(flag, fallback):
        """Return path from flag if it's a string, else fallback."""
        if isinstance(flag, str):
            return flag
        return fallback

    primary_csv_path = csv_path_for(args.t2, None) or \
                       csv_path_for(args.t3, None) or \
                       csv_path_for(args.t5, None) or \
                       args.csv

    t1_path = csv_path_for(args.t1, None) or args.t1_summary

    # ---- T1 ----
    if run_t1:
        if not t1_path:
            print("T1: specify summary CSV via --t1 <file> or --t1-summary <file>")
        else:
            test_t1(t1_path)
            print()

    # ---- T2 ----
    if run_t2:
        src = csv_path_for(args.t2, primary_csv_path)
        if not src:
            print("T2: specify CSV via --t2 <file> or --csv <file>")
        else:
            rows = load_csv(src)
            test_t2(rows)
            print()

    # ---- T3 ----
    if run_t3:
        src = csv_path_for(args.t3, primary_csv_path)
        if not src:
            print("T3: specify CSV via --t3 <file> or --csv <file>")
        else:
            rows = load_csv(src)
            test_t3(rows, vmstat_log=args.vmstat)
            print()

    # ---- T4 ----
    if run_t4:
        if isinstance(args.t4, list) and len(args.t4) >= 2:
            q_path, l_path = args.t4[0], args.t4[1]
        elif primary_csv_path and args.loaded:
            q_path, l_path = primary_csv_path, args.loaded
        else:
            print("T4: provide two CSVs via --t4 quiescent.csv loaded.csv "
                  "or --csv Q --loaded L")
            q_path = l_path = None
        if q_path and l_path:
            q_rows = load_csv(q_path)
            l_rows = load_csv(l_path)
            test_t4(q_rows, l_rows)
            print()

    # ---- T5 ----
    if run_t5:
        src = csv_path_for(args.t5, primary_csv_path)
        if not src:
            print("T5: specify CSV via --t5 <file> or --csv <file>")
        else:
            rows = load_csv(src)
            test_t5(rows)
            print()

    divline()
    print("Analysis complete.")


if __name__ == "__main__":
    main()

