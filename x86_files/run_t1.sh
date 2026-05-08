#!/usr/bin/env bash
# run_t1.sh — T1: IPI Isolation via CPU Hotplug
# CS614 / x86_64 / Linux 6.1.4
#
# Takes CPUs offline one at a time and runs Benchmark A (512 × 4KB pages)
# at each online CPU count. Produces a summary CSV that analyze_unmap.py
# uses to determine whether unmap_ns scales linearly with CPU count (H1).
#
# Usage:
#   sudo bash run_t1.sh [output_dir]
#
# Output:
#   <output_dir>/t1_summary.csv          — online_cpus, mean_unmap_ns, ...
#   <output_dir>/timing_cpus_N.csv       — raw mig_timing data at N CPUs
#
# Requirements:
#   - Root (needed for CPU hotplug: echo 0 > /sys/devices/.../online)
#   - Instrumented kernel with /sys/kernel/debug/mig_timing
#   - mig_bench_x86 binary in current dir or $BENCH_BIN
#   - Python3 + numpy (for per-run summary stats)
#
# How it works:
#   1. Count online CPUs (N_start)
#   2. Run Bench A, collect timing data → compute mean unmap_ns
#   3. Take the highest-numbered CPU offline
#   4. Repeat until only 2 CPUs remain (minimum for NUMA migration)
#   5. Write summary CSV + restore all CPUs
#
# x86_64 interpretation:
#   If unmap_ns grows linearly with N_online (R² > 0.85 in analyze_unmap.py --t1):
#     → H1 confirmed: flush_tlb_others() IPI cost dominates.
#     → Each additional CPU adds ~3-10µs per-IPI ACK round-trip to unmap time.
#   If R² < 0.30:
#     → IPI is not the bottleneck; investigate H2/H3/H4.

set -euo pipefail

OUT_DIR="${1:-./t1_results}"
BENCH_BIN="${BENCH_BIN:-./mig_bench}"
PAGES="${PAGES:-512}"
DEBUGFS_TIMING="/sys/kernel/debug/mig_timing"
MIN_CPUS=2

# ── Preflight checks ──────────────────────────────────────────────────────────

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: must run as root (CPU hotplug requires root)"
    exit 1
fi

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "ERROR: benchmark binary not found: $BENCH_BIN"
    echo "  Build with: gcc -O2 -Wall -o mig_bench_x86 mig_bench_x86.c -lnuma -lpthread"
    echo "  Or set: export BENCH_BIN=/path/to/mig_bench_x86"
    exit 1
fi

if [[ ! -r "$DEBUGFS_TIMING" ]]; then
    echo "ERROR: $DEBUGFS_TIMING not accessible"
    echo "  Is the instrumented kernel running?"
    echo "  Is debugfs mounted? sudo mount -t debugfs none /sys/kernel/debug"
    exit 1
fi

mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/t1_summary.csv"
echo "online_cpus,n_samples,mean_unmap_ns,median_unmap_ns,p95_unmap_ns,p05_unmap_ns,std_unmap_ns,mean_try_migrate_ns,mean_total_ns" > "$SUMMARY"

# ── Helper: compute stats from a timing CSV ───────────────────────────────────

compute_stats() {
    local csv="$1"
    python3 - "$csv" << 'PYEOF'
import sys, csv, numpy as np

path = sys.argv[1]
rows = []
with open(path) as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            if int(row.get('page_was_mapped', 1)) == 1 and int(row.get('result', 0)) == 0:
                rows.append({
                    'unmap':       int(row['unmap_ns']),
                    'try_migrate': int(row.get('try_migrate_ns', 0)),
                    'total':       int(row['total_ns']),
                })
        except (ValueError, KeyError):
            continue

if not rows:
    print("0,0,0,0,0,0,0")
    sys.exit(0)

unmap = np.array([r['unmap'] for r in rows], dtype=float)
tm    = np.array([r['try_migrate'] for r in rows], dtype=float)
tot   = np.array([r['total'] for r in rows], dtype=float)

print(f"{len(rows)},{np.mean(unmap):.1f},{np.median(unmap):.1f},"
      f"{np.percentile(unmap,95):.1f},{np.percentile(unmap,5):.1f},"
      f"{np.std(unmap):.1f},{np.mean(tm):.1f},{np.mean(tot):.1f}")
PYEOF
}

# ── Helper: list all hotpluggable CPUs (all except CPU 0 which is BSP) ────────

get_hotplug_cpus() {
    local cpus=()
    for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*/; do
        cpu_num=$(basename "$cpu_dir" | tr -d 'cpu')
        [[ "$cpu_num" == "0" ]] && continue   # BSP — never offline
        [[ -f "${cpu_dir}online" ]] && cpus+=("$cpu_num")
    done
    # Return in descending order (take highest offline first)
    printf '%s\n' "${cpus[@]}" | sort -rn
}

