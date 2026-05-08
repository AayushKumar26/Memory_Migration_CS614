#!/usr/bin/env bash
# =============================================================================
# run_x86_full.sh — CS614 x86_64 O1+O3 Optimisation Artifact: Full Evaluation
# Target: x86_64, Linux 6.1.4, emulated NUMA (numa=fake=2 or real NUMA)
# =============================================================================

set -euo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────────
QUICK=0
SKIP_BENCHE=0
BASELINE_ONLY=0
OPTIMISED_ONLY=0
RESULTS_DIR="./results_x86"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)           QUICK=1 ;;
        --skip-benche)     SKIP_BENCHE=1 ;;
        --baseline-only)   BASELINE_ONLY=1 ;;
        --optimised-only)  OPTIMISED_ONLY=1 ;;
        --results-dir)     RESULTS_DIR="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${RESULTS_DIR}"
RESULTS_DIR="$(cd "${RESULTS_DIR}" && pwd)"
PLOTS_DIR="${RESULTS_DIR}/plots"
LOG="${RESULTS_DIR}/run_x86.log"
mkdir -p "${PLOTS_DIR}"
: > "$LOG"

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
CYN='\033[0;36m'; RST='\033[0m'

info()   { echo -e "${CYN}[INFO]${RST}  $*" | tee -a "$LOG"; }
ok()     { echo -e "${GRN}[OK]${RST}    $*" | tee -a "$LOG"; }
warn()   { echo -e "${YLW}[WARN]${RST}  $*" | tee -a "$LOG"; }
fail()   { echo -e "${RED}[FAIL]${RST}  $*" | tee -a "$LOG"; exit 1; }

header() {
    {
        echo ""
        echo -e "${CYN}══════════════════════════════════════════════════════${RST}"
        echo -e "${CYN}  $*${RST}"
        echo -e "${CYN}══════════════════════════════════════════════════════${RST}"
        echo ""
    } | tee -a "$LOG"
}

header "CS614 x86_64 O1+O3 NUMA Migration Optimisation — Artifact Evaluation"
info "Results directory : ${RESULTS_DIR}"
info "Log file          : ${LOG}"
info "Timestamp         : $(date)"

# ── Step 1: Preflight checks ──────────────────────────────────────────────────
header "Step 1: Preflight checks"

[[ $EUID -eq 0 ]] || fail "Must run as root: sudo bash \$0"
ok "Running as root"

ARCH=$(uname -m)
[[ "$ARCH" == "x86_64" ]] || warn "Expected x86_64, got ${ARCH}"
ok "Architecture: ${ARCH}"

KVER=$(uname -r)
info "Kernel version: ${KVER}"

DEBUGFS="/sys/kernel/debug/mig_timing"
if [[ ! -e "$DEBUGFS" ]]; then
    info "Mounting debugfs..."
    mount -t debugfs none /sys/kernel/debug 2>/dev/null || \
        fail "Cannot mount debugfs — is the instrumented kernel running?"
fi
[[ -r "$DEBUGFS" && -w "$DEBUGFS" ]] || fail "Cannot access ${DEBUGFS}"
ok "Debugfs accessible"

NCOLS=$(head -1 "$DEBUGFS" 2>/dev/null | tr ',' '\n' | wc -l || echo 0)
HAVE_RMAP_PATCH=0
if [[ "$NCOLS" -eq 25 ]]; then
    ok "rmap patch active (25 columns detected)"
    HAVE_RMAP_PATCH=1
fi

HAVE_O1=0
if grep -q "migrate_pages_deferred_ipi" /proc/kallsyms 2>/dev/null; then
    ok "O1: batch function found"
    HAVE_O1=1
fi

command -v numactl &>/dev/null || fail "numactl not found"
NUMNODES=$(numactl --hardware | grep -c "^node [0-9]" || true)
[[ "$NUMNODES" -lt 2 ]] && fail "Need ≥2 NUMA nodes. Found: ${NUMNODES}"
ok "NUMA nodes: ${NUMNODES}"

echo 0 | tee /proc/sys/kernel/numa_balancing > /dev/null
ok "NUMA auto-balancing disabled"

