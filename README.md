# NUMA Page Migration Analysis and Optimization
**Quantifying and Reducing Migration Cost in Linux 6.1.4 — ARM64 & x86\_64**

| | |
|---|---|
| **Course** | CS614 — Linux Kernel Programming, IIT Kanpur |
| **Team** | `team_name.ko` |
| **Members** |  Aayush Kumar (230027) · Yogit (211207) · Cezan Vispi Damania (230310) |
| **Kernel** | Linux 6.1.4 |
| **Evaluation** | Emulated NUMA (`numa=fake=2`) + real multi-node tiering systems |
| **Report** | [`project_report.pdf`](project_report.pdf) · [`Project_Presentation.pdf`](Project_Presentation.pdf) |

---

## Overview

NUMA page migration relocates memory pages to the node where they are accessed most frequently, improving data locality for running processes. In Linux 6.1.4, every migrated page stalls application threads for the full five-stage pipeline — Lock, Unmap, Copy, Remap, and Unlock — even in cases where the stall is architecturally unnecessary.

This project takes a measurement-first approach. The kernel migration pipeline is instrumented with nanosecond-precision timestamps via a `debugfs` CSV ring buffer, exposing the per-stage cost breakdown on both ARM64 and x86\_64. The dominant bottleneck on each architecture is isolated through systematic hypothesis testing before any optimization is proposed or implemented.

Experiments were conducted on both emulated NUMA environments (`numa=fake=2` on VirtualBox and KVM) and real multi-socket tiering systems, including an Akshat Xeon 6226R and a Cezan i7-13620H, to validate that results hold beyond a simulated topology.

---

## Objectives

1. **Instrument** the Linux 6.1.4 NUMA migration pipeline to measure per-stage latency with 5 ns resolution across both ARM64 and x86\_64.
2. **Identify** the dominant bottleneck on each architecture through controlled benchmarks and sub-component decomposition (T1–T5 analysis on x86\_64).
3. **Design and implement** targeted kernel optimizations in `mm/migrate.c` and `mm/rmap.c` that reduce application-visible stall with no correctness regressions.
4. **Validate** results across multiple machines — emulated and real NUMA — reporting Disruption Factor and p999 tail latency alongside raw throughput.

---

## Architecture-Specific READMEs

Each subdirectory contains a self-contained README with full setup instructions, kernel build steps, benchmark descriptions, expected outputs, and troubleshooting guidance.

| Subdirectory | Optimization | Detailed README |
|---|---|---|
| `arm64_files/` | **ROCM** — Read-Only Copy Migration fast path | [`arm64_files/README.md`](arm64_files/README.md) |
| `x86_files/` | **Deferred Batch IPI Shootdown + Address-Ordered Sort** | [`x86_files/README.md`](x86_files/README.md) |

> Start with the README for your target architecture. This top-level README provides a project-wide summary only.

---

## Repository Structure

```
Memory_Migration_CS614/
│
├── README.md                      ← This file
├── Project_Presentation.pdf       ← Slide deck
├── project_report.pdf             ← Full written report
│
├── arm64_files/                   ← ARM64 artifact: ROCM optimization
│   ├── README.md                  ← Full ARM64 setup & evaluation guide
│   ├── migrate.c                  ← ROCM-patched mm/migrate.c
│   ├── migrate_arm.patch          ← Unified diff against stock Linux 6.1.4
│   ├── mig_bench.c                ← Benchmarks A, B, C, D (userspace)
│   ├── mig_bench_e.c              ← Benchmark E: 7 concurrent-load configs
│   ├── mig_bench_rocm.c           ← ROCM vs standard, single-threaded
│   ├── mig_bench_rocm_mt.c        ← ROCM vs standard, multithreaded
│   ├── analyze_timing.py          ← Stage breakdown analysis + PNG plots
│   ├── analyze_downtime.py        ← Bench D downtime analysis (24 MHz timer)
│   ├── run_arm64_full.sh          ← Full automation script (9 steps)
│   └── results_arm64/             ← Output CSVs and plots
│
└── x86_files/                     ← x86_64 artifact: Deferred Batch IPI + Sort
    ├── README.md                  ← Full x86_64 setup & evaluation guide
    ├── migrate.c.txt              ← Optimised mm/migrate.c
    ├── rmap.c.txt                 ← Optimised mm/rmap.c (TTU_BATCH_FLUSH + T5 timing)
    ├── migrate_x86.patch          ← Unified diff against stock Linux 6.1.4
    ├── rmap_x86.patch             ← Unified diff against stock Linux 6.1.4
    ├── mig_bench_x86.c            ← Benchmarks A, B, C, D (userspace)
    ├── mig_bench_e_x86.c          ← Benchmark E: 7 concurrent-load configs
    ├── analyze_timing.py          ← Stage breakdown analysis + PNG plots
    ├── analyze_unmap.py           ← T1–T5 sub-component decomposition
    ├── analyze_bench_e_x86.py     ← Bench E disruption factor + p999 plots
    ├── run_t1.sh                  ← T1: CPU hotplug IPI isolation driver
    ├── run_x86_full.sh            ← Full automation script (9 steps)
    └── results_x86/               ← Output CSVs and plots
```

