# CS614 Artifact — ARM64 Copy-Stage Optimization (ROCM)
**Platform:** ARM64 (AArch64) · Linux 6.1.4 · emulated NUMA (`numa=fake=2`) · 4KB base pages

---

## How to Start

```bash
# 1. Place all files from this folder in one directory, e.g. ~/eval_scripts/
# 2. Apply the kernel patches MANUALLY (see Section 3 — Kernel Compilation below)
#    There is no automation script for patching — this must be done by hand.
# 3. Rebuild and reboot into the patched kernel.
# 4. SSH into the machine after reboot and run:

cd ~/eval_scripts
sudo env PATH="$PATH" bash run_arm64_full.sh --results-dir ./results_arm64
```

### Required Files — All Must Be in the Same Folder

| File | What it is | Required? |
|------|-----------|-----------|
| `run_arm64_full.sh` | Main automation script | **YES** |
| `mig_bench.c` | Userspace Benchmarks A–D source | **YES** |
| `mig_bench_e.c` | Benchmark E source (concurrent load) | **YES** |
| `mig_bench_rocm.c` | ROCM vs standard single-threaded comparison | **YES** |
| `mig_bench_rocm_mt.c` | ROCM vs standard multithreaded comparison | **YES** |
| `analyze_timing.py` | Stage breakdown + plots | **YES** |
| `analyze_downtime.py` | Downtime analysis + plots | **YES** |
| `migrate.c` | Optimized ROCM kernel source — **copy manually** to `linux-6.1.4/mm/migrate.c` | **YES** |
| `migrate_arm.patch` | Patch diff for migrate.c (alternative to full file) | Optional |


```bash
# Verify all required files are present:
ls ~/eval_scripts/
# Must show: run_arm64_full.sh, mig_bench.c, mig_bench_e.c, mig_bench_rocm.c,
#            mig_bench_rocm_mt.c, analyze_timing.py, analyze_downtime.py, migrate_rocm.c
```

---

## Artifact Directory Structure

```
.
├── mig_bench.c              # Userspace benchmark driver: Benchmarks A, B, C, D
├── mig_bench_e.c            # Userspace benchmark driver: Benchmark E (concurrent load)
├── mig_bench_rocm.c         # ROCM vs standard single-threaded comparison benchmark
├── mig_bench_rocm_mt.c      # ROCM vs standard multithreaded controlled benchmark
├── analyze_timing.py        # Stage breakdown analysis + plots for Bench A / B / C CSVs
├── analyze_downtime.py      # Downtime analysis + plots for Bench D CSVs
├── run_arm64_full.sh        # ← Main automation script — runs ALL benchmarks end-to-end
└── results_arm64/           # Auto-created by run_arm64_full.sh; all outputs land here
    ├── timing_4kb_512.csv
    ├── timing_4kb_2048.csv
    ├── timing_2mb_32.csv
    ├── timing_2mb_128.csv
    ├── timing_shared_deg001.csv  …  timing_shared_deg064.csv   ← zero-padded filenames
    ├── downtime_samenode.csv
    ├── downtime_crossnode.csv
    ├── bench_e_workers_*.csv
    ├── bench_e_migrator_*.csv
    ├── bench_e_timing_*.csv
    ├── rocm_standard_timing.csv
    ├── rocm_fast_timing.csv
    ├── rocm_comparison.txt
    ├── run_arm64.log
    └── plots/
        ├── timing_4kb_512_pie_hist.png
        ├── timing_4kb_2048_pie_hist.png
        ├── timing_4kb_512_boxplot.png
        ├── timing_4kb_2048_boxplot.png
        ├── thp_projection.png
        ├── sharing_scaling.png
        ├── downtime_samenode_plot.png
        └── downtime_crossnode_plot.png

        
linux-6.1.4/mm/                ← kernel source modifications
└── migrate.c                   ROCM fast-path + timing
```

---

## Optimization Background: ROCM on ARM64