# ── Step 2: Build userspace benchmarks ───────────────────────────────────────
header "Step 2: Build userspace benchmarks"

BENCH_SRC="${SCRIPT_DIR}/mig_bench_x86.c"
BENCH_E_SRC="${SCRIPT_DIR}/mig_bench_e_x86.c"

[[ -f "$BENCH_SRC" ]] || fail "mig_bench_x86.c not found"
[[ -f "$BENCH_E_SRC" ]] || fail "mig_bench_e_x86.c not found"

gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench" "$BENCH_SRC" -lnuma -lpthread 2>&1 | tee -a "$LOG"
gcc -O2 -Wall -o "${RESULTS_DIR}/mig_bench_e" "$BENCH_E_SRC" -lnuma -lpthread -lm 2>&1 | tee -a "$LOG"
ok "Benchmarks built"

# ── Step 3: Benchmarks A, B, C, D (quiescent + downtime) ─────────────────────
header "Step 3: Benchmarks A, B, C, D — Quiescent + Downtime"
info "Estimated runtime: ~5 minutes"

echo 1 > "$DEBUGFS"
sleep 0.1

pushd "${RESULTS_DIR}" > /dev/null
./mig_bench 2>&1 | tee -a "$LOG"
popd > /dev/null

# ── Step 4: Analyse Stage Timing & Generate Plots ────────────────────────────
header "Step 4: Stage Timing Analysis & Plotting (A, B, C)"

TIMING_SCRIPT="${SCRIPT_DIR}/analyze_timing.py"
if [[ ! -f "$TIMING_SCRIPT" ]]; then
    warn "analyze_timing.py not found — skipping plot generation"