get_online_count() {
    # Count files whose content is exactly "1" — use awk to sum, not grep -c
    # (grep -c with multiple files returns "file:count" not a plain number)
    local count=0
    for f in /sys/devices/system/cpu/cpu[0-9]*/online; do
        [[ -f "$f" ]] && [[ "$(cat "$f" 2>/dev/null)" == "1" ]] && count=$((count + 1))
    done
    # Always add 1 for CPU0 which has no online file but is always online
    echo $((count + 1))
}

# ── Main loop ─────────────────────────────────────────────────────────────────

ALL_CPUS=($(get_hotplug_cpus))
echo "T1: CPU hotplug IPI isolation test"
echo "  Output dir  : $OUT_DIR"
echo "  Benchmark   : $BENCH_BIN"
echo "  Pages/run   : $PAGES"
echo "  Min CPUs    : $MIN_CPUS"
echo "  Hotpluggable: ${ALL_CPUS[*]}"
echo ""

TAKEN_OFFLINE=()

run_at_current_cpus() {
    local n_online
    n_online=$(get_online_count)
    local raw_csv="$OUT_DIR/timing_cpus_${n_online}.csv"

    echo "  ── Run at $n_online online CPUs ──────────────────────"

    # KEY FIX: mig_bench calls timing_read() after each sub-benchmark,
    # which reads AND resets the kernel ring buffer internally. By the
    # time the script does 'cat $DEBUGFS_TIMING', only ~68 Bench-D
    # records remain, NOT the 512 Bench-A records needed for T1.
    #
    # Fix: run mig_bench in a temp dir and copy timing_4kb_512.csv,
    # which mig_bench writes directly after Benchmark A completes.
    local run_dir bench_abs
    run_dir=$(mktemp -d)
    bench_abs=$(realpath "$BENCH_BIN")

    timeout 180 bash -c "cd '$run_dir' && '$bench_abs'" > /dev/null 2>&1 || true

    if [[ -f "$run_dir/timing_4kb_512.csv" ]]; then
        cp "$run_dir/timing_4kb_512.csv" "$raw_csv"
        echo "  (sourced timing_4kb_512.csv from mig_bench output)"
    else
        echo "  WARNING: timing_4kb_512.csv missing in $run_dir"
        echo "  Files: $(ls $run_dir 2>/dev/null | tr '\n' ' ')"
        cat "$DEBUGFS_TIMING" > "$raw_csv"  # fallback
    fi
    rm -rf "$run_dir"

    local n_rows
    n_rows=$(tail -n +2 "$raw_csv" | wc -l)
    echo "  Collected $n_rows Bench-A records → $raw_csv"

    # Compute stats
    local stats
    stats=$(compute_stats "$raw_csv")
    echo "${n_online},${stats}" >> "$SUMMARY"

    IFS=',' read -r n_s mean_u med_u p95_u p05_u std_u mean_tm mean_tot <<< "$stats"
    python3 -c "
n_s='$n_s'; mean_u=$mean_u; p95_u=$p95_u; mean_tm=$mean_tm
print(f'  n={n_s:<6}  mean_unmap={mean_u/1000:<8.2f} µs  p95={p95_u/1000:<8.2f} µs  mean_try_mig={mean_tm/1000:<8.2f} µs')
" 2>/dev/null || echo "  n=$n_s  mean_unmap=${mean_u}ns  p95=${p95_u}ns"
}

# Run at full CPU count first
run_at_current_cpus

# Progressively take CPUs offline
for cpu in "${ALL_CPUS[@]}"; do
    n_current=$(get_online_count)
    if [[ $n_current -le $MIN_CPUS ]]; then
        echo "  Reached minimum CPU count ($MIN_CPUS), stopping."
        break
    fi

    echo ""
    echo "  Taking CPU $cpu offline..."
    if echo 0 > "/sys/devices/system/cpu/cpu${cpu}/online" 2>/dev/null; then
        TAKEN_OFFLINE+=("$cpu")
        sleep 0.5   # Let scheduler settle after CPU removal
        run_at_current_cpus
    else
        echo "  WARNING: could not take CPU $cpu offline (may be required by kernel)"
    fi
done

# ── Restore all CPUs ──────────────────────────────────────────────────────────

echo ""
echo "  Restoring CPUs..."
for cpu in "${TAKEN_OFFLINE[@]}"; do
    echo 1 > "/sys/devices/system/cpu/cpu${cpu}/online" 2>/dev/null && \
        echo "  CPU $cpu → online" || \
        echo "  WARNING: could not restore CPU $cpu"
done

echo ""
echo "══════════════════════════════════════════════════════"
echo "  T1 complete."
echo "  Summary CSV: $SUMMARY"
echo "  Raw CSVs   : $OUT_DIR/timing_cpus_*.csv"
echo ""
echo "  Analyse with:"
echo "    python3 analyze_unmap.py --t1 $SUMMARY"
echo "══════════════════════════════════════════════════════"