On ARM64, quiescent migration cost splits roughly equally between copy and unmap:

| Dataset    | Copy % | Unmap % |
|------------|:------:|:-------:|
| 512 pages  | 33.9%  | 42.0%   |
| 2048 pages | 41.1%  | 38.0%   |

The standard pipeline installs a migration swap entry (Present=0) during unmap, forcing
any faulting thread to sleep in `migration_entry_wait()` for the full copy+remap window
(~1,100 ns / 4KB page). ROCM eliminates this stall for read-only file-backed pages by
copying first, then atomically swapping PTEs — reducing the application stall to a single
TLBI VAE1IS round-trip (~420 ns).

| Pipeline | Stage order | App stall window |
|----------|-------------|------------------|
| Standard | Lock → Unmap → Copy → Remap → Unlock | ~1,100 ns |
| ROCM     | Lock → Copy → [atomic PTE swap] → Unlock | ~420 ns |

---

## Setup Instructions

### CPU
- **Minimum:** 4 cores total (2 per emulated NUMA node)
- **Recommended for kernel build:** ≥ 8 cores (build time ~8 min at 8 cores vs ~35 min at 2)
- The benchmark itself uses 2 migrator CPUs + 2 worker CPUs (node1 limit); extra cores help only build time

### Memory
- **Minimum: 4 GB RAM** (tested and verified at 4 GB)
  - 512 MB: benchmark buffer (allocated by `mig_bench`)
  - 1–2 GB: kernel build + page tables during compilation
  - Remainder: OS, Python analysis tools, headroom during Bench E

> **Note:** 1.5 GB is insufficient — the kernel build and Bench E's 512 MB buffer
> together cause swap pressure below 4 GB. Use exactly 4 GB or more.

### Storage
| Partition | Minimum size | Contents |
|-----------|:------------:|---------|
| `/`       | 8 GB         | OS, kernel build tree, benchmark source, Python packages |
| `/boot`   | 256 MB       | Kernel image + initrd (two kernel versions: baseline + ROCM) |
| `/tmp`    | 512 MB       | Kernel build temporaries (`make -j`) |

Total minimum: **~9 GB** across all partitions.

### Extra Hardware
None required. All benchmarks run entirely in software on a single physical machine
with emulated NUMA (`numa=fake=2`).

### Operating System
- **Tested on:** Ubuntu 22.04 LTS ARM64 (Jammy Jellyfish)
- **Kernel:** Linux 6.1.4 (custom build from source with timing instrumentation)
- Other Debian-based ARM64 distros should work; package names may differ

### Software Dependencies
```bash
sudo apt update
sudo apt install -y \
    build-essential \       # gcc, make, binutils
    libnuma-dev \           # NUMA library headers + libnuma.so
    numactl \               # numactl --hardware, numactl --membind
    python3 \               # Python 3.10+ (ships with Ubuntu 22.04)
    python3-numpy \         # numpy (stage timing statistics)
    python3-matplotlib      # matplotlib (plot generation)
```

Verify installations:
```bash
gcc --version              # should show gcc 11+
python3 -c "import numpy, matplotlib; print('OK')"
numactl --hardware         # should show 2 nodes after boot with numa=fake=2
```

### Boot Parameter (one-time setup)
```bash
# Edit GRUB config:
sudo nano /etc/default/grub
# Set this line:
GRUB_CMDLINE_LINUX="numa=fake=2"

sudo update-grub
sudo reboot

# Verify after reboot:
cat /proc/cmdline | grep numa=fake
numactl --hardware         # must show: available: 2 nodes (0-1)
```

### Linux Kernel Compilation — Baseline (Benchmarks A–E)
**Human time:** ~5 minutes | **Compute time:** ~8–35 minutes depending on core count

The baseline kernel uses the timing instrumentation already present in the source tree.
No additional patches are needed for Benchmarks A–E.

