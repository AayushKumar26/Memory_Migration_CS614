#!/usr/bin/env bash
# run_arm64_full.sh — CS614 ARM64 Copy-Optimization Artifact: Full Evaluation Script
# Target: ARM64, Linux 6.1.4, emulated NUMA (numa=fake=2)
#
# Usage:
#   sudo bash run_arm64_full.sh [--quick] [--results-dir DIR] [--skip-rocm]
#
# Options:
#   --quick        Run Bench E config 0 only. Total runtime ~10 min vs ~55 min.
#   --results-dir  Where to write all CSVs and plots. Default: ./results_arm64
#   --skip-rocm    Skip ROCM benchmark (use if mig_bench_rocm not built yet)
#
# Human-time estimate:    ~10 minutes (setup checks + watching output)
# Compute-time estimate:  ~55 minutes full  |  ~10 minutes with --quick
#
# Benchmark flow:
#   [Build] mig_bench + mig_bench_e + mig_bench_rocm (if present)
#   [A] Bench A: 4KB quiescent, 512 + 2048 pages  → timing_4kb_*.csv
#   [B] Bench B: 2MB THP, 32 + 128 THPs           → timing_2mb_*.csv (if THP works)
#   [C] Bench C: Shared-page sweep deg 1–64        → timing_shared_deg*.csv
#   [D] Bench D: Migration downtime                → downtime_*.csv
#   [Analyze A-D] analyze_timing.py + analyze_downtime.py
#   [E] Bench E: 7 concurrent-load configs         → bench_e_*.csv
#   [ROCM] ROCM vs standard comparison             → rocm_*.csv + rocm_comparison.txt

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ─────────────────────────────────────────────────────────────────────────────

QUICK=0
SKIP_ROCM=0
RESULTS_DIR="./results_arm64"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)        QUICK=1 ;;
        --skip-rocm)    SKIP_ROCM=1 ;;
        --results-dir)  RESULTS_DIR="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Resolve RESULTS_DIR to absolute path BEFORE any pushd ──────────────────
# FIX: the original script used a relative LOG path which broke inside pushd.
# By converting to absolute here, tee -a "$LOG" works from any directory.
mkdir -p "${RESULTS_DIR}"
RESULTS_DIR="$(cd "${RESULTS_DIR}" && pwd)"
PLOTS_DIR="${RESULTS_DIR}/plots"
LOG="${RESULTS_DIR}/run_arm64.log"
mkdir -p "${PLOTS_DIR}"

# Create log file before any tee calls
: > "$LOG"

# ─────────────────────────────────────────────────────────────────────────────
# Colour helpers
# ─────────────────────────────────────────────────────────────────────────────

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
CYN='\033[0;36m'; RST='\033[0m'

info()  { echo -e "${CYN}[INFO]${RST}  $*" | tee -a "$LOG"; }
ok()    { echo -e "${GRN}[OK]${RST}    $*" | tee -a "$LOG"; }
warn()  { echo -e "${YLW}[WARN]${RST}  $*" | tee -a "$LOG"; }
fail()  { echo -e "${RED}[FAIL]${RST}  $*" | tee -a "$LOG"; exit 1; }

header() {
    local msg="$*"
    {
        echo ""
        echo -e "${CYN}══════════════════════════════════════════════════════${RST}"
        echo -e "${CYN}  ${msg}${RST}"
        echo -e "${CYN}══════════════════════════════════════════════════════${RST}"
        echo ""
    } | tee -a "$LOG"
}

header "CS614 ARM64 Copy-Stage Optimization — Artifact Evaluation"
info "Results directory : ${RESULTS_DIR}"
info "Log file          : ${LOG}"
info "Quick mode        : $( [[ $QUICK -eq 1 ]]     && echo YES || echo NO )"
info "Skip ROCM         : $( [[ $SKIP_ROCM -eq 1 ]] && echo YES || echo NO )"
info "Timestamp         : $(date)"

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Preflight checks
# ─────────────────────────────────────────────────────────────────────────────

header "Step 1: Preflight checks"

[[ $EUID -eq 0 ]] || fail "Must run as root: sudo bash $0"
ok "Running as root"

ARCH=$(uname -m)
[[ "$ARCH" == "aarch64" ]] || warn "Expected aarch64, got ${ARCH}"
ok "Architecture: ${ARCH}"

KVER=$(uname -r)
info "Kernel version: ${KVER}"
[[ "$KVER" == *"6.1"* ]] || warn "Expected 6.1.x kernel — got ${KVER}"

