# CS614 — x86_64 NUMA Migration Optimisation Artifact

## O1: Deferred Batch TLB Shootdown | O3: Address-Ordered Migration

---

## How to Start

```bash
# 1. Place all files from this folder in one directory, e.g. ~/eval_scripts/
# 2. Copy the modified kernel files and rebuild (see Section 3 below)
# 3. SSH into the machine after reboot and run:

cd ~/eval_scripts
sudo bash run_x86_full.sh --results-dir ./results_x86
```

---

### Required Files — All Must Be in the Same Folder as `run_x86_full.sh`

| File | What it is | Required? |
|------|-----------|-----------|
| `run_x86_full.sh` | Main automation script (9 steps) | **YES** |
| `mig_bench_x86.c` | Userspace Benchmarks A–D source | **YES** |
| `mig_bench_e_x86.c` | Benchmark E source (concurrent load) | **YES** |
| `analyze_unmap.py` | T1–T5 sub-component analysis | **YES** |
| `analyze_timing.py` | Stage breakdown + plots (Step 4) | **YES** |
| `analyze_bench_e_x86.py` | Bench E disruption/throughput plots (Step 7) | **YES** |
| `migrate.c` | Optimised kernel source → copy to `linux-6.1.4/mm/migrate.c` | **YES** |
| `rmap.c` | Optimised kernel source → copy to `linux-6.1.4/mm/rmap.c` | **YES** |
| `migrate_x86.patch` | Patch diff for migrate.c (alternative to full file) | Optional |
| `rmap_x86.patch` | Patch diff for rmap.c (alternative to full file) | Optional |
| `run_t1.sh` | T1 CPU hotplug IPI isolation driver | Optional — auto-runs |

```bash
# Verify all required files are present:
ls ~/eval_scripts/
# Must show: run_x86_full.sh, mig_bench_x86.c (or _nod.c),
#            mig_bench_e_x86.c, analyze_unmap.py
```

---

## Artifact Directory Structure

```
eval_scripts/                   ← put run_x86_full.sh here
├── run_x86_full.sh             automation script (this README's subject)
├── mig_bench_x86.c             Benchmarks A–D source (x86_64) but Bench D disabled
├── mig_bench_e_x86.c           Benchmark E source (concurrent load)
├── analyze_unmap.py            T1–T5 unmap sub-component analysis
├── run_t1.sh                   T1 CPU hotplug driver
├── apply_rmap_patch.py         rmap.c patch helper
└── results_x86/                ← created by the script
    ├── mig_bench               compiled binary
    ├── mig_bench_e             compiled binary
    ├── timing_4kb_512.csv      Bench A — 512 pages
    ├── timing_4kb_2048.csv     Bench A — 2048 pages (main quiescent CSV)
    ├── timing_2mb_*.csv        Bench B — THP
    ├── timing_shared_deg*.csv  Bench C — sharing degree 1–64
    ├── downtime_samenode.csv   Bench D — same-node stall
    ├── bench_e_timing_*.csv    Bench E — migration timing ring buffer
    ├── bench_e_workers_*.csv   Bench E — worker throughput/latency
    ├── bench_e_migrator_*.csv  Bench E — migration rate
    ├── t1_results/             T1 per-CPU-count CSVs
    │   └── t1_summary.csv
    └── run_x86.log             full log of the run

linux-6.1.4/mm/                ← kernel source modifications
├── migrate.c                   O1 batch function + O3 sort + timing
└── rmap.c                      per-CPU IPI/ptl/rwsem sub-timing
```

---

## Setup Instructions

### 1. Hardware Requirements

| Component | Requirement |
|-----------|------------|
| CPU | x86_64, ≥4 logical cores. Tested: i5-1340P (VirtualBox), Xeon Gold 6226R (KVM/QEMU) |
| RAM | Minimum 4 GB (Bench E uses 512 MB buffer) |
| Storage | 30 GB free (kernel source 1.2 GB, build output 8 GB, CSVs <100 MB) |
| NUMA | Emulated (`numa=fake=2`) or real multi-socket. No extra hardware. |

**Compute-time estimate:** ~65 minutes full run | ~15 minutes with `--quick`

**Human-time estimate:** ~15 minutes (setup + watching output)