```bash
cd /path/to/linux-6.1.4

# Use your existing .config (or copy from running kernel):
cp /boot/config-$(uname -r) .config
make olddefconfig

# Build and install:
make -j$(nproc)
sudo make modules_install
sudo make install
sudo reboot

# Verify after reboot:
uname -r                             # should show 6.1.4
ls /sys/kernel/debug/mig_timing      # must exist (instrumentation active)
```

### Linux Kernel Compilation — ROCM Patch (Benchmark ROCM only)
**Human time:** ~5 minutes | **Compute time:** ~8–35 minutes

> **This step requires manually copying `migrate_rocm.c` from the artifact into the
> kernel source tree. There is no script that does this automatically.**

```bash
# Step 1 — Copy the optimized migrate.c from the artifact into the kernel source tree:
cp /path/to/artifact/migrate_rocm.c /path/to/linux-6.1.4/mm/migrate.c

# Verify the copy succeeded:
grep -c "rocm_swap_ptes\|folio_all_mappings_readonly" /path/to/linux-6.1.4/mm/migrate.c
# Expected: ≥ 5 matches — confirms ROCM code is present

# Step 2 — Rebuild (incremental — only migrate.o is recompiled):
cd /path/to/linux-6.1.4
make -j$(nproc)
sudo make modules_install
sudo make install
sudo reboot

# Step 3 — Verify ROCM patch is active after reboot:
head -1 /sys/kernel/debug/mig_timing | grep rocm_path
# Must print a line containing: rocm_path
# If rocm_path is absent: the copy in Step 1 did not take effect — check path and redo
```

### Debugfs and NUMA Balancing
```bash
# Debugfs (usually auto-mounted; check with):
ls /sys/kernel/debug/mig_timing
# If missing:
sudo mount -t debugfs none /sys/kernel/debug

# Disable kernel auto-migration (prevents noise in measurements):
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
```

---

## Features, Functionalities, and Outputs

### Summary Table

| Feature | Benchmark | Script step | Threads | Pages | Expected outcome |
|---------|-----------|-------------|---------|-------|-----------------|
| Quiescent 4KB migration timing | A | Step 3 | 1 migrator | 512, 2048 | Copy ~38–46%, Unmap ~38–42% |
| 2MB THP migration timing | B | Step 3 | 1 migrator | 32, 128 THPs | Copy ~95.8% (analytical projection on this VM) |
| Shared-page rmap walk scaling | C | Step 3 | 1 migrator | 64 × deg 1–64 | Unmap grows O(mapcount), α ≈ 0.53 µs/process |
| Application-visible downtime | D | Step 3 | 1 accessor + 1 migrator | 1 page | Same-node stall ~62–80 µs |
| Concurrent load disruption | E | Step 6 | 1–8 workers + 1 migrator | 131,072 | disruption_factor ≈ 1.0 for chunk≥64 |
| ROCM stall elimination | ROCM | Step 8 | 1–4 + 1 | 2048 | Stall ~1,100 ns → ~420 ns, remap_ns ≈ 0 |

**Automation:** All features are driven by `run_arm64_full.sh`. Individual benchmarks
can also be run in isolation — see the Detailed Evaluation section for exact commands.

### Benchmark A — Quiescent 4KB Page Migration
- **Purpose:** Establish per-stage cost ratios on ARM64; confirm copy-dominated profile.
- **Parameters:** 4KB (order-0), 512 & 2048 pages, 1 migrator thread.
- **Expected Result:** Copy ≈ 38–46%, Unmap ≈ 38–42%, mean total ≈ 1,500–2,200 ns/page.
- **Analysis Command:** `python3 analyze_timing.py results_arm64/timing_4kb_512.csv results_arm64/timing_4kb_2048.csv`
- **Outputs:** `timing_4kb_2048_pie_hist.png`, `timing_4kb_2048_boxplot.png`

