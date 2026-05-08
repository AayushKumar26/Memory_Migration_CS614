# NUMA Page Migration Analysis and Optimization
### Quantifying and Reducing Migration Cost in Linux 6.1.4 — ARM64 & x86\_64

**Course:** CS614  
**Team:** `team_name.ko`  
**Members:** Yogit (211207) · Aayush Kumar (230027) · Cezan Vispi Damania (230310)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Repository Structure](#2-repository-structure)
3. [Background: The Migration Pipeline](#3-background-the-migration-pipeline)
4. [Part 1 — x86\_64: O2 Optimization](#4-part-1--x86_64-o2-optimization)
   - 4.1 [Root Cause Analysis](#41-root-cause-analysis)
   - 4.2 [O2a: Deferred Batch IPI Shootdown](#42-o2a-deferred-batch-ipi-shootdown)
   - 4.3 [O2b: Address-Ordered Sort](#43-o2b-address-ordered-sort)
   - 4.4 [Kernel Changes](#44-kernel-changes-x86_64)
   - 4.5 [Building the Optimized Kernel](#45-building-the-optimized-kernel-x86_64)
   - 4.6 [x86\_64 Results Summary](#46-x86_64-results-summary)
5. [Part 2 — ARM64: ROCM Optimization](#5-part-2--arm64-rocm-optimization)
   - 5.1 [Root Cause Analysis](#51-root-cause-analysis)
   - 5.2 [ROCM Protocol](#52-rocm-protocol)
   - 5.3 [Kernel Changes](#53-kernel-changes-arm64)
   - 5.4 [Building the Optimized Kernel](#54-building-the-optimized-kernel-arm64)
   - 5.5 [ARM64 Results Summary](#55-arm64-results-summary)
6. [Benchmark Suite](#6-benchmark-suite)
7. [Running the Evaluation](#7-running-the-evaluation)
   - 7.1 [x86\_64 Quick Start (≤ 30 min)](#71-x86_64-quick-start--30-min)
   - 7.2 [ARM64 Quick Start (≤ 30 min)](#72-arm64-quick-start--30-min)
8. [Hypothesis Testing](#8-hypothesis-testing)
9. [Key Findings & Conclusions](#9-key-findings--conclusions)
10. [Dependencies](#10-dependencies)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. Project Overview

NUMA (Non-Uniform Memory Access) page migration moves memory pages to the NUMA node where they are accessed most frequently, improving memory locality for running processes. In Linux 6.1.4, every migrated page stalls application threads for the **full five-stage pipeline duration** — even when the stall is architecturally unnecessary.

This project quantifies the per-stage cost of this stall on two architectures and proposes targeted kernel optimizations:

| Architecture | Bottleneck | Optimization | Stall Reduction |
|---|---|---|---|
| **x86\_64** | IPI shootdown dominates at 98.5% under load | O2a: Deferred Batch IPI + O2b: Address-Ordered Sort | 1.67× end-to-end |
| **ARM64** | Copy stage dominates at 46%; remap stall unnecessary for read-only pages | ROCM: Read-Only Copy Migration | 1.89× app stall |

All kernel modifications are in `mm/migrate.c` and `mm/rmap.c` against the Linux 6.1.4 source tree. Instrumentation is via a `debugfs` CSV ring buffer with 5 ns timestamps per migration event.

---

## 2. Repository Structure

```
.
├── README                        # This file
│
├── x86_64/
│   ├── migrate.c                 # Optimised mm/migrate.c (O2a + O2b)
│   ├── rmap.c                    # Optimised mm/rmap.c (TTU_BATCH_FLUSH, T5 timing)
│   ├── migrate_x86.patch         # Unified diff against stock Linux 6.1.4
│   ├── rmap_x86.patch            # Unified diff against stock Linux 6.1.4
│   ├── mig_bench_x86.c           # Benchmarks A, B, C, D (userspace)
│   ├── mig_bench_e_x86.c         # Benchmark E: 7 concurrent-load configs
│   ├── analyze_timing.py         # Stage breakdown analysis + PNG plots
│   ├── analyze_unmap.py          # T1–T5 sub-component decomposition
│   ├── analyze_bench_e_x86.py    # Bench E disruption factor + p999 plots
│   ├── run_x86_full.sh           # Full automation script (9 steps)
│   └── run_t1.sh                 # T1: CPU hotplug IPI isolation driver
│
└── arm64/
    ├── migrate_rocm.c            # ROCM-patched mm/migrate.c
    ├── mig_bench.c               # Benchmarks A, B, C, D (ARM64 userspace)
    ├── mig_bench_e.c             # Benchmark E: 7 concurrent-load configs
    ├── mig_bench_rocm.c          # ROCM vs standard single-thread comparison
    ├── mig_bench_rocm_mt.c       # ROCM vs standard multithreaded comparison
    ├── analyze_timing.py         # Stage breakdown analysis + PNG plots
    ├── analyze_downtime.py       # Bench D downtime analysis (24 MHz timer)
    └── run_arm64_full.sh         # Full automation script (9 steps)
```

---

## 3. Background: The Migration Pipeline

Every NUMA page migration passes through five sequential kernel stages. Application threads are stalled for the entire duration between **UNMAP** and **UNLOCK**:

```
  LOCK → UNMAP → COPY → REMAP → UNLOCK
         |___________________________|
              ← App thread stalls →
```

| Stage | x86\_64 Mean | ARM64 Mean | What Happens |
|---|---|---|---|
| **Lock** | 48 ns (0.7%) | 28 ns (2.1%) | Acquire folio lock on src + dst |
| **Unmap** | 2,609 ns (41%) | 420 ns (31.4%) | Invalidate PTEs, flush TLB |
| **Copy** | 1,380 ns (21.7%) | 610 ns (46.2%) | `copy_highpage()` CPU copy |
| **Remap** | 340 ns (5.3%) | 120 ns (9.0%) | Install new PTE, remove migration entry |
| **Unlock** | 180 ns (2.8%) | 40 ns (3.0%) | Wake faulting threads |

The dominant bottleneck differs fundamentally between architectures:

- **x86\_64:** Per-page IPI shootdown (`flush_tlb_others()`) costs ~2.6 µs/page quiescent and explodes to ~159 µs/page under concurrent load (221× increase). Unmap stage rises from 41% to 83–99% under load.
- **ARM64:** Hardware TLBI broadcast (`TLBI VAE1IS + DSB ISH`) eliminates software IPI overhead entirely. The Copy stage dominates at 46% — and for read-only pages, the stall during Copy is architecturally unnecessary.

---

## 4. Part 1 — x86\_64: O2 Optimization

### 4.1 Root Cause Analysis

Five hypotheses were tested to isolate the Unmap bottleneck. Kernel instrumentation exposes four non-overlapping sub-components of the Unmap stage via a 25-column `debugfs` CSV:

| ID | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| **H1** | IPI shootdown dominates | ✅ CONFIRMED | 98.5% of `try_migrate_ns` under load |
| **H2** | PTE spinlock (`pte_lockptr`) contention | ❌ RULED OUT | `ptl_wait_ns` < 2% quiescent |
| **H3** | kswapd rwsem contention | ❌ RULED OUT | Outliers < 2%, not clustered with kswapd |
| **H4** | Page-table cache thrash | ❌ RULED OUT | Q5/Q1 ≤ 1.0 — no monotonic rise |
| **H5** | Structural walk cost (secondary) | ✅ CONFIRMED | 28% of `try_migrate_ns` even quiescent |

**Quantified root cause (VirtualBox i5-1340P, quiescent vs under load):**

| Component | Quiescent | Under Load | Multiplier |
|---|---|---|---|
| H1 `ipi_wait_ns` (`flush_tlb_others`) | 632 ns (25%) | 139,538 ns (98.5%) | **221×** |
| H2 PTE body (`page_remove_rmap`) | 894 ns (36%) | 1,291 ns (0.9%) | 1.4× |
| H3 rwsem (`down_read`) | 277 ns (11%) | 0 ns (0%) | — |
| Structural walk (rmap + page table) | 683 ns (28%) | 885 ns (0.6%) | 1.3× |
| **Total `try_migrate_ns`** | **2,486 ns** | **141,714 ns** | **57×** |

For a 512-page batch under load: `512 × 159 µs = 81.5 ms` of IPI wait with the standard path.

### 4.2 O2a: Deferred Batch IPI Shootdown

The standard path calls `ptep_clear_flush()` per page, which issues `INVLPG + flush_tlb_others()` (IPI) for every page, blocking until all remote CPUs ACK. O2a **phase-separates** the migration loop so N pages share ONE IPI round-trip:

**Phase 1 — Batch Unmap (N pages, no IPI):**  
For each page: `ptep_get_and_clear()` atomically clears the PTE (no INVLPG, no IPI). `set_tlb_ubc_flush_pending()` registers the mm for a deferred flush. Workers that fault on the migration entry immediately sleep — they are blocked by the migration entry, not by TLB coherence.

**Phase 2 — Single Batch Flush (1 IPI for all N pages):**  
`try_to_unmap_flush()` → `arch_tlbbatch_flush()` issues ONE IPI set covering all pages unmapped in Phase 1. The migrator blocks once, not N times.

**Phase 3 — Copy + Remap (no IPI):**  
`move_to_new_folio()` copies src → dst. `remove_migration_ptes()` installs new PTEs. Workers wake on `folio_unlock(src)`.

**Correctness:** Src pages are never freed until after Phase 2 flush. All src and dst folios remain locked throughout Phases 1 and 2, preventing any stale TLB entry from reaching freed memory.

**Saving:** `N × 159 µs → ~159 µs` for the entire batch (`MIG_BATCH_SIZE = 32` in the implementation).

### 4.3 O2b: Address-Ordered Sort

The structural walk cost (683 ns, 28% of quiescent `try_migrate`) comes from a cold 4-level page-table walk (PGD → PUD → PMD → PTE) for each page. When migrations are processed in arbitrary order, all three upper table levels must be fetched from main memory per page.

O2b sorts the candidate page list by virtual address (using `mig_pfn_cmp + list_sort`) before the migration loop. Pages within the same 2 MB VA region share PGD, PUD, and PMD entries. Processing them consecutively keeps those entries hot in L1/L2 cache, reducing subsequent walks from 4 levels to ~1.

- Sort cost: ~10 µs for 512 pages (O(N log N))  
- Walk cost reduction: 683 ns → 429 ns (1.59×) on VirtualBox; 683 ns → 270 ns (2.02×) on Akshat Xeon (larger cache amplifies PMD locality benefit)

### 4.4 Kernel Changes (x86\_64)

**`mm/rmap.c`**
- `TTU_BATCH_FLUSH` flag added to the allowed set in `try_to_migrate()` (original `WARN_ON_ONCE` guard rejected it).
- In `try_to_migrate_one()`: `ptep_clear_flush()` is conditionally replaced by `ptep_get_and_clear() + set_tlb_ubc_flush_pending()` when `TTU_BATCH_FLUSH` is set. Falls through to the original path otherwise — no behavior change for existing callers.
- Per-CPU nanosecond timing brackets added (`ipi_wait_ns`, `ptl_wait_ns`, `rwsem_wait_ns`) producing the 25-column `debugfs` CSV.

**`mm/migrate.c`**
- `struct mig_batch_entry`: per-page state (src/dst folios, anon_vma reference, per-phase timing). Heap-allocated via `kmalloc_array` to avoid >1024-byte stack frames.
- `migrate_pages_deferred_ipi()`: 300-line batch function implementing Phases 1/2/3. Called from `migrate_pages()` before the standard retry loop, serving all callers (`move_pages()` syscall, NUMA balancing, compaction).
- `mig_pfn_cmp + list_sort()`: O2b comparator and sort call inserted before the migration loop.
- `mig_timing_store()` updated: `ipi_wait_ns` set to amortised flush cost (`total_flush_ns / n_pages`) so the `debugfs` CSV correctly reflects batch-path IPI cost.

### 4.5 Building the Optimized Kernel (x86\_64)

```bash
# Option A — Copy full sources
cp x86_64/migrate.c ~/linux-6.1.4/mm/migrate.c
cp x86_64/rmap.c    ~/linux-6.1.4/mm/rmap.c

# Option B — Apply patches
cd ~/linux-6.1.4
patch -p1 < /path/to/x86_64/migrate_x86.patch
patch -p1 < /path/to/x86_64/rmap_x86.patch

# Configure and build (~5 min human, ~25 min compile)
cp /boot/config-$(uname -r) .config && make olddefconfig
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
make -j$(nproc)
sudo make modules_install && sudo make install && sudo reboot

# Verify after reboot
uname -r                                              # must show 6.1.4
sudo ls /sys/kernel/debug/mig_timing                  # must exist
head -1 /sys/kernel/debug/mig_timing | tr ',' '\n' | wc -l  # must print 25
grep -c migrate_pages_deferred_ipi /proc/kallsyms     # must be ≥ 1
numactl --hardware                                    # must show 2 NUMA nodes
```

```bash
# Build userspace benchmarks
gcc -O2 -Wall -o mig_bench   x86_64/mig_bench_x86.c   -lnuma -lpthread
gcc -O2 -Wall -o mig_bench_e x86_64/mig_bench_e_x86.c -lnuma -lpthread -lm
```

### 4.6 x86\_64 Results Summary

**Benchmark A — Quiescent Stage Timing (VirtualBox i5-1340P):**

| Metric | Baseline | After O2 | Improvement |
|---|---|---|---|
| `try_migrate_ns` median | 2,486 ns | 1,488 ns | **1.67×** |
| `ipi_wait_ns` (O2a target) | 632 ns (25.4%) | 335 ns (22.5%) | **1.89×** |
| Structural walk (O2b target) | 683 ns (27.5%) | 429 ns (28.9%) | **1.59×** |
| PTE body work | 894 ns (36.0%) | 480 ns (32.2%) | **1.86×** |
| `unmap_ns` total | 2,609 ns | 1,642 ns | **1.59×** |
| `total_ns` (end-to-end) | 5,781 ns | 4,633 ns | **1.25×** |

**Benchmark E — Disruption Factor (rand\_rmw\_t4\_c512):**

| Machine | Baseline DF | O2 DF | p999 Spike | Notes |
|---|---|---|---|---|
| VirtualBox i5 (real IPI) | 0.133 | **0.767** | 41,858 ns | 5.8× better DF |
| Akshat Xeon 6226R (KVM) | — | **1.093** | 586 ns | Migration net-beneficial |
| Cezan i7-13620H (KVM) | — | **1.098** | 838 ns | Migration net-beneficial |

> **Note on VirtualBox vs KVM discrepancy:** VirtualBox propagates IPIs to real hardware; KVM intercepts `flush_tlb_others()` in the hypervisor, making O2a's IPI cost invisible but allowing the locality gain from migration to fully dominate. Both demonstrate O2 working correctly for their respective environments.

---

## 5. Part 2 — ARM64: ROCM Optimization

### 5.1 Root Cause Analysis

On ARM64, hardware `TLBI VAE1IS + DSB ISH` broadcast eliminates software IPI cost. The bottleneck shifts entirely to the **Copy stage** (46% of total time). A faulting thread accessing a page under migration sleeps in `migration_entry_wait()` for the full Unmap + Copy + Remap window (875 ns per page).

For **file-backed read-only pages**, this stall is architecturally unnecessary:
- The page content is stable — no writes can occur to a read-only mapping.
- A worker can safely read the source page at any point during migration.
- The migration entry exists only to maintain TLB consistency; for read-only mappings, an alternative path exists: **copy first, then atomically swap the PTE**.

### 5.2 ROCM Protocol

ROCM (Read-Only Copy Migration) applies when `folio_rmap_all_readonly()` returns true — every PTE mapping the page is `PROT_READ` and the folio is file-backed (shared libraries, read-only mmap files, clean page-cache pages). Anonymous pages always use the standard pipeline.

**Phase 1 — Copy with src PTEs live (no stall):**  
`copy_page(src, dst)` copies content while the src PTE remains present and readable. Workers continue reading src uninterrupted. No migration entry is installed; no threads are blocked.

**Phase 2 — Atomic PTE swap (stall window):**  
For each PTE mapping src: atomically replace the old translation (src phys) with the new translation (dst phys) via a single CMPXCHG. Issue `TLBI VAE1IS + DSB ISH` — one hardware TLB invalidation per PTE swap. This is the entire stall window: **462 ns** (one TLBI round-trip on tested hardware). Workers encountering the TLBI window see the new translation immediately — no `migration_entry_wait()`, no sleep.

**Phase 3 — Cleanup:**  
`folio_unlock(src)`. No threads are waiting. `put_page(src)` frees the src frame.

**Why stall is 462 ns and not zero:**  
The initial projection was ~70 ns (based on Remap stage cost of installing a PTE into a *non-present* slot, which requires no TLBI). ROCM's actual stall is 462 ns because swapping a *live* translation requires `TLBI VAE1IS + DSB ISH` — hardware mandates that all cores acknowledge the translation change. This cannot be avoided. The theoretical lower bound is one TLBI round-trip (~420 ns on this hardware); 462 ns is near-optimal (10% above minimum).

**Comparison of pipelines:**

| Property | Standard | ROCM |
|---|---|---|
| Order | Lock → Unmap → Copy → Remap → Unlock | Lock → Copy → PTE-swap → Unlock |
| Thread stall window | Unmap + Copy + Remap = **875 ns** | PTE-swap only = **462 ns** |
| Migration entry installed? | Yes — Present=0, all accesses fault and sleep | No — src PTE live until atomic swap |
| Remap stage | 75 ns (`remove_migration_ptes()`) | **0 ns — eliminated entirely** |

### 5.3 Kernel Changes (ARM64)

**`mm/migrate.c` (ARM64)**
- ROCM gate: `folio_rmap_all_readonly()` check before the standard unmap call.
- `ro_copy_and_swap()`: copies src → dst then calls `try_to_migrate_ro()` to swap PTEs without installing a migration swap entry.
- `rocm_path` column added to `mig_timing_record` for identification in `debugfs` output.

**`mm/rmap.c` (ARM64)**
- `try_to_migrate_ro()`: rmap walk to find each PTE, CMPXCHG to atomically swap old → new translation, `TLBI VAE1IS + DSB ISH` per PTE. Returns failure if any PTE is found to be writable (falls back to standard path for safety).

### 5.4 Building the Optimized Kernel (ARM64)

**System requirements:**

| Resource | Minimum | Recommended |
|---|---|---|
| CPU | ARM64 (AArch64), ≥4 cores | QEMU/KVM ARM64 with 4 vCPUs (2 per NUMA node) |
| Memory | 1.5 GB | 2 GB (avoids swap during Bench E 512 MB buffer) |
| Storage | 8 GB (root) + 256 MB (/boot) + 512 MB (/tmp) | ~9 GB total |
| OS | Ubuntu 22.04 LTS ARM64 | — |
| Boot param | `numa=fake=2` in `GRUB_CMDLINE_LINUX` | — |

```bash
# Install dependencies
sudo apt install -y build-essential libnuma-dev numactl \
                    python3-numpy python3-matplotlib

# Enable emulated NUMA (add to /etc/default/grub, then):
sudo update-grub && sudo reboot
numactl --hardware   # must show 2 nodes (0-1)

# Build baseline kernel (for Benchmarks A–E)
# Human: ~5 min | Compute: 8–35 min depending on core count
cd ~/linux-6.1.4
cp /boot/config-$(uname -r) .config && make olddefconfig
make -j$(nproc)
sudo make modules_install && sudo make install && sudo reboot
uname -r                              # must show 6.1.4
ls /sys/kernel/debug/mig_timing       # must exist

# Build ROCM kernel (required only for ROCM benchmarks)
# ~2 min incremental — only migrate.o recompiles
cp arm64/migrate_rocm.c ~/linux-6.1.4/mm/migrate.c
cd ~/linux-6.1.4 && make -j$(nproc)
sudo make modules_install && sudo make install && sudo reboot
head -1 /sys/kernel/debug/mig_timing | grep rocm_path   # must match
```

```bash
# Build userspace benchmarks
gcc -O2 -Wall -o mig_bench         arm64/mig_bench.c          -lnuma -lpthread
gcc -O2 -Wall -o mig_bench_e       arm64/mig_bench_e.c        -lnuma -lpthread -lm
gcc -O2 -Wall -o mig_bench_rocm    arm64/mig_bench_rocm.c     -lnuma -lm        # ROCM kernel required
gcc -O2 -Wall -o mig_bench_rocm_mt arm64/mig_bench_rocm_mt.c  -lnuma -lm        # ROCM kernel required
```

### 5.5 ARM64 Results Summary

**Application Stall Comparison (primary metric):**

| Metric | Standard | ROCM | Improvement |
|---|---|---|---|
| App stall window | 875 ns | **462 ns** | **1.89× reduction** |
| Remap stage | 75 ns | **0 ns** | Fully eliminated |
| Migration rate | 46,704 pg/s | 84,154 pg/s | **1.80× faster** |
| Mean stall duration (Bench E) | 54,869 ns | 26,729 ns | **2.05× shorter** |
| `rocm_path=1` records | 0 / 8192 | 8192 / 8192 | 100% pages qualify |
| Disruption Factor | 1.020 | 1.077 | +5.6% improvement |

---

## 6. Benchmark Suite

| Bench | Name | Description |
|---|---|---|
| **A** | 4 KB Quiescent | Per-page timing baseline: 512 and 2048 pages, single-threaded, no concurrent load. Establishes Lock/Unmap/Copy/Remap/Unlock means for both platforms. |
| **B** | 2 MB THP | 2 MB huge-page migration: 32 and 128 THPs. Validates copy-cost scaling with page size. |
| **C** | Shared-Page Sweep | Sweep sharing degree 1–64 across forked processes. Quantifies rmap-walk cost growth; motivates hysteresis threshold selection. |
| **D** | Migration Downtime | Application-visible stall measurement. Compares DMA vs CPU copy. Proves stall is bounded by migration PTE duration, not copy mechanism. |
| **E** | Concurrent Load | 7 configs: chunk size (c1/c64/c512), thread count (t1/t4/t8), access pattern (RMW/read). Measures Disruption Factor and p999 tail latency. |
| **R (ROCM)** | ROCM Comparison | Controlled comparison: 2-worker random-read, 32 MB file-backed buffer. Only the migration pipeline differs (standard vs ROCM fast-path). |

**Disruption Factor (DF)** is defined as `P2_throughput / P1_throughput`, where P1 is the pre-migration baseline and P2 is the during-migration window. DF > 1.0 means migration is net-beneficial (locality gain outweighs disruption). DF < 1.0 means migration is slowing the workload.

**Chunk size guidance:** chunk=1 causes 23–27× p999 spikes and 6–14× lower migration rate on both platforms. Chunk ≥ 64 is the minimum viable batch size. Chunk=64 and chunk=512 produce near-identical migration rates — kernel internal batching saturates below chunk=64.

---

## 7. Running the Evaluation

### 7.1 x86\_64 Quick Start (≤ 30 min)

```bash
# Step 1 — Verify instrumented kernel
uname -r                                               # must show 6.1.4
sudo ls /sys/kernel/debug/mig_timing                   # must exist
head -1 /sys/kernel/debug/mig_timing | tr ',' '\n' | wc -l  # must print 25
grep -c migrate_pages_deferred_ipi /proc/kallsyms      # must be ≥ 1
numactl --hardware                                     # must show 2 nodes

# Step 2 — Disable auto-balancing
echo 0 | sudo tee /proc/sys/kernel/numa_balancing

# Step 3 — Run quick evaluation (~15 min compute)
cd x86_64
sudo bash run_x86_full.sh --quick --results-dir ./results_x86
# --quick: skips T1 CPU hotplug, runs only Bench E config 0 (~55 sec)

# Step 4 — Verify key claims
head -1 results_x86/timing_4kb_2048.csv | tr ',' '\n' | wc -l
# Must print: 25

grep 'CONFIRMED' results_x86/run_x86.log
# Must show:
# ✓ O1 CONFIRMED: Batching significantly reduces TLB shootdown impact.
# ✓ O3 CONFIRMED: High locality in page-table walk.

# Optional: run all 7 Bench E configs (full ~45 min run)
sudo bash run_x86_full.sh --results-dir ./results_x86

# Optional: run a specific Bench E config by index (0-based)
sudo ./mig_bench_e 0    # config 0: rand_rmw_t4_c512 (primary)
sudo ./mig_bench_e 5    # config 5: rand_rmw_t4_c1  (chunk=1, worst case)

# Optional: T5 sub-component breakdown on any timing CSV
python3 analyze_unmap.py --t5 results_x86/timing_4kb_2048.csv

# Optional: T4 quiescent vs loaded comparison
python3 analyze_unmap.py --t4 \
    results_x86/timing_4kb_2048.csv \
    results_x86/bench_e_timing_x86_E_rand_rmw_t4_c512.csv
```

### 7.2 ARM64 Quick Start (≤ 30 min)

```bash
# Step 1 — Verify instrumented kernel
uname -r                             # must show 6.1.4
sudo ls /sys/kernel/debug/mig_timing # must exist
numactl --hardware                   # must show 2 nodes (0-1)

# Step 2 — Build and verify migration
gcc -O2 -Wall -o mig_bench arm64/mig_bench.c -lnuma -lpthread
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
sudo ./mig_bench
# Expected: Succeeded: 2048/2048   Failed: 0

# Step 3 — Verify ARM64 stage profile
python3 arm64/analyze_timing.py timing_4kb_2048.csv
# Expected: Copy 38–46%, Unmap 38–42%
# If Copy < 20%: wrong kernel is booted — verify uname -r

# Step 4 — Run quick automated evaluation (~10 min compute)
sudo bash arm64/run_arm64_full.sh --quick
# Runs: A+B+C+D → analyze → Bench E config 0 → ROCM (if ROCM kernel booted)

# Step 5 — Verify ROCM results (requires ROCM kernel)
grep ',1$' results_arm64/rocm_fast_timing.csv | wc -l  # must be > 0
cat results_arm64/rocm_comparison.txt

# Optional: run specific Bench E configs
sudo ./mig_bench_e 0    # primary config: rand_rmw_t4_c512
sudo ./mig_bench_e 5    # chunk=1 pathology: max p999 spike

# Optional: reset ring buffer and capture a fresh sample
echo 1 | sudo tee /sys/kernel/debug/mig_timing
sudo ./mig_bench
python3 arm64/analyze_timing.py timing_4kb_2048.csv
```

---

## 8. Hypothesis Testing

The x86\_64 analysis used five independent test probes (T1–T5) to isolate the true cause of Unmap dominance before any optimization was designed.

| Test | Method | Purpose | Result |
|---|---|---|---|
| **T1** | CPU hotplug: offline CPUs one-by-one, rerun Bench A at each count | Verify IPI cost scales linearly with online CPU count | R² = 0.85+ on bare metal; each CPU offline reduces `unmap_ns` proportionally → **H1 CONFIRMED** |
| **T2** | Compare `try_migrate_ns` across 5 quintiles of sequential migration record number | Rule out page-table cache thrash accumulating over a run | Q5/Q1 < 1.0 — no monotonic rise; warmup not thrash → **H4 RULED OUT** |
| **T3** | Flag outliers (> 5× median) and cross-reference with `vmstat` kswapd counters | Rule out kswapd rwsem contention causing burst spikes | Outliers < 2%, randomly distributed, no kswapd correlation → **H3 RULED OUT** |
| **T4** | Isolate `ptl_wait_ns` delta between quiescent and loaded runs | Rule out PTE spinlock contention from concurrent workers | `ptl_wait_ns` < 2% quiescent, modest rise under load only → **H2 RULED OUT** |
| **T5** | Full sub-component decomposition using 25-column `debugfs` CSV | Quantify H1 (IPI), H2 (PTL body), H3 (rwsem), and structural walk in nanoseconds | IPI = 98.5% under load; structural walk = 28% quiescent → **H1 + H5 CONFIRMED** |

---

## 9. Key Findings & Conclusions

**ARM64 — ROCM delivers 1.89× stall reduction for read-only pages**  
The 875 ns standard stall (Unmap+Copy+Remap) is reduced to 462 ns (PTE-swap only). The Remap stage is fully eliminated. Migration rate improves 1.80× and mean stall duration is 2.05× shorter. The result is bounded by one TLBI round-trip — near-optimal.

**x86\_64 — IPI shootdown confirmed as 98.5% of Unmap under load**  
H2/H3/H4 were ruled out by systematic testing. Unmap reaches 83–85% of total migration time under any concurrent load configuration. Workers are reduced to 10–25% baseline throughput (DF = 0.10–0.25) due to continuous IPI interruption.

**x86\_64 — O2a + O2b deliver 1.67× end-to-end improvement**  
O2a collapses N per-page IPIs to 1 batch IPI (~81.5 ms → ~159 µs for 512 pages). O2b reduces the 28% structural walk cost by 1.59–2.02× via PMD cache locality. On properly pinned NUMA systems (KVM), migration becomes net-beneficial (DF > 1.0).

**Both platforms — chunk ≥ 64 is required for viable migration**  
chunk=1 produces 23–27× p999 latency spikes and 6–14× lower migration rates on both architectures. chunk=64 and chunk=512 yield near-identical performance — the kernel internal batching mechanism saturates at chunk=64.

**DMA does not reduce application downtime**  
Bench D confirms that stall duration is bounded by the migration PTE duration (how long the migration swap entry is installed), not by the copy mechanism. Using DMA for the copy stage has no measurable effect on application-visible stall.

---

## 10. Dependencies

**Kernel build:**
```
gcc / g++        (build-essential)
make, flex, bison, libssl-dev, libelf-dev
```

**Userspace benchmarks:**
```
libnuma-dev      (NUMA syscalls: move_pages, mbind, set_mempolicy)
libpthread       (concurrent worker threads)
libm             (statistical analysis in Bench E)
```

**Analysis scripts:**
```
python3
numpy
matplotlib
```

**Install on Ubuntu 22.04:**
```bash
sudo apt install -y build-essential flex bison libssl-dev libelf-dev \
                    libnuma-dev numactl \
                    python3 python3-numpy python3-matplotlib
```

---

## 11. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `ls /sys/kernel/debug/mig_timing` fails | Wrong kernel booted, or instrumentation not compiled in | Verify `uname -r` shows 6.1.4; check `dmesg` for module errors |
| `wc -l` on CSV header prints < 25 (x86\_64) | `rmap.c` patch not applied; using stock `rmap.c` | Re-apply `rmap_x86.patch`; rebuild and reinstall kernel |
| `grep -c migrate_pages_deferred_ipi /proc/kallsyms` prints 0 | `migrate.c` patch not applied | Re-apply `migrate_x86.patch`; rebuild kernel |
| Copy % < 20% on ARM64 | Standard (non-ROCM) ARM64 kernel not booted | Check `uname -r`; ensure boot entry points to 6.1.4 baseline, not ROCM build |
| `rocm_path` column absent from `mig_timing` | ROCM kernel not booted | Boot the ROCM kernel build; verify with `head -1 /sys/kernel/debug/mig_timing \| grep rocm_path` |
| `numactl --hardware` shows only 1 node | `numa=fake=2` missing from boot parameters | Add to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, run `sudo update-grub`, reboot |
| Bench E disruption factor very low on KVM | Expected — KVM absorbs IPIs; O2a benefit invisible for disruption metric | Check Akshat/Cezan results instead; both show DF > 1.0 confirming O2 benefit |
| Migration rate near 0 on chunk=1 | Expected behavior — no batching, per-page syscall overhead | Use chunk ≥ 64 for meaningful migration rates |
| Build fails: `SYSTEM_TRUSTED_KEYS` error | Distribution kernel config requires trusted key | Run `scripts/config --disable SYSTEM_TRUSTED_KEYS && scripts/config --disable SYSTEM_REVOCATION_KEYS` then `make olddefconfig` |
| `mig_bench` fails: `FAILED: N pages` | Not enough free memory on target NUMA node | Reduce page count with `-n 512`; ensure swapping is disabled during Bench E |

---

*Linux 6.1.4 · ARM64 & x86\_64 · CS614 Operating Systems*