---

### 2. OS and Software Dependencies

Tested on: **Ubuntu 22.04 LTS** (VirtualBox 7.x guest and QEMU/KVM guest)

```bash
sudo apt update && sudo apt install -y \
    build-essential libncurses-dev bison flex libssl-dev \
    libelf-dev bc dwarves libnuma-dev \
    python3-numpy python3-matplotlib git
```

---

### 3. Kernel Setup (one-time, ~30 min)

#### 3a. Add NUMA emulation boot parameter

```bash
# Edit GRUB:
sudo nano /etc/default/grub
# Change to:
GRUB_CMDLINE_LINUX="numa=fake=2"

sudo update-grub && sudo reboot

# Verify after reboot:
numactl --hardware   # must show: available: 2 nodes (0-1)
```

#### 3b. Copy the modified kernel source files

```bash
# Copy optimised migrate.c and rmap.c into the kernel source tree:
cp ~/eval_scripts/migrate.c   ~/linux-6.1.4/mm/migrate.c
cp ~/eval_scripts/rmap.c      ~/linux-6.1.4/mm/rmap.c

# Verify O1+O3 changes are present:
grep -c "migrate_pages_deferred_ipi\|mig_pfn_cmp\|CONFIG_MIG_RMAP_TIMING" \
    ~/linux-6.1.4/mm/migrate.c
# Expected: ≥ 10 matches
```

> **Note:** If you only have `run_x86_full.sh` and the userspace files but not `migrate.c`/`rmap.c`,
> you can still run Benchmarks A–D and get baseline timing data. T5 will not work
> (requires rmap patch for per-CPU sub-timing), and O1/O3 won't be active.

#### 3c. Build and install the kernel (~25 min)

```bash
cd ~/linux-6.1.4
cp /boot/config-$(uname -r) .config
make olddefconfig
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS

make -j$(nproc) 2>&1 | grep -E "error:|mm/migrate|mm/rmap" | head -20
# Expected: only two lines:
#   CC  mm/migrate.o
#   CC  mm/rmap.o

sudo make modules_install
sudo make install
sudo reboot
```

#### 3d. Verify after reboot

```bash
uname -r                                   # shows 6.1.4
sudo ls /sys/kernel/debug/mig_timing       # must exist
head -1 /sys/kernel/debug/mig_timing | tr ',' '\n' | wc -l
# Must print 25 (confirms rmap patch active: ipi_wait_ns column present)
# If prints 21 → rmap.c patch not compiled in — see Troubleshooting
```

---

## Running the Evaluation

### Full automated run (recommended)

```bash
cd ~/eval_scripts
sudo bash run_x86_full.sh --results-dir ./results_x86
```

### Quick mode (Bench E config 0 only, ~15 min)

```bash
sudo bash run_x86_full.sh --results-dir ./results_x86
```

### Skip Bench E (3 min — Bench A/B/C + analysis only)

```bash
sudo bash run_x86_full.sh --skip-benche --results-dir ./results_x86
```

### Manual step-by-step

```bash
# 1. Build
gcc -O2 -Wall -o mig_bench mig_bench_x86.c -lnuma -lpthread
gcc -O2 -Wall -o mig_bench_e mig_bench_e_x86.c -lnuma -lpthread -lm

# 2. Disable NUMA balancing
echo 0 | sudo tee /proc/sys/kernel/numa_balancing

# 3. Run Bench A–C
sudo ./mig_bench

# 4. Run sub-component analysis
python3 analyze_unmap.py --t5 timing_4kb_2048.csv   # main result
python3 analyze_unmap.py --t2 timing_4kb_2048.csv   # cache thrash
python3 analyze_unmap.py --t3 timing_4kb_2048.csv   # outliers

# 5. Run Bench E (config 0, ~55 sec)
sudo ./mig_bench_e 0

# 6. Load-delta + loaded T5
python3 analyze_unmap.py --t4 \
    timing_4kb_2048.csv \
    bench_e_timing_x86_E_rand_rmw_t4_c512.csv

python3 analyze_unmap.py --t5 \
    bench_e_timing_x86_E_rand_rmw_t4_c512.csv
```

---

## Getting Started (Within 30 Minutes)