### Benchmark B — 2MB THP Migration
- **Purpose:** Quantify copy dominance at 2MB scale; motivate DMA offload for large pages.
- **Parameters:** 2MB (order-9 THP), 32 & 128 pages.
- **Expected Result:** Copy ≈ 95.8% (analytical projection on this VM — see Known Issues).
- **Analysis Command:** `python3 analyze_timing.py results_arm64/timing_2mb_32.csv results_arm64/timing_2mb_128.csv`
- **Outputs:** `thp_projection.png`

### Benchmark C — Shared-Page rmap Walk Scaling
- **Purpose:** Measure rmap walk cost vs sharing degree; confirm O(mapcount) unmap scaling.
- **Parameters:** 64 pages shared across 1 to 64 processes.
- **Expected Result:** Unmap grows linearly: α ≈ 0.53 µs/process. At degree 64: unmap ≈ 39 µs.
- **Analysis Command:** `python3 analyze_timing.py results_arm64/timing_shared_deg*.csv`
- **Outputs:** `sharing_scaling.png`

### Benchmark D — Application-Visible Downtime
- **Purpose:** Measure faulting thread stall caused by standard migration PTE installation.
- **Parameters:** `CNTVCT_EL0` 24 MHz timer, 1 accessor + 1 migrator, same-node & cross-node.
- **Expected Result:** Same-node max stall ~62–80 µs; cross-node ~26–38 µs.
- **Analysis Command:** `python3 analyze_downtime.py results_arm64/downtime_samenode.csv results_arm64/downtime_crossnode.csv`
- **Outputs:** `downtime_samenode_plot.png`, `downtime_crossnode_plot.png`

### Benchmark E — Concurrent Load
- **Purpose:** Quantify migration disruption, p999 tail latency spike under workload pressure.
- **Parameters:** 131,072 pages, varying workers (1–8), chunks (1–512), access patterns (Read/RMW).
- **Expected Result:** `disruption_factor` ≈ 1.0 for chunk≥64; chunk=1 p999 spike ~22×.
- **Analysis Command:** `python3 analyze_timing.py results_arm64/bench_e_timing_E_rand_rmw_t4_c512.csv results_arm64/timing_4kb_2048.csv`
- **Outputs:** `bench_e_workers_*.csv`, `bench_e_migrator_*.csv`, `bench_e_timing_*.csv`

### ROCM — Stall Elimination
- **Purpose:** Validate ROCM eliminates migration-entry stall for read-only file-backed pages.
- **Parameters:** 2048 pages, 1+1 and 4+1 threading models.
- **Requires:** `migrate_rocm.c` copied into kernel tree and kernel rebuilt (see Kernel Compilation).
- **Expected Result:** `rocm_path=1` in all fast-path records; `remap_ns` ≈ 0; stall ~1,100 ns → ~420 ns.
- **Verify Command:** `grep ",1$" results_arm64/rocm_fast_timing.csv | wc -l` (must be > 0)
- **Outputs:** `rocm_standard_timing.csv`, `rocm_fast_timing.csv`, `rocm_comparison.txt`

### Crash, Deadlock, and Assertion Failures

| Finding | Type | Frequency | Trigger | Impact |
|---------|------|-----------|---------|--------|
| `move_pages()` partial failures | Non-fatal error | Occasional (~5–15%) | numa=fake=2 policy not honoring `mbind()` strictly | Pages reported on wrong node; migration still proceeds |
| THP split before migration | Non-fatal warning | Every run on this VM | `numa=fake=2` kernel splits THPs before `move_pages()` | Bench B has 0 real THP records; analytical projection used |
| `WARN_ONCE: rocm: unexpected PMD abort` | Kernel warning | Rare (<1%) | PMD-mapped THP during ROCM rmap walk | ROCM falls back to standard path for that page; data valid |
| `folio_migrate_mapping` returns -EAGAIN | Transient retry | Occasional | Concurrent page-cache lookup during ROCM PTE swap | Retried up to 10×; not counted as failure |
| Ring buffer overflow during Bench E Phase 2 | Expected truncation | Every Bench E run | 131,072 pages > 16,384 ring buffer slots | Wraps ~8×; data is a valid statistical sample |
| Worker thread count clamped | Behavioural note | Every Bench E run | Node 1 has only 2 physical vCPUs | t4, t8 configs run with 2 actual workers; t1→t8 sweep is uninterpretable |