---

## Results

### ARM64 — ROCM: Read-Only Copy Migration

On ARM64, hardware `TLBI VAE1IS + DSB ISH` broadcast eliminates software IPI overhead entirely. The bottleneck shifts to the Copy stage, which accounts for 46% of total migration time. For file-backed read-only pages, faulting threads stall through the full Unmap + Copy + Remap window (875 ns) even though the page content is stable throughout — making the stall architecturally unnecessary.

ROCM resolves this by copying the page while source PTEs remain live, then atomically replacing the PTE translation with a single CMPXCHG + TLBI. No migration swap entry is ever installed, the Remap stage is fully eliminated, and the application stall is reduced to one TLBI round-trip — near-optimal for this hardware.

| Metric | Standard Pipeline | ROCM Fast Path | Improvement |
|---|---|---|---|
| Application stall window | 875 ns | **462 ns** | **1.89×** |
| Remap stage cost | 75 ns | **0 ns** | Fully eliminated |
| Migration rate | 46,704 pg/s | **84,154 pg/s** | **1.80×** |
| Mean stall duration (Bench E) | 54,869 ns | **26,729 ns** | **2.05×** |
| Disruption Factor | 1.020 | **1.077** | +5.6% |

---

### x86\_64 — Deferred Batch IPI Shootdown + Address-Ordered Sort

On x86\_64, per-page IPI shootdown (`flush_tlb_others()`) costs ~159 µs per page under concurrent load — a 221× increase over quiescent. For a 512-page batch this totals 81.5 ms of IPI wait with the standard path. Five independent hypothesis tests (T1–T5) confirmed IPI shootdown as 98.5% of Unmap cost under load and systematically ruled out PTE spinlock contention, kswapd rwsem contention, and page-table cache thrash as contributing causes.

The deferred batch approach phase-separates the migration loop so N pages share a single IPI round-trip instead of N separate ones. The address-ordered sort reduces the 28% structural walk cost — identified as a secondary bottleneck — by keeping PMD table entries cache-hot across the batch. Both optimizations were validated on emulated NUMA (VirtualBox, KVM) and real multi-socket tiering hardware (Akshat Xeon 6226R, Cezan i7-13620H).

| Metric | Baseline | Optimised | Improvement |
|---|---|---|---|
| `try_migrate_ns` median | 2,486 ns | **1,488 ns** | **1.67×** |
| IPI wait (`ipi_wait_ns`) | 632 ns | **335 ns** | **1.89×** |
| Structural walk | 683 ns | **429 ns** | **1.59×** |
| `unmap_ns` total | 2,609 ns | **1,642 ns** | **1.59×** |
| End-to-end `total_ns` | 5,781 ns | **4,633 ns** | **1.25×** |
| Disruption Factor (VirtualBox, real IPI) | 0.133 | **0.767** | 5.8× better |
| Disruption Factor (KVM, real tiering HW) | — | **1.093 – 1.098** | Net-beneficial |

---

*Linux 6.1.4 · CS614 Operating Systems · IIT Kanpur*