This section helps you verify the basic functionality of the artifact end-to-end within 30 minutes, starting from a machine with the instrumented kernel already booted.

**Human time: ~5 minutes | Compute time: ~15 minutes**

### Step 1 — Verify the instrumented kernel is running (2 min)

```bash
uname -r
# Expected: 6.1.4

sudo ls /sys/kernel/debug/mig_timing
# Must exist — if absent, mount debugfs:
# sudo mount -t debugfs none /sys/kernel/debug

head -1 /sys/kernel/debug/mig_timing | tr ',' '\n' | wc -l
# Must print 25 — confirms rmap patch compiled in (ipi_wait_ns column present)
# If prints 21 → rmap.c was not patched. See Troubleshooting.

grep -c migrate_pages_deferred_ipi /proc/kallsyms
# Must be ≥ 1 — confirms O1 batch function is in the running kernel

numactl --hardware
# Must show: available: 2 nodes (0-1), with CPUs on both nodes
```

### Step 2 — Verify all required files are in place (30 sec)

```bash
ls ~/eval_scripts/
# Must show ALL of:
# run_x86_full.sh   run_t1.sh
# mig_bench_x86.c   mig_bench_e_x86.c
# analyze_unmap.py  analyze_timing.py  analyze_bench_e_x86.py
# migrate.c  rmap.c
# migrate_x86.patch  rmap_x86.patch  README
```

If any file is missing, copy it from the artifact folder before continuing.

### Step 3 — "Hello world" quick run (~15 min)

This runs the complete pipeline in quick mode: Bench A-D quiescent → T5/T2/T3 analysis → Bench E config 0 only (55 seconds).

```bash
cd ~/eval_scripts
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
sudo bash run_x86_full.sh --quick --results-dir ./results_x86
```

Watch for these lines in the output — they confirm both optimisations are working:
```
[OK]  rmap patch active (25 columns detected)
[OK]  O1: batch function found
✓ O3 CONFIRMED: High locality in page-table walk.
✓ O1 CONFIRMED: Batching significantly reduces TLB shootdown impact.
```

### Step 4 — Check key claims (30 sec)

```bash
# Claim 1: rmap patch active (25 CSV columns)
head -1 results_x86/timing_4kb_2048.csv | tr ',' '\n' | wc -l
# Expected: 25

# Claim 2: O2a and O2b confirmed
grep "CONFIRMED" results_x86/run_x86.log
# Expected:
#   ✓ O1 CONFIRMED: Batching significantly reduces TLB shootdown impact.
#   ✓ O3 CONFIRMED: High locality in page-table walk.

# Claim 3: records collected
wc -l results_x86/timing_4kb_2048.csv
# Expected: 2049 (2048 data rows + 1 header)
```

### Step 5 — Supply your own inputs (robustness check)

This step demonstrates that the artifact accepts arbitrary inputs, not just the defaults.

```bash
# Run T5 on any timing CSV you provide:
python3 analyze_unmap.py --t5 /path/to/your_timing.csv

# Reset the kernel ring buffer and capture a fresh sample:
echo 1 | sudo tee /sys/kernel/debug/mig_timing
sudo ./mig_bench
python3 analyze_unmap.py --t5 timing_4kb_2048.csv

# Run T4 with your own quiescent and loaded CSVs:
python3 analyze_unmap.py --t4 \
    results_x86/timing_4kb_2048.csv \
    results_x86/bench_e_timing_x86_E_rand_rmw_t4_c512.csv

# Run a specific Bench E config by index (0-based, 0–6):
cd results_x86
sudo ./mig_bench_e 5   # config 5 = E_rand_rmw_t4_c1 (chunk=1, max overhead)
sudo ./mig_bench_e 6   # config 6 = E_rand_rmw_t4_c64

# Run all T-tests on any timing CSV:
python3 analyze_unmap.py --all --csv /path/to/any_timing.csv
```

---

## Features and Expected Outcomes