---

## Assumptions and Unsupported Features

### Assumptions
- **NUMA is emulated:** `numa=fake=2` creates two logical nodes from one physical DIMM.
  True inter-socket memory latency difference is absent. All throughput and benefit
  results are indicative rather than absolute.
- **Explicit migration only:** `move_pages()` syscall is used directly. Kernel NUMA
  balancing (PROT_NONE fault path) is not exercised.
- **ROCM eligibility is conservative:** ROCM only fires for file-backed, clean,
  non-anonymous pages with all-read-only PTEs. Anonymous pages always use the standard
  pipeline even with the ROCM kernel.

### Unsupported Features
- **Benchmark B (real THP data):** Kernel splits all THPs under `numa=fake=2`. Analytical
  projection is substituted automatically by the analysis script.
- **Thread scaling beyond t2:** Node 1 has ≤2 vCPUs. t4 and t8 configs are clamped to 2
  workers. The t1→t8 scaling sweep is not meaningful beyond t2.
- **Cross-node downtime (Bench D):** May be absent on VMs with no CPU on node 1.
- **Sub-tick stall resolution (Bench D):** ARM64 24 MHz timer (41.67 ns/tick). Stalls
  shorter than ~42 ns round to zero — visible as median = 0.0 ns in crossnode CSV.
- **T1–T5 unmap decomposition tests:** x86-specific tests; not included in this artifact.

---

## Getting Started (Quick Verification, ~25 minutes)

**Human time: ~5 minutes | Compute time: ~20 minutes**

### Step 1: Verify environment (2 minutes)
```bash
numactl --hardware
# Expected: available: 2 nodes (0-1)

ls /sys/kernel/debug/mig_timing
head -1 /sys/kernel/debug/mig_timing
# Expected first line contains: seq,cpu,pid,src_pfn,...

uname -r
# Expected: 6.1.4
```

### Step 2: Build benchmarks (2 minutes)
```bash
cd ~/eval_scripts

gcc -O2 -Wall -o mig_bench   mig_bench.c   -lnuma -lpthread
gcc -O2 -Wall -o mig_bench_e mig_bench_e.c -lnuma -lpthread -lm

# Only if ROCM kernel is already built and booted:
gcc -O2 -Wall -o mig_bench_rocm    mig_bench_rocm.c    -lnuma -lm
gcc -O2 -Wall -o mig_bench_rocm_mt mig_bench_rocm_mt.c -lnuma -lm
```

### Step 3: Run a single quick migration (3 minutes)
```bash
sudo ./mig_bench
# Expected: Succeeded: 2048/2048   Failed: 0
#           Timing data → timing_4kb_2048.csv
```

### Step 4: Verify stage breakdown (1 minute)
```bash
python3 analyze_timing.py timing_4kb_2048.csv
# Expected: Copy ≈ 38–46%, Unmap ≈ 38–42%
# If Copy < 20%: instrumented kernel not running — check uname -r
```

### Step 5: Run quick Bench E (10 minutes)
```bash
sudo ./mig_bench_e 0
# Config 0 = E_rand_rmw_t4_c512
# Expected: disruption_factor ≈ 1.0, p999 spike < 2×
```

### Step 6: Full script quick mode (~10 minutes)
```bash
sudo bash run_arm64_full.sh --quick
# Runs: A+B+C+D → analyze → Bench E config 0 → ROCM (if kernel has patch)
```

### Supplying Your Own Inputs