DEBUGFS="/sys/kernel/debug/mig_timing"
if [[ ! -e "$DEBUGFS" ]]; then
    info "Mounting debugfs..."
    mount -t debugfs none /sys/kernel/debug || fail "Cannot mount debugfs"
fi
[[ -r "$DEBUGFS" && -w "$DEBUGFS" ]] || \
    fail "Cannot read/write ${DEBUGFS} — is the instrumented kernel running?"
ok "Debugfs: ${DEBUGFS} accessible"

# Check whether ROCM kernel patch is active (rocm_path column in CSV header)
if head -1 "$DEBUGFS" 2>/dev/null | grep -q "rocm_path"; then
    HAVE_ROCM_KERNEL=1
    ok "ROCM kernel patch detected — rocm_path column present in debugfs"
else
    HAVE_ROCM_KERNEL=0
    info "ROCM kernel patch NOT detected — ROCM benchmark will be skipped"
    info "  Apply migrate_rocm.c + rebuild kernel to enable ROCM testing"
    SKIP_ROCM=1
fi

if ! command -v numactl &>/dev/null; then
    fail "numactl not found — install: sudo apt install libnuma-dev"
fi
NUMNODES=$(numactl --hardware | grep -c "^node [0-9]" || true)
[[ "$NUMNODES" -lt 2 ]] && \
    fail "Need ≥2 NUMA nodes. Found: ${NUMNODES}. Boot with numa=fake=2."
ok "NUMA nodes: ${NUMNODES}"

python3 -c "import numpy" 2>/dev/null || fail "numpy missing: pip3 install numpy"
ok "Python3 + numpy available"
if python3 -c "import matplotlib" 2>/dev/null; then
    ok "matplotlib available — plots will be generated"
else
    warn "matplotlib not found — text-only output (pip3 install matplotlib)"
fi

echo 0 | tee /proc/sys/kernel/numa_balancing > /dev/null
ok "NUMA auto-balancing disabled"

# THP — enable always mode; note VM limitation
echo always | tee /sys/kernel/mm/transparent_hugepage/enabled > /dev/null 2>&1 || true
echo always | tee /sys/kernel/mm/transparent_hugepage/defrag  > /dev/null 2>&1 || true
sysctl -w vm.nr_hugepages=256 > /dev/null 2>&1 || true
THP_STATUS=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "unknown")
info "THP status: ${THP_STATUS}"
info "NOTE: numa=fake=2 kernels often split THPs before migration — Bench B may show"
info "      0 order-9 records. This is a known VM limitation, not a benchmark failure."

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Build userspace binaries
# ─────────────────────────────────────────────────────────────────────────────

header "Step 2: Build userspace benchmarks"

BENCH_SRC="${SCRIPT_DIR}/mig_bench.c"
BENCH_E_SRC="${SCRIPT_DIR}/mig_bench_e.c"
BENCH_ROCM_SRC="${SCRIPT_DIR}/mig_bench_rocm.c"
BENCH_ROCM_MT_SRC="${SCRIPT_DIR}/mig_bench_rocm_mt.c"

[[ -f "$BENCH_SRC" ]]   || fail "mig_bench.c not found at ${BENCH_SRC}"
[[ -f "$BENCH_E_SRC" ]] || fail "mig_bench_e.c not found at ${BENCH_E_SRC}"

info "Building mig_bench (Benchmarks A, B, C, D)..."
gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench" "$BENCH_SRC" -lnuma -lpthread \
    2>&1 | tee -a "$LOG"
ok "mig_bench built → ${RESULTS_DIR}/mig_bench"

info "Building mig_bench_e (Benchmark E)..."
gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench_e" "$BENCH_E_SRC" -lnuma -lpthread -lm \
    2>&1 | tee -a "$LOG"
ok "mig_bench_e built → ${RESULTS_DIR}/mig_bench_e"

if [[ $SKIP_ROCM -eq 0 ]]; then
    if [[ -f "$BENCH_ROCM_SRC" ]]; then
        info "Building mig_bench_rocm..."
        gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench_rocm" "$BENCH_ROCM_SRC" \
            -lnuma -lm 2>&1 | tee -a "$LOG"
        ok "mig_bench_rocm built"
    else
        warn "mig_bench_rocm.c not found — ROCM single-thread test skipped"
    fi

    if [[ -f "$BENCH_ROCM_MT_SRC" ]]; then
        info "Building mig_bench_rocm_mt..."
        gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench_rocm_mt" "$BENCH_ROCM_MT_SRC" \
            -lnuma -lm 2>&1 | tee -a "$LOG"
        ok "mig_bench_rocm_mt built"
    else
        warn "mig_bench_rocm_mt.c not found — ROCM MT test skipped"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Helper: run a binary from RESULTS_DIR with optional extra args