| Test | Script flag | Purpose | Runtime | Expected result |
|------|-------------|---------|---------|-----------------|
| Bench A (512 pages) | auto | Baseline 4KB quiescent | 30 s | 512 records, 25 columns |
| Bench A (2048 pages) | auto | Main quiescent dataset | 2 min | 2048 records, 25 columns |
| Bench B (THP) | auto | 2MB THP migration | 15 min | 32/128 THPs succeeded |
| Bench C (shared) | auto | rmap scaling sweep | 2 min | 7 CSVs deg001–deg064 |
| T2 cache thrash | `--t2` | Detect H4 | <1 min | Q5/Q1 < 1.10, H4 ruled out |
| T3 rwsem outliers | `--t3` | Detect H3 | <1 min | Outliers < 2%, scattered |
| T5 quiescent | `--t5` | Sub-component decomp | <1 min | ipi_wait_ns 0–450 ns, walk 250–550 ns |
| T1 CPU hotplug | auto | IPI-per-CPU scaling | 15 min | Inconclusive on KVM; linear on VirtualBox/bare metal |
| Bench E config 0 | `--quick` | Loaded disruption | 55 s | Disruption > 0.60 with O1 |
| T4 load delta | auto | Quiescent vs loaded | <1 min | Unmap delta inside try_migrate |
| T5 loaded | auto | H1 under load | <1 min | IPI % < 10% (amortised) |
| Worker metrics | auto | Disruption factor | <1 min | Disruption > 0.60, recovery ≈ 1.0 |

---

## Assumptions and Unsupported Features

**Assumptions:**
- `numa=fake=2` boot parameter is set (or real multi-socket NUMA available)
- debugfs mounted at `/sys/kernel/debug` (default on Ubuntu 22.04)
- NUMA auto-balancing disabled before each run
- Run as root (move_pages syscall + debugfs write access)
- Ring buffer holds 16,384 records — Bench E captures a statistical sample

**Unsupported / Known Limitations:**
- **T1 on KVM**: vCPU remapping noise larger than IPI signal. Use VirtualBox or bare metal.
- **Bench D freeze**: On some VMs the downtime test hangs. Use `mig_bench_x86_nod.c` which skips Bench D.
- **KVM IPI absorption**: KVM intercepts `flush_tlb_others()` — O1 disruption improvement invisible. VirtualBox or bare metal required to see the 4.4× disruption improvement.
- **THP pages**: O1 batch function skips THPs (routes to standard retry loop).
- **O1 migration rate**: Intentionally slower (~2,848 pages/s vs ~39,000 baseline) due to batch serialisation. This is the throughput-for-disruption tradeoff.
- **QEMU topology**: If all 4 vCPUs are on guest node 0 (node 1 has no CPUs), Bench E clamps to 1 worker and Phase 3 recovery metrics are not meaningful. Fix: split CPUs across both nodes in QEMU `-numa` command.

---

## Detailed Evaluation

### Experiment 1: Bench A — Quiescent 4KB Page Migration

| Field | Details |
|-------|---------|
| **Purpose** | Measure the per-page migration stage cost breakdown (Lock/Unmap/Copy/Remap/Unlock) with no concurrent load. Verify that O2b reduces the structural walk cost below the 683 ns baseline. Produces `timing_4kb_2048.csv` which is used as the quiescent baseline for all T2/T3/T4/T5 analysis. |
| **How to run** | `sudo bash run_x86_full.sh --skip-benche --results-dir ./results_x86` (Bench A runs as part of Step 3 automatically) |
| **Estimated runtime** | ~2 minutes |
| **Expected result** | `timing_4kb_2048.csv`: 2048 records, 25 columns. T5 structural walk (outside-ptl): 250–550 ns (vs 683 ns baseline = 1.24–2.73× improvement). T2: Q5/Q1 < 1.10 (H4 cache thrash ruled out). T3: outliers < 2%, scattered across > 40% of sequence range (H3 ruled out). |
| **How to access result** | `python3 analyze_unmap.py --t5 results_x86/timing_4kb_2048.csv` → console breakdown. `python3 analyze_timing.py results_x86/timing_4kb_2048.csv` → `results_x86/plots/timing_4kb_2048_pie_hist.png`, `timing_4kb_2048_boxplot.png`. Check `grep "O3 CONFIRMED" results_x86/run_x86.log`. |

---

### Experiment 2: Bench B — 2MB THP Migration