```bash
# Run a specific Bench E config by 0-based index:
sudo ./mig_bench_e 5   # index 5 = E_rand_rmw_t4_c1 (chunk=1, maximum overhead)
sudo ./mig_bench_e 6   # index 6 = E_rand_rmw_t4_c64

# Analyse any timing CSV:
python3 analyze_timing.py timing_4kb_512.csv timing_shared_deg032.csv

# Analyse any downtime CSV:
python3 analyze_downtime.py downtime_samenode.csv

# Reset ring buffer and capture a fresh sample manually:
echo 1 | sudo tee /sys/kernel/debug/mig_timing
sudo ./mig_bench
cat /sys/kernel/debug/mig_timing > my_custom_timing.csv
python3 analyze_timing.py my_custom_timing.csv
```

---

## Detailed Evaluation

### Experiment 1: Bench A — Quiescent Stage Breakdown

| Field | Value |
|-------|-------|
| **Purpose** | Establish per-stage cost ratios; confirm copy-dominated profile; produce quiescent baseline for all subsequent comparisons |
| **How to run** | `sudo ./mig_bench` (A+B+C+D run automatically) or `run_arm64_full.sh` Step 3 |
| **Human time** | 2 minutes |
| **Compute time** | ~10 minutes |
| **Expected result** | Copy ≈ 38–46%, Unmap ≈ 38–42%, mean total ≈ 1,500–2,200 ns/page |
| **Output data** | `results_arm64/timing_4kb_512.csv`, `results_arm64/timing_4kb_2048.csv` |
| **Plots** | `results_arm64/plots/timing_4kb_2048_pie_hist.png`, `timing_4kb_2048_boxplot.png` |
| **Analysis command** | `python3 analyze_timing.py results_arm64/timing_4kb_512.csv results_arm64/timing_4kb_2048.csv` |

### Experiment 2: Bench B — 2MB THP Migration

| Field | Value |
|-------|-------|
| **Purpose** | Quantify copy dominance at 2MB scale; motivate DMA offload for large pages |
| **How to run** | Runs automatically after Bench A |
| **Human time** | 0 minutes (fully automated) |
| **Compute time** | ~3 minutes (included in Bench A total) |
| **Expected result** | Copy ≈ 95.8% on real hardware. On this VM: analytical projection — copy = 499.4 µs (99.7%) |
| **Output data** | `results_arm64/timing_2mb_32.csv`, `results_arm64/timing_2mb_128.csv` |
| **Plots** | `results_arm64/plots/thp_projection.png` |
| **Analysis command** | `python3 analyze_timing.py results_arm64/timing_2mb_32.csv results_arm64/timing_2mb_128.csv` |

### Experiment 3: Bench C — Sharing Degree Sweep

| Field | Value |
|-------|-------|
| **Purpose** | Measure rmap walk cost vs sharing degree; confirm O(mapcount) unmap scaling |
| **How to run** | Runs automatically after Bench B |
| **Human time** | 0 minutes (fully automated) |
| **Compute time** | ~5 minutes (included in Bench A total) |
| **Expected result** | Unmap grows linearly: α ≈ 0.53 µs/process. At degree 64: unmap ≈ 39 µs |
| **Output data** | `results_arm64/timing_shared_deg001.csv` through `timing_shared_deg064.csv` |
| **Plots** | `results_arm64/plots/sharing_scaling.png` |
| **Analysis command** | `python3 analyze_timing.py results_arm64/timing_shared_deg*.csv` |

### Experiment 4: Bench D — Application-Visible Downtime

| Field | Value |
|-------|-------|
| **Purpose** | Measure faulting thread stall caused by migration PTE; motivate ROCM |
| **How to run** | Runs automatically after Bench C |
| **Human time** | 0 minutes (fully automated) |
| **Compute time** | ~5 minutes (included in Bench A total) |
| **Expected result** | Same-node max stall ~62–80 µs; cross-node ~26–38 µs |
| **Output data** | `results_arm64/downtime_samenode.csv`, `results_arm64/downtime_crossnode.csv` |
| **Plots** | `results_arm64/plots/downtime_samenode_plot.png`, `downtime_crossnode_plot.png` |
| **Analysis command** | `python3 analyze_downtime.py results_arm64/downtime_samenode.csv results_arm64/downtime_crossnode.csv` |