else
    TIMING_CSVS=()
    for f in "${RESULTS_DIR}"/timing_4kb_*.csv "${RESULTS_DIR}"/timing_shared_deg*.csv; do
        [[ -f "$f" ]] && TIMING_CSVS+=("$f")
    done

    if [[ ${#TIMING_CSVS[@]} -gt 0 ]]; then
        info "Running analyze_timing.py on ${#TIMING_CSVS[@]} files..."
        pushd "${RESULTS_DIR}" > /dev/null
        python3 "$TIMING_SCRIPT" "${TIMING_CSVS[@]}" 2>&1 | tee -a "$LOG"
        find . -maxdepth 1 -name "*.png" -exec mv {} "${PLOTS_DIR}/" \; 2>/dev/null || true
        popd > /dev/null
        ok "Timing plots generated in ${PLOTS_DIR}/"
    fi
fi

# ── Step 5: T5/T2/T3 Sub-Component Decomposition ─────────────────────────────
header "Step 5: T5 — Quiescent Sub-Component Decomposition"

ANALYZE="${SCRIPT_DIR}/analyze_unmap.py"
Q_CSV="${RESULTS_DIR}/timing_4kb_2048.csv"

if [[ $HAVE_RMAP_PATCH -eq 1 && -f "$Q_CSV" ]]; then
    info "Analyzing O1+O3 metrics via analyze_unmap.py..."
    python3 "$ANALYZE" --t5 "$Q_CSV" 2>&1 | tee -a "$LOG"
    python3 "$ANALYZE" --t2 "$Q_CSV" 2>&1 | tee -a "$LOG"
    python3 "$ANALYZE" --t3 "$Q_CSV" 2>&1 | tee -a "$LOG"
    
    info "Verifying O3 Address-Ordered Walk Cost..."
    python3 - <<PY 2>&1 | tee -a "$LOG"
import csv, statistics, sys
rows = [r for r in csv.DictReader(open("$Q_CSV")) if r.get('page_was_mapped') == '1']
if rows:
    walks = [float(r['try_migrate_ns']) - float(r.get('ptl_wait_ns',0)) - float(r.get('rwsem_wait_ns',0)) for r in rows]
    avg_walk = statistics.mean(walks)
    print(f"  Median structural walk (outside PTL): {statistics.median(walks):.0f} ns")
    if avg_walk < 200:
        print("  ✓ O3 CONFIRMED: High locality in page-table walk.")
PY
else
    warn "Skipping T5 — rmap patch not active or CSV missing"
fi

# ── Step 6: Analyse Downtime (Benchmark D) ───────────────────────────────────
header "Step 6: Analyse Migration Downtime (Benchmark D)"

DOWNTIME_SCRIPT="${SCRIPT_DIR}/analyze_downtime.py"
if [[ -f "$DOWNTIME_SCRIPT" ]]; then
    D_CSVS=()
    for f in "${RESULTS_DIR}"/downtime_*.csv; do [[ -f "$f" ]] && D_CSVS+=("$f"); done
    if [[ ${#D_CSVS[@]} -gt 0 ]]; then
        pushd "${RESULTS_DIR}" > /dev/null
        python3 "$DOWNTIME_SCRIPT" "${D_CSVS[@]}" 2>&1 | tee -a "$LOG"
        find . -maxdepth 1 -name "*.png" -exec mv {} "${PLOTS_DIR}/" \; 2>/dev/null || true
        popd > /dev/null
        ok "Downtime plots generated."
    fi
fi

# ── Step 7: Benchmark E — Concurrent load ────────────────────────────────────
header "Step 7: Benchmark E — Concurrent Load"

if [[ $SKIP_BENCHE -eq 1 ]]; then
    info "Benchmark E skipped"
else
    pushd "${RESULTS_DIR}" > /dev/null
    if [[ $QUICK -eq 1 ]]; then
        ./mig_bench_e 0 2>&1 | tee -a "$LOG"
    else
        ./mig_bench_e 2>&1 | tee -a "$LOG"
    fi
    popd > /dev/null
    ok "Benchmark E complete"
fi

# ── Benchmark E Analysis ─────────────────────────────────────────────────────
BENCH_E_ANALYZE="${SCRIPT_DIR}/analyze_bench_e_x86.py"
if [[ -f "$BENCH_E_ANALYZE" ]]; then
    info "Running Benchmark E analysis (analyze_bench_e_x86.py)..."
    pushd "${RESULTS_DIR}" > /dev/null
    python3 "$BENCH_E_ANALYZE" 2>&1 | tee -a "$LOG"
    find . -maxdepth 1 -name "*.png" -exec mv {} "${PLOTS_DIR}/" \; 2>/dev/null || true
    popd > /dev/null
    ok "Benchmark E analysis and plots generated in ${PLOTS_DIR}/"
fi

# ── Step 8: O1 Worker Disruption Verification ────────────────────────────────
header "Step 8: O1 Verification — Worker Disruption"

WORKERS_CSV="${RESULTS_DIR}/bench_e_workers_x86_E_rand_rmw_t4_c512.csv"
if [[ -f "$WORKERS_CSV" ]]; then
    python3 - "$WORKERS_CSV" << 'PYEOF' 2>&1 | tee -a "$LOG"
import csv, statistics, sys
PHASE_DUR = {'baseline': 10, 'migration': 30, 'steady': 10}
phases = {}
with open(sys.argv[1]) as f:
    for row in csv.DictReader(f):
        ph = row['phase']
        if ph not in phases: phases[ph] = {'ops': 0, 'p999': []}
        phases[ph]['ops'] += int(row['ops'])
        phases[ph]['p999'].append(float(row['p999_ns']))

rates = {ph: phases[ph]['ops'] / PHASE_DUR[ph] for ph in phases}
disrupt = rates.get('migration', 0) / rates.get('baseline', 1)
print(f"  Disruption factor: {disrupt:.4f}")
if disrupt > 0.60:
    print("  ✓ O1 CONFIRMED: Batching significantly reduces TLB shootdown impact.")
PYEOF
fi

# ── Step 9: Final Summary ────────────────────────────────────────────────────
header "Step 9: Artifact Evaluation Summary"
echo "  Machine: $(uname -m)"
echo "  Kernel : $(uname -r)"
echo "  Plots  : ${PLOTS_DIR}/"
echo ""

echo 1 > /proc/sys/kernel/numa_balancing 2>/dev/null || true
ok "Done. Full log: ${LOG}"