| Field | Details |
|-------|---------|
| **Purpose** | Measure 2MB THP migration cost. Confirm copy-dominated profile at 2MB scale. THP routes through the standard pipeline (O1 batch skips THPs). |
| **How to run** | Part of Step 3 — runs automatically after Bench A in the same `sudo ./mig_bench` invocation. |
| **Estimated runtime** | ~10 minutes (combined with Bench A/C/D in Step 3) |
| **Expected result** | `timing_2mb_32.csv`, `timing_2mb_128.csv`. Copy stage > 90% of total. Copy BW ~300–2800 MB/s depending on hardware. |
| **How to access result** | `python3 analyze_timing.py results_x86/timing_2mb_32.csv results_x86/timing_2mb_128.csv` → `results_x86/plots/timing_2mb_*_pie_hist.png`. |

---

### Experiment 3: Bench C — Shared Page rmap Scaling

| Field | Details |
|-------|---------|
| **Purpose** | Measure how unmap cost scales with the number of processes sharing each page (rmap walk + IPI per mapping). Confirms O(mapcount) growth. |
| **How to run** | Part of Step 3 — runs automatically after Bench B. |
| **Estimated runtime** | ~2 minutes (within Step 3 total) |
| **Expected result** | 7 CSVs `timing_shared_deg001.csv` through `timing_shared_deg064.csv`. Unmap cost rises sub-linearly with sharing degree. |
| **How to access result** | `python3 analyze_timing.py results_x86/timing_shared_deg*.csv` → `results_x86/plots/sharing_scaling.png`. |

---

### Experiment 4: Bench D — Migration Downtime

| Field | Details |
|-------|---------|
| **Purpose** | Measure application-visible stall when a thread accesses a page currently under migration (hits the migration swap entry, sleeps in `migration_entry_wait()`). Confirms that the migration-entry mechanism — not the copy engine — determines stall duration. |
| **How to run** | Part of Step 3. Requires `mig_bench_x86.c` (Bench D enabled). If it hangs, use `mig_bench_x86` with Bench D commented out and skip this experiment. |
| **Estimated runtime** | ~2 minutes (within Step 3 total) |
| **Expected result** | `downtime_samenode.csv`: 400,000 samples, 6–18 stall events above 5,000 ns threshold. Max stall 42–310 µs. Normal access mean 13–36 ns. |
| **How to access result** | `python3 analyze_downtime.py results_x86/downtime_samenode.csv` → `results_x86/plots/downtime_samenode_plot.png`. Max stall value printed to console. |

---

### Experiment 5: T5 — Sub-Component Decomposition

| Field | Details |
|-------|---------|
| **Purpose** | Decompose `try_to_migrate()` into four non-overlapping cost components using the 25-column rmap timing brackets: H1 `ipi_wait_ns` (IPI shootdown), H2 PTE body (`page_remove_rmap` atomics), H3 `rwsem_wait_ns` (rwsem acquisition), and structural walk (outside-ptl). Isolates the root cause and confirms H1 is dominant under concurrent load while H2/H3/H4 are ruled out. |
| **How to run** | `python3 analyze_unmap.py --t5 results_x86/timing_4kb_2048.csv` (runs automatically in Step 5 of `run_x86_full.sh`) |
| **Estimated runtime** | < 1 minute |
| **Expected result** | IPI % (median-based): 0–25% quiescent (0% on KVM, ~23% on VirtualBox). Structural walk: 250–550 ns (27–35% of total). CV of try_migrate: < 0.5 on clean runs. T2 Q5/Q1 < 1.10 (H4 not supported). T3 outliers scattered (H3 not supported). |
| **How to access result** | Console output and `results_x86/run_x86.log`. `grep "CONFIRMED\|H4 NOT\|H3" results_x86/run_x86.log`. |

---

### Experiment 6: T1 — IPI Isolation via CPU Hotplug