### Experiment 5: Bench E — Concurrent Load (All 7 Configurations)

| Field | Value |
|-------|-------|
| **Purpose** | Quantify migration disruption, p999 spike, and break-even time under concurrent workload |
| **How to run** | `sudo ./mig_bench_e` (all 7 configs) or `run_arm64_full.sh` Step 6 |
| **Human time** | 5 minutes |
| **Compute time** | ~50 minutes (7 configs × ~7 minutes each) |
| **Expected result** | disruption_factor ≈ 1.0 for chunk≥64; chunk=1 p999 spike ~22×; seq_read throughput may rise during migration |
| **Output data** | `results_arm64/bench_e_workers_*.csv`, `bench_e_migrator_*.csv`, `bench_e_timing_*.csv` |
| **Plots** | Quiescent vs under-load stage comparison printed to terminal and log (Step 7) |
| **Analysis command** | `python3 analyze_timing.py results_arm64/bench_e_timing_E_rand_rmw_t4_c512.csv results_arm64/timing_4kb_2048.csv` |

### Experiment 6: ROCM — Stall Elimination

| Field | Value |
|-------|-------|
| **Purpose** | Validate ROCM eliminates migration-entry stall for read-only file-backed pages |
| **How to run** | `sudo ./mig_bench_rocm && sudo ./mig_bench_rocm_mt` or `run_arm64_full.sh` Step 8 |
| **Requires** | `migrate_rocm.c` copied into kernel tree manually + kernel rebuilt and booted |
| **Human time** | 3 minutes |
| **Compute time** | ~10 minutes total |
| **Expected result** | `rocm_path=1` in all fast-path records; `remap_ns` ≈ 0; stall ~1,100 ns → ~420 ns |
| **Output data** | `results_arm64/rocm_standard_timing.csv`, `results_arm64/rocm_fast_timing.csv` |
| **Summary** | `results_arm64/rocm_comparison.txt` |
| **Verify fast path** | `grep ",1$" results_arm64/rocm_fast_timing.csv \| wc -l` — must be > 0 |

---

## Known Issues

| Issue | Type | Frequency | Notes |
|-------|------|-----------|-------|
| `tee: run_arm64.log: No such file or directory` | Bug (fixed) | Was in earlier script version | Caused by relative LOG path inside `pushd` — now uses absolute path |
| Bench C CSV not found (`deg1`, `deg16` etc.) | Bug (fixed) | Was in earlier script version | Actual filenames are zero-padded (`deg001`, `deg016`) — glob now matches correctly |
| THP filter: 0 order-9 records | Expected VM limitation | Every run | Known `numa=fake=2` behaviour — analytical projection substituted automatically |
| `move_pages()` partial failures (5–15%) | Non-fatal | Occasional | Normal on emulated NUMA; does not affect stage timing statistics |
| Worker thread count clamped to 2 | Behavioural | Every Bench E run | Node 1 has ≤2 vCPUs; t4 and t8 configs both run as 2 workers |
| Ring buffer overflow (wraps ~8×) | Expected | Every Bench E run | 131,072 pages / 16,384 slots; data is a valid statistical sample |
| Cross-node downtime CSV absent | Expected on some VMs | Depends on hypervisor | Requires at least 1 CPU on node 1; script handles absence gracefully |
| Stall median = 0 ns in crossnode CSV | Measurement artifact | Every run | ARM64 24 MHz timer (41.67 ns/tick): accesses faster than one tick round to zero |
| ROCM not firing (`rocm_path=0` in all records) | Config error | If kernel not rebuilt | Confirm `migrate_rocm.c` was copied to `mm/migrate.c` and kernel was rebuilt and rebooted |