# FIX: original used "${args[@]:-}" which fails on empty arrays in strict mode.
#      Now uses explicit $# check before passing args.
# FIX: LOG is now absolute so tee -a works correctly after pushd.
# ─────────────────────────────────────────────────────────────────────────────

run_bench() {
    local bin="$1"
    local label="$2"
    shift 2

    info "Running: ${bin}${*:+ $*}"
    echo 1 > "$DEBUGFS"
    sleep 0.1

    pushd "${RESULTS_DIR}" > /dev/null
    if [[ $# -gt 0 ]]; then
        "./${bin}" "$@" 2>&1 | tee -a "$LOG"
    else
        "./${bin}" 2>&1 | tee -a "$LOG"
    fi
    popd > /dev/null
    ok "Completed: ${label}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Benchmarks A, B, C, D
# ─────────────────────────────────────────────────────────────────────────────

header "Step 3: Benchmarks A, B, C, D — Quiescent + Downtime"
info "Estimated runtime: ~10 minutes"
info "NOTE: timing_4kb_2048.csv is the quiescent baseline for Bench E comparison"

run_bench mig_bench "Benchmarks A+B+C+D"

# Bench A
for f in timing_4kb_512.csv timing_4kb_2048.csv; do
    if [[ -f "${RESULTS_DIR}/${f}" ]]; then
        ok "${f}: $(wc -l < "${RESULTS_DIR}/${f}") records"
    else
        warn "${f} not found"
    fi
done

# Bench B — check for real THP records (order-9)
for f in timing_2mb_32.csv timing_2mb_128.csv; do
    if [[ -f "${RESULTS_DIR}/${f}" ]]; then
        NLINES=$(wc -l < "${RESULTS_DIR}/${f}")
        REAL_THP=$(grep -c ",9," "${RESULTS_DIR}/${f}" 2>/dev/null || echo 0)
        if [[ "$REAL_THP" -gt 0 ]]; then
            ok "${f}: ${NLINES} records (${REAL_THP} real THP order-9)"
        else
            warn "${f}: ${NLINES} records but 0 order-9 THP records"
            warn "  Kernel split all THPs — known numa=fake=2 limitation"
            warn "  Analytical THP projection will be used in analysis"
        fi
    else
        warn "${f} not found"
    fi
done

# Bench C — FIX: actual filenames are zero-padded (deg001, deg016, deg064)
# Original script checked deg1, deg16, deg64 which do not exist
NDEG=0
for deg in 001 002 004 008 016 032 064; do
    f="${RESULTS_DIR}/timing_shared_deg${deg}.csv"
    if [[ -f "$f" ]]; then
        NDEG=$((NDEG+1))
        ok "timing_shared_deg${deg}.csv: $(wc -l < "$f") records"
    else
        warn "timing_shared_deg${deg}.csv missing"
    fi
done
[[ $NDEG -eq 7 ]] && ok "All 7 sharing degree CSVs present" || \
    warn "Only ${NDEG}/7 sharing degree CSVs found"

# Bench D — downtime
for f in downtime_samenode.csv downtime_crossnode.csv; do
    if [[ -f "${RESULTS_DIR}/${f}" ]]; then
        ok "${f}: $(wc -l < "${RESULTS_DIR}/${f}") records"
    else
        info "${f} not found (cross-node requires CPU on node1)"
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# Step 4: Analyse stage timing (A, B, C)
# ─────────────────────────────────────────────────────────────────────────────

header "Step 4: Analyse Stage Timing (Benchmarks A, B, C)"

TIMING_SCRIPT="${SCRIPT_DIR}/analyze_timing.py"
[[ -f "$TIMING_SCRIPT" ]] || fail "analyze_timing.py not found at ${TIMING_SCRIPT}"

TIMING_CSVS=()
for f in "${RESULTS_DIR}"/timing_4kb_512.csv \
          "${RESULTS_DIR}"/timing_4kb_2048.csv; do
    [[ -f "$f" ]] && TIMING_CSVS+=("$f")
done
# Only include THP CSVs if they contain real THP records
for f in "${RESULTS_DIR}"/timing_2mb_32.csv \
          "${RESULTS_DIR}"/timing_2mb_128.csv; do
    if [[ -f "$f" ]]; then
        REAL=$(grep -c ",9," "$f" 2>/dev/null || echo 0)
        [[ "$REAL" -gt 0 ]] && TIMING_CSVS+=("$f") || \
            info "Excluding ${f##*/} — no real THP records (will use analytical projection)"
    fi
done
# Shared degree CSVs — use glob which matches zero-padded names correctly
for f in "${RESULTS_DIR}"/timing_shared_deg*.csv; do
    [[ -f "$f" ]] && TIMING_CSVS+=("$f")
done

[[ ${#TIMING_CSVS[@]} -eq 0 ]] && \
    fail "No timing CSVs found — benchmarks did not produce output"

info "Running analyze_timing.py on ${#TIMING_CSVS[@]} CSV files..."
pushd "${RESULTS_DIR}" > /dev/null
python3 "$TIMING_SCRIPT" "${TIMING_CSVS[@]}" 2>&1 | tee -a "$LOG"
find . -maxdepth 1 -name "*.png" -exec mv {} "${PLOTS_DIR}/" \; 2>/dev/null || true
popd > /dev/null
ok "Stage timing analysis complete — plots in ${PLOTS_DIR}/"

# ARM64 sanity check
info "Checking ARM64 stage profile (copy should be ~40–46%, unmap ~31–42%)..."
python3 - <<PY 2>&1 | tee -a "$LOG"
import csv, statistics, sys

def load(path):
    try:
        return [r for r in csv.DictReader(open(path))
                if r.get('copy_ns') and r.get('try_migrate_ns')]
    except Exception:
        return []

rows = load("${RESULTS_DIR}/timing_4kb_2048.csv")
if not rows:
    print("  [WARN] Could not load timing_4kb_2048.csv")
    sys.exit(0)

def pct(stage):
    s = [float(r[stage]) for r in rows]
    t = [float(r['try_migrate_ns']) for r in rows if float(r['try_migrate_ns']) > 0]
    return statistics.mean(s) / statistics.mean(t) * 100 if t else 0.0

copy_pct  = pct('copy_ns')
unmap_pct = pct('unmap_ns')
print(f"  Copy  stage: {copy_pct:.1f}%  (expected ~40–46%)")
print(f"  Unmap stage: {unmap_pct:.1f}%  (expected ~31–42%)")
if copy_pct >= 30:
    print("  ✓ ARM64 copy-dominated profile confirmed")
elif unmap_pct >= 30:
    print("  ~ Unmap-dominated — TLBI cost visible (still valid)")
else:
    print("  ✗ Unexpected profile — verify instrumented kernel is booted (uname -r)")
PY

# ─────────────────────────────────────────────────────────────────────────────
# Step 5: Analyse downtime (Benchmark D)
# ─────────────────────────────────────────────────────────────────────────────

header "Step 5: Analyse Migration Downtime (Benchmark D)"

DOWNTIME_SCRIPT="${SCRIPT_DIR}/analyze_downtime.py"
if [[ ! -f "$DOWNTIME_SCRIPT" ]]; then
    warn "analyze_downtime.py not found at ${DOWNTIME_SCRIPT} — skipping"
else
    DOWNTIME_CSVS=()
    for f in "${RESULTS_DIR}"/downtime_samenode.csv \
              "${RESULTS_DIR}"/downtime_crossnode.csv; do
        [[ -f "$f" ]] && DOWNTIME_CSVS+=("$f")
    done

    if [[ ${#DOWNTIME_CSVS[@]} -gt 0 ]]; then
        info "Running analyze_downtime.py on ${#DOWNTIME_CSVS[@]} files..."
        pushd "${RESULTS_DIR}" > /dev/null
        python3 "$DOWNTIME_SCRIPT" "${DOWNTIME_CSVS[@]}" 2>&1 | tee -a "$LOG"
        find . -maxdepth 1 -name "*.png" -exec mv {} "${PLOTS_DIR}/" \; 2>/dev/null || true
        popd > /dev/null
        ok "Downtime analysis complete"
    else
        warn "No downtime CSVs found — Bench D did not produce output"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 6: Benchmark E — Concurrent load
# ─────────────────────────────────────────────────────────────────────────────

header "Step 6: Benchmark E — Concurrent Load"

if [[ $QUICK -eq 1 ]]; then
    info "QUICK mode: running config 0 only (E_rand_rmw_t4_c512)"
    info "Estimated runtime: ~5 minutes"
    run_bench mig_bench_e "Benchmark E config 0 (quick)" 0
else
    info "Running all 7 Benchmark E configurations"
    info "Estimated runtime: ~50 minutes"
    run_bench mig_bench_e "Benchmark E (all 7 configs)"
fi

MISSING_E=0
for label in E_rand_rmw_t4_c512 E_seq_read_t4_c512 E_rand_read_t4_c512 \
             E_rand_rmw_t1_c512 E_rand_rmw_t8_c512 E_rand_rmw_t4_c1 E_rand_rmw_t4_c64; do
    [[ $QUICK -eq 1 && "$label" != "E_rand_rmw_t4_c512" ]] && continue
    for suffix in workers migrator timing; do
        f="${RESULTS_DIR}/bench_e_${suffix}_${label}.csv"
        if [[ -f "$f" ]]; then
            ok "${f##*/}: $(wc -l < "$f") records"
        else
            warn "Missing: ${f##*/}"
            MISSING_E=$((MISSING_E+1))
        fi
    done
done
[[ $MISSING_E -gt 0 ]] && warn "${MISSING_E} Bench E output file(s) missing" || \
    ok "All expected Bench E output files present"

# ─────────────────────────────────────────────────────────────────────────────
# Step 7: Quiescent vs under-load stage comparison
# ─────────────────────────────────────────────────────────────────────────────

header "Step 7: Quiescent vs Under-Load Stage Comparison"

QUIESCENT_CSV="${RESULTS_DIR}/timing_4kb_2048.csv"
LOADED_CSV="${RESULTS_DIR}/bench_e_timing_E_rand_rmw_t4_c512.csv"

if [[ -f "$QUIESCENT_CSV" && -f "$LOADED_CSV" ]]; then
    info "Comparing quiescent (Bench A) vs under-load (Bench E primary config)..."
    python3 - <<PY 2>&1 | tee -a "$LOG"
import csv, statistics

def load(path):
    return list(csv.DictReader(open(path)))

def stage_stats(rows, stage, total='try_migrate_ns'):
    s = [float(r[stage]) for r in rows
         if stage in r and total in r and float(r.get(total, 0)) > 0]
    t = [float(r[total]) for r in rows
         if stage in r and total in r and float(r.get(total, 0)) > 0]
    if not s:
        return float('nan'), float('nan')
    return statistics.mean(s), statistics.mean(s) / statistics.mean(t) * 100

q = load("${QUIESCENT_CSV}")
l = load("${LOADED_CSV}")

print(f"\n{'Stage':<12} {'Quiescent mean':>18} {'Q%':>8} {'Loaded mean':>14} {'L%':>8}")
print("-" * 65)
for stage in ['lock_ns', 'unmap_ns', 'copy_ns', 'remap_ns', 'unlock_ns']:
    qm, qp = stage_stats(q, stage)
    lm, lp = stage_stats(l, stage)
    print(f"{stage.replace('_ns',''):<12} {qm:>15,.0f} ns  {qp:>6.1f}%  {lm:>11,.0f} ns  {lp:>6.1f}%")

q_copy = statistics.mean([float(r['copy_ns']) for r in q if 'copy_ns' in r])
l_copy = statistics.mean([float(r['copy_ns']) for r in l if 'copy_ns' in r])
delta  = l_copy - q_copy
print(f"\nCopy delta (loaded - quiescent): {delta:+,.0f} ns")
if delta < 0:
    print("  ✓ Copy_ns FALLS under load — workers stall on migration PTEs,")
    print("    quieting the memory bus (expected ARM64 behaviour)")
else:
    print("  ~ Copy_ns did not fall — likely due to only 2 workers (node1 CPU limit)")
PY
    ok "Stage comparison complete"
else
    warn "Skipping stage comparison — need both ${QUIESCENT_CSV##*/} and ${LOADED_CSV##*/}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 8: ROCM benchmark (only if patched kernel active)
# ─────────────────────────────────────────────────────────────────────────────

header "Step 8: ROCM vs Standard Migration Comparison"

if [[ $SKIP_ROCM -eq 1 ]]; then
    info "ROCM tests skipped (kernel patch not detected or --skip-rocm passed)"
    info "  To enable: apply migrate_rocm.c, rebuild kernel, reboot, re-run script"
else
    ROCM_RAN=0

    if [[ -x "${RESULTS_DIR}/mig_bench_rocm" ]]; then
        info "Running ROCM single-threaded comparison..."
        info "  Expected: rocm_path=1 records in rocm_fast_timing.csv"
        info "  Expected: stall window drops from ~1100 ns → ~420 ns (one TLBI round-trip)"
        pushd "${RESULTS_DIR}" > /dev/null
        ./mig_bench_rocm 2>&1 | tee -a "$LOG"
        popd > /dev/null
        ROCM_RAN=1
        ok "ROCM single-threaded test complete → rocm_comparison.txt"
    else
        warn "mig_bench_rocm not built — skipping"
    fi

    if [[ -x "${RESULTS_DIR}/mig_bench_rocm_mt" ]]; then
        info "Running ROCM multithreaded comparison..."
        info "  Run A: writable alias → standard pipeline"
        info "  Run B: read-only mapping → ROCM fast path"
        pushd "${RESULTS_DIR}" > /dev/null
        ./mig_bench_rocm_mt 2>&1 | tee -a "$LOG"
        popd > /dev/null
        ROCM_RAN=1
        ok "ROCM multithreaded test complete"
    else
        warn "mig_bench_rocm_mt not built — skipping"
    fi

    # Verify rocm_path=1 records were produced
    if [[ $ROCM_RAN -eq 1 ]]; then
        ROCM_FAST="${RESULTS_DIR}/rocm_fast_timing.csv"
        if [[ -f "$ROCM_FAST" ]]; then
            ROCM_COUNT=$(grep -c ",1$" "$ROCM_FAST" 2>/dev/null || echo 0)
            if [[ "$ROCM_COUNT" -gt 0 ]]; then
                ok "ROCM fast path confirmed: ${ROCM_COUNT} rocm_path=1 records"
            else
                warn "No rocm_path=1 records — ROCM fast path not firing"
                warn "  Check: pages must be file-backed, clean, PROT_READ only, non-anonymous"
            fi
        fi
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 9: Summary
# ─────────────────────────────────────────────────────────────────────────────

header "Artifact Evaluation Complete"

{
    echo ""
    echo "  ${RESULTS_DIR}/"
    echo "  ├── timing_4kb_512.csv           Bench A — 512 pages quiescent"
    echo "  ├── timing_4kb_2048.csv          Bench A — 2048 pages (Bench E baseline)"
    echo "  ├── timing_2mb_*.csv             Bench B — THP (may be 4KB fallback on this VM)"
    echo "  ├── timing_shared_deg*.csv       Bench C — zero-padded: deg001..deg064"
    echo "  ├── downtime_samenode.csv        Bench D — same-node stall (max ~68–80 µs)"
    echo "  ├── downtime_crossnode.csv       Bench D — cross-node stall"
    echo "  ├── bench_e_workers_*.csv        Bench E — per-thread per-phase app stats"
    echo "  ├── bench_e_migrator_*.csv       Bench E — migration throughput"
    echo "  ├── bench_e_timing_*.csv         Bench E — kernel ring buffer stage samples"
    echo "  ├── rocm_standard_timing.csv     ROCM — standard path ring buffer"
    echo "  ├── rocm_fast_timing.csv         ROCM — ROCM fast path ring buffer"
    echo "  ├── rocm_comparison.txt          ROCM — stall comparison summary"
    echo "  ├── run_arm64.log                Full log of this run"
    echo "  └── plots/                       All PNG plots"
    echo ""
    echo "  Key claim checklist:"
    echo "    Copy-dominated ARM64 : timing_4kb_2048.csv → copy ~40–46%, unmap ~31–42%"
    echo "    rmap walk scaling    : timing_shared_deg*.csv → unmap grows O(mapcount)"
    echo "    App stall (Bench D)  : downtime_samenode.csv → max stall ~62–80 µs"
    echo "    Load disruption      : bench_e_migrator_E_rand_rmw_t4_c512.csv → factor ≈ 1.0"
    echo "    Chunk=1 pathology    : bench_e_workers_E_rand_rmw_t4_c1.csv → p999 spike ~22×"
    echo "    ROCM stall reduction : rocm_comparison.txt → ~1100 ns → ~420 ns"
    echo ""
} | tee -a "$LOG"

# Restore NUMA balancing
echo 1 > /proc/sys/kernel/numa_balancing 2>/dev/null || true

ok "Done. Full log: ${LOG}"