| Field | Details |
|-------|---------|
| **Purpose** | Isolate the per-CPU IPI cost by offlining CPUs one at a time and measuring how `unmap_ns` scales. If IPI dominates, unmap_ns should fall linearly as fewer remote CPUs need to be interrupted. |
| **How to run** | `sudo bash run_t1.sh results_x86/t1_results results_x86/mig_bench` (runs automatically in Step 5 of `run_x86_full.sh` if `run_t1.sh` is present) |
| **Estimated runtime** | ~15 minutes (3 CPU configs × 5 min each) |
| **Expected result** | `t1_results/t1_summary.csv`. On VirtualBox/bare metal: linear decrease in unmap_ns as CPU count falls (R² ≥ 0.85 confirms H1). On KVM: inconclusive (R² ~ 0.75) — hypervisor absorbs IPI, remapping noise dominates. |
| **How to access result** | `python3 analyze_unmap.py --t1 results_x86/t1_results/t1_summary.csv` → prints slope, R², and H1 verdict. Raw per-CPU CSVs in `results_x86/t1_results/`. |

---

### Experiment 7: Bench E — Concurrent Load Disruption (Primary O1 Claim)

| Field | Details |
|-------|---------|
| **Purpose** | Measure application worker throughput (ops/s) and p999 tail latency during active NUMA page migration. Validates that O1 (deferred batch IPI) reduces disruption to concurrent workers from the unoptimised baseline of 0.10–0.25 to > 0.60. Seven configurations sweep access pattern, thread count, and chunk size. |
| **How to run** | Full (all 7 configs, ~55 min): `sudo bash run_x86_full.sh --results-dir ./results_x86`. Quick (config 0 only, ~55 sec): `sudo bash run_x86_full.sh --quick --results-dir ./results_x86`. Single config: `cd results_x86 && sudo ./mig_bench_e 0` |
| **Estimated runtime** | 55 min (all 7 configs) / 55 sec (config 0 only) |
| **Expected result** | Primary config (E_rand_rmw_t4_c512): disruption factor ≥ 0.60, Phase 3 recovery ≥ 0.90, p999 during migration < 50,000 ns. On KVM with proper NUMA pinning: disruption > 1.0 (workers gain locality as pages migrate). Script auto-prints `✓ O1 CONFIRMED` on success. |
| **How to access result** | Step 8 of `run_x86_full.sh` prints disruption factor automatically. `grep "Disruption\|CONFIRMED" results_x86/run_x86.log`. Raw data: `results_x86/bench_e_workers_x86_E_rand_rmw_t4_c512.csv`. Plots (if `analyze_bench_e_x86.py` present): `results_x86/plots/bench_e_*.png`. |

---

## Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `[FAIL] mig_bench_x86.c not found` | Source files not in same folder as `run_x86_full.sh` | Copy `mig_bench_x86.c` and `mig_bench_e_x86.c` into the same directory |
| `25 columns shows 21` | rmap.c patch not compiled in | Add `#define CONFIG_MIG_RMAP_TIMING 1` to both `mm/rmap.c` and `mm/migrate.c`, rebuild, reboot |
| `T5: ERROR: CSV does not contain rmap patch columns` | Running old timing CSV generated before kernel rebuild | `sudo ./mig_bench` to regenerate CSV, then re-run T5 |
| `ipi_wait_ns all zero` | migrate.c missing the `#define CONFIG_MIG_RMAP_TIMING 1` | Add the define near the top of `mm/migrate.c` (after includes), rebuild |
| `cannot access mig_timing: Permission denied` | debugfs needs root | Always run with `sudo` |
| `Bench D freeze / hang` | Known VM issue with TSC-timed downtime test | Use `mig_bench_x86_nod.c` instead (Bench D disabled) |
| `numactl: No NUMA support available` | `numa=fake=2` not in boot params | Add to GRUB_CMDLINE_LINUX, `sudo update-grub`, reboot |
| `list_sort undeclared` | Missing include in migrate.c | Add `#include <linux/list_sort.h>` near other includes |

---

## Output Files Reference

| File | Generated by | Used by |
|------|-------------|---------|
| `timing_4kb_2048.csv` | `mig_bench` | T2, T3, T4, T5-quiescent |
| `bench_e_timing_x86_E_rand_rmw_t4_c512.csv` | `mig_bench_e` | T4, T5-loaded |
| `bench_e_workers_x86_E_rand_rmw_t4_c512.csv` | `mig_bench_e` | Worker disruption metrics |
| `t1_results/t1_summary.csv` | `run_t1.sh` | T1 IPI scaling analysis |
| `results_x86/run_x86.log` | `run_x86_full.sh` | Full run log |
