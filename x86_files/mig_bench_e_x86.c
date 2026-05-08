/*
 * mig_bench_e_x86.c — Benchmark E: Multi-Threaded NUMA Migration Characterization
 * Target: x86_64, Linux 6.1.4, emulated NUMA (numa=fake=2), 4KB pages
 *
 * Three-phase benchmark with concurrent worker threads and a migrator:
 *
 *   Phase 1 (Baseline)    — N worker threads pin to node1, access 512 MB
 *                           buffer bound to node0 (purely remote access)
 *   Phase 2 (Migration)   — migrator walks buffer node0→node1 in chunks via
 *                           move_pages(); workers continue uninterrupted
 *   Phase 3 (Steady)      — workers access pages now on node1 (local)
 *
 * Application-side metrics: per-phase ops/s, GB/s, p50/p95/p99/p999 latency
 * Migration-side metrics:   pages/s, failure rate, duration
 * Kernel ring buffer:       5-stage per-page timings via debugfs (same as A/B)
 *
 * Key derived metrics (computed in analyze_bench_e.py):
 *   disruption_factor  = Phase2_tput / Phase1_tput
 *   migration_benefit  = Phase3_tput / Phase1_tput
 *   p999_spike_ratio   = Phase2_p999 / Phase1_p999
 *   break_even_time    = lost_work / gain_rate
 *
 * Timing: LFENCE-serialised RDTSC, calibrated against CLOCK_MONOTONIC at
 * startup. TSC is invariant on Nehalem+ (P-state and C-state independent).
 * LFENCE before each RDTSC prevents the OoO pipeline from reordering the
 * counter read past the memory access being timed — equivalent to ARM64's
 * ISB before MRS CNTVCT_EL0. Calibration: 100 ms CLOCK_MONOTONIC window
 * at startup gives ns/tick to ~0.01% accuracy.
 *
 * Build:
 *   gcc -O2 -Wall -o mig_bench_e_x86 mig_bench_e_x86.c -lnuma -lpthread -lm
 *
 * Run all 7 configs:
 *   sudo ./mig_bench_e_x86
 * Run single config by 0-based index:
 *   sudo ./mig_bench_e_x86 0
 *
 * Outputs (per config, suffixed _x86 to avoid collision with ARM64 results):
 *   bench_e_workers_x86_<label>.csv   — per-thread per-phase application stats
 *   bench_e_migrator_x86_<label>.csv  — migration throughput/failure stats
 *   bench_e_timing_x86_<label>.csv    — kernel ring buffer 5-stage timing sample
 *
 * NOTE — numa=fake=2 caveat:
 *   Both nodes share one physical DIMM. "Remote" vs "local" is a topology
 *   metadata distinction, not a real DRAM latency gap. Phase 1 vs Phase 3
 *   throughput differences will be small. The migration-PTE stall mechanism
 *   (p999 spike in Phase 2) is real and architecture-correct. Stage ratios
 *   in the ring buffer remain structurally valid for cross-stage comparisons.
 *
 * NOTE — x86 vs ARM64 differences:
 *   Unmap stage: x86 issues INVLPG + IPI shootdown per PTE (try_to_migrate()
 *   cost ~2.68 µs vs ARM64's 0.38 µs). Expect Unmap to dominate over Copy
 *   even under load, unlike ARM64 where they compete. Stage reversal under
 *   concurrent load (Unmap rising) may be amplified on x86 due to the higher
 *   per-PTE IPI cost. Migration rate will be similar (kernel copy path is
 *   the same); p999 spikes at chunk=1 should be comparable or larger.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>
#include <numaif.h>
#include <numa.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

/* ─────────────────────────────────────────────────────────────────── */
/* Configuration                                                       */
/* ─────────────────────────────────────────────────────────────────── */

#define PAGE_SZ          4096UL
#define BUF_PAGES        131072UL             /* 512 MB = 131072 × 4 KB */
#define BUF_SZ           (BUF_PAGES * PAGE_SZ)
#define DEBUGFS_PATH     "/sys/kernel/debug/mig_timing"
#define MAX_SAMPLES      10000                /* reservoir size per phase/thread */
#define SAMPLE_EVERY     64                   /* time 1-in-N ops to bound overhead */
#define MAX_THREADS      8
#define MAX_NODE_CPUS    16

/* ─────────────────────────────────────────────────────────────────── */
/* Types                                                               */
/* ─────────────────────────────────────────────────────────────────── */

typedef enum { PAT_SEQ, PAT_RAND, PAT_STRIDE16 } access_pat_t;
typedef enum { OP_READ, OP_WRITE, OP_RMW }        op_type_t;

typedef struct {
    access_pat_t pat;
    op_type_t    op;
    int          nthreads;
    int          chunk_pages;        /* pages per move_pages() call */
    int          phase1_s;
    int          phase2_s;
    int          phase3_s;
    char         label[64];
} bench_cfg_t;

/*
 * Per-thread, per-phase statistics.
 * Written exclusively by the owning worker thread; read by main after join.
 * No synchronisation needed for the data fields (join provides the barrier).
 *
 * Memory note: MAX_THREADS × 3 phases × MAX_SAMPLES × 8 B = ~1.9 MB total.
 * Allocated on the heap via calloc() in run_bench().
 */
typedef struct {
    uint64_t ops;
    uint64_t bytes;
    uint64_t dur_ns;
    /* Reservoir of per-op latency samples (ns), filled by worker */
    uint64_t samp[MAX_SAMPLES];
    int      n_samp;      /* filled slots, capped at MAX_SAMPLES */
    int      n_seen;      /* total timed ops (for reservoir probability) */
    /* Percentiles — computed by worker before it exits */
    double   p50, p95, p99, p999, mean_ns, stddev_ns;
} pstats_t;

/* Shared state between main thread, worker threads, and the migrator */
typedef struct {
    /*
     * Phase signal: 0=init (workers spin-wait), 1=baseline, 2=migration,
     * 3=steady, 4=done. Written by main, read by all threads.
     */
    atomic_int  phase;
    void       *buf;
    bench_cfg_t cfg;
    int         src_node, dst_node;
    int         node0_cpu;                   /* CPU for migrator */
    int         node1_cpus[MAX_NODE_CPUS];   /* CPUs for workers */
    int         n_node1_cpus;
    /* Per-thread per-phase results: ws[thread][phase_idx 0..2] */
    pstats_t    ws[MAX_THREADS][3];
    /* Migrator results — written by migrator thread, read by main after join */
    uint64_t    mig_attempted;
    uint64_t    mig_succeeded;
    uint64_t    mig_failed;
    uint64_t    mig_dur_ns;
    /* Output filename prefix — set before threads launch */
    char        prefix[128];
} shared_t;

typedef struct { shared_t *sh; int tid; } warg_t;

/* ─────────────────────────────────────────────────────────────────── */
/* Utility                                                             */
/* ─────────────────────────────────────────────────────────────────── */

static void die(const char *m)
{
    fprintf(stderr, "FATAL: %s: %s\n", m, strerror(errno));
    exit(EXIT_FAILURE);
}

static void warn_msg(const char *m)
{
    fprintf(stderr, "WARN: %s: %s\n", m, strerror(errno));
}

/*
 * x86_64 TSC timing.
 *
 * The invariant TSC (Nehalem+) increments at a fixed frequency regardless
 * of P-states or C-states. We calibrate ns/tick once at startup using a
 * 100 ms CLOCK_MONOTONIC window, then convert all TSC deltas to ns.
 *
 * LFENCE before RDTSC: serialises the instruction stream so the counter
 * read cannot be speculated past the memory operation being timed. This
 * is equivalent to ARM64's ISB before MRS CNTVCT_EL0. Without it, the
 * OoO pipeline may reorder the RDTSC before the store/load, producing
 * underestimates. RDTSCP would also work but adds unnecessary overhead
 * since we pin threads to specific CPUs (core-ID check is redundant).
 *
 * Cost: LFENCE+RDTSC ≈ 20–30 cycles (≈6–10 ns at 3 GHz). Comparable to
 * ARM64 VDSO clock_gettime (5–10 ns). Sampling every SAMPLE_EVERY=64 ops
 * keeps the measurement overhead below 0.1% of total execution.
 */
static double g_ns_per_tick = 1.0;   /* calibrated in calibrate_tsc() */

static void calibrate_tsc(void)
{
    uint32_t lo, hi;
    uint64_t t0, t1, ticks;
    struct timespec ts0, ts1;
    uint64_t real_ns;

    /*
     * CPUID: full serialisation barrier before the calibration window.
     * Stronger than LFENCE — drains all speculative work before t0.
     */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0) : "memory");

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    __asm__ volatile ("lfence" ::: "memory");
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    t0 = ((uint64_t)hi << 32) | lo;

    usleep(100000);   /* 100 ms — amortises scheduling noise */

    __asm__ volatile ("lfence" ::: "memory");
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    t1 = ((uint64_t)hi << 32) | lo;
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    ticks   = t1 - t0;
    real_ns = (uint64_t)(ts1.tv_sec  - ts0.tv_sec)  * 1000000000ULL
            + (uint64_t)(ts1.tv_nsec - ts0.tv_nsec);

    g_ns_per_tick = (double)real_ns / (double)ticks;
    printf("  TSC calibration: %.3f ns/tick  (%.0f MHz)\n",
           g_ns_per_tick, 1000.0 / g_ns_per_tick);
}

/*
 * now_ns — LFENCE-serialised RDTSC, converted to nanoseconds.
 * On x86_64 this is the correct equivalent of ARM64's:
 *   ISB; MRS x0, CNTVCT_EL0
 */
static inline uint64_t now_ns(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence" ::: "memory");
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t ticks = ((uint64_t)hi << 32) | lo;
    return (uint64_t)((double)ticks * g_ns_per_tick);
}

static void pin_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s) < 0)
        warn_msg("sched_setaffinity");
}

/*
 * parse_cpulist — read /sys/devices/system/node/nodeN/cpulist.
 * Handles ranges ("2-3") and comma-separated lists ("0,2-3,5").
 * Returns number of CPUs found.
 */
static int parse_cpulist(const char *path, int *out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256];
    int  n = 0;
    if (fgets(buf, sizeof(buf), f)) {
        char *p = buf;
        while (*p && *p != '\n' && n < max) {
            int a = -1, b = -1;
            if (sscanf(p, "%d-%d", &a, &b) == 2) {
                for (int i = a; i <= b && n < max; i++) out[n++] = i;
            } else if (sscanf(p, "%d", &a) == 1) {
                out[n++] = a;
            }
            while (*p && *p != ',' && *p != '\n') p++;
            if (*p == ',') p++;
        }
    }
    fclose(f);
    return n;
}

static void timing_reset(void)
{
    int fd = open(DEBUGFS_PATH, O_WRONLY);
    if (fd < 0) { warn_msg("open debugfs (reset)"); return; }
    ssize_t wr_ret = write(fd, "1", 1);
    (void)wr_ret;
    close(fd);
    usleep(20000);   /* allow kernel reset to drain */
}

static void timing_read(const char *outfile)
{
    char    buf[8192];
    ssize_t n;
    int rfd = open(DEBUGFS_PATH, O_RDONLY);
    if (rfd < 0) { warn_msg("open debugfs (read)"); return; }
    int wfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) { warn_msg("open ring buf output"); close(rfd); return; }
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        ssize_t wret = write(wfd, buf, n);
        (void)wret;
    }
    close(rfd);
    close(wfd);
}

/*
 * sample_placement — verify page placement using move_pages() query mode
 * (NULL nodes argument). Checks 'nsamp' evenly-spaced pages in the buffer.
 * Returns the count confirmed on 'expected_node'.
 */
static int sample_placement(void *buf, size_t npages, int expected, int nsamp)
{
    if ((size_t)nsamp > npages) nsamp = (int)npages;
    size_t stride = npages / (size_t)nsamp;
    void **pgs = malloc((size_t)nsamp * sizeof(void *));
    int   *st  = malloc((size_t)nsamp * sizeof(int));
    if (!pgs || !st) { free(pgs); free(st); return -1; }

    for (int i = 0; i < nsamp; i++)
        pgs[i] = (char *)buf + (size_t)i * stride * PAGE_SZ;

    move_pages(0, (unsigned long)nsamp, pgs, NULL, st, 0);

    int ok = 0;
    for (int i = 0; i < nsamp; i++)
        if (st[i] == expected) ok++;

    free(pgs);
    free(st);
    return ok;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Percentile computation                                              */
/* ─────────────────────────────────────────────────────────────────── */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/*
 * compute_pct — sort the reservoir in place, then compute percentiles.
 * Called by each worker thread before it returns.
 */
static void compute_pct(pstats_t *ps)
{
    int n = ps->n_samp;
    if (n == 0) {
        ps->p50 = ps->p95 = ps->p99 = ps->p999 = ps->mean_ns = ps->stddev_ns = 0;
        return;
    }
    qsort(ps->samp, (size_t)n, sizeof(uint64_t), cmp_u64);

    /* Clamp indices to valid range */
#define PCT(f) ps->samp[(int)((f) * (n - 1))]
    ps->p50  = (double)PCT(0.500);
    ps->p95  = (double)PCT(0.950);
    ps->p99  = (double)PCT(0.990);
    ps->p999 = (double)PCT(0.999);
#undef PCT

    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += (double)ps->samp[i];
    ps->mean_ns = sum / n;

    double sq = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)ps->samp[i] - ps->mean_ns;
        sq += d * d;
    }
    ps->stddev_ns = (n > 1) ? sqrt(sq / (n - 1)) : 0.0;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Worker Thread                                                       */
/* ─────────────────────────────────────────────────────────────────── */

/*
 * worker — one thread per bench run, pinned to a node1 CPU.
 *
 * Continuously accesses the shared buffer according to the configured
 * pattern and operation type. Tracks per-phase statistics independently.
 * Phase transitions are detected by polling sh->phase at the top of
 * each iteration — the polling overhead is negligible (~1 ns/check).
 *
 * Latency is sampled every SAMPLE_EVERY ops using a reservoir of
 * MAX_SAMPLES entries. Two LFENCE+RDTSC calls bracket the target
 * memory operation. LFENCE cost ~20-30 cycles (≈6-10 ns at 3 GHz);
 * small relative to the NUMA access latency being measured (50–500 ns).
 *
 * A private LCG provides fast, lock-free random page selection.
 * The LCG seed is salted per thread to avoid correlated access patterns.
 */
static void *worker(void *arg)
{
    warg_t   *wa  = (warg_t *)arg;
    shared_t *sh  = wa->sh;
    int       tid = wa->tid;

    /* Pin to a node1 CPU; round-robin if fewer CPUs than threads */
    if (sh->n_node1_cpus > 0)
        pin_cpu(sh->node1_cpus[tid % sh->n_node1_cpus]);

    char         *buf = (char *)sh->buf;
    size_t        np  = BUF_PAGES;
    access_pat_t  pat = sh->cfg.pat;
    op_type_t     op  = sh->cfg.op;
    pstats_t     *ps  = sh->ws[tid];   /* ps[0..2] = phases 1..3 */

    /*
     * LCG state: Knuth's multiplier + odd addend.
     * Per-thread salt prevents all workers from accessing the same pages
     * simultaneously, which would create artificial cache-line contention.
     */
    uint64_t rng = 0xdeadbeefcafe0000ULL ^ ((uint64_t)tid * 0x9e3779b97f4a7c15ULL);

    /* Stagger sequential/stride16 start positions across threads */
    size_t pidx = ((size_t)tid * (np / (size_t)sh->cfg.nthreads)) % np;

    uint64_t op_cnt = 0;   /* for SAMPLE_EVERY throttle */

    /* Spin until main sets phase = 1 (clean start synchronisation) */
    while (atomic_load_explicit(&sh->phase, memory_order_acquire) == 0)
#ifdef __aarch64__
        __asm__ volatile ("yield");
#else
        __asm__ volatile ("pause");
#endif

    int      cur_ph       = 1;
    int      pi           = 0;           /* phase index: cur_ph - 1 */
    uint64_t phase_start  = now_ns();

    /* ── Main access loop ── */
    for (;;) {
        int ph = atomic_load_explicit(&sh->phase, memory_order_acquire);

        /* ── Phase transition ── */
        if (ph != cur_ph) {
            uint64_t now  = now_ns();
            ps[pi].dur_ns = now - phase_start;

            if (ph >= 4) break;   /* done signal from main */

            cur_ph       = ph;
            pi           = ph - 1;
            phase_start  = now;
            op_cnt       = 0;
        }

        /* ── Advance page index per access pattern ── */
        switch (pat) {
        case PAT_SEQ:
            /* Linear scan, wraps at end of buffer */
            pidx = (pidx + 1) % np;
            break;
        case PAT_RAND:
            /* LCG: fast, no atomics, no libc rand() global lock */
            rng  = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            pidx = (rng >> 17) % np;
            break;
        case PAT_STRIDE16:
            /*
             * Stride of 16 pages = 64 KB between accesses.
             * Defeats hardware prefetcher; maximises TLB miss rate.
             * Stress-tests the remote-access NUMA latency path.
             */
            pidx = (pidx + 16) % np;
            break;
        }

        volatile uint64_t *ptr =
            (volatile uint64_t *)((char *)buf + pidx * PAGE_SZ);

        int do_time = (++op_cnt % SAMPLE_EVERY == 0);
        uint64_t t0 = 0;
        if (do_time) t0 = now_ns();

        /* ── Perform the target memory operation ── */
        switch (op) {
        case OP_READ:
        {
            uint64_t v = *ptr;
            (void)v;           /* prevent dead-store elimination */
            break;
        }
        case OP_WRITE:
            *ptr = op_cnt;
            break;
        case OP_RMW:
            /*
             * 64-bit atomic fetch-add with RELAXED ordering.
             * On ARM64: LDADDAL or LDADD depending on ordering.
             * Generates a real RMW bus transaction — stress-tests
             * the coherence fabric under concurrent access.
             */
            __atomic_fetch_add((uint64_t *)ptr, 1, __ATOMIC_RELAXED);
            break;
        }

        /* ── Reservoir sampling of per-op latency ── */
        if (do_time) {
            uint64_t lat = now_ns() - t0;
            int n = ps[pi].n_seen++;
            if (ps[pi].n_samp < MAX_SAMPLES) {
                ps[pi].samp[ps[pi].n_samp++] = lat;
            } else {
                /*
                 * Vitter's reservoir: replace a uniformly random existing
                 * slot with probability MAX_SAMPLES / (n + 1).
                 * Reuse the LCG to avoid rand() overhead.
                 */
                rng = rng * 6364136223846793005ULL + 1ULL;
                uint64_t r = rng % ((uint64_t)n + 1);
                if (r < (uint64_t)MAX_SAMPLES)
                    ps[pi].samp[r] = lat;
            }
        }

        ps[pi].ops++;
        ps[pi].bytes += 8;   /* one 8-byte word per op */
    }

    /* Compute percentiles from each phase's reservoir before exiting */
    for (int i = 0; i < 3; i++)
        compute_pct(&ps[i]);

    return NULL;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Migrator Thread                                                     */
/* ─────────────────────────────────────────────────────────────────── */

/*
 * migrator — single thread, pinned to node0.
 *
 * Waits for Phase 2, then walks the 512 MB buffer sequentially, calling
 * move_pages() in chunks of cfg.chunk_pages to migrate pages from
 * src_node (node0) to dst_node (node1).
 *
 * At ~2 µs/page and 131072 pages, one full pass takes ~262 ms.
 * The 30 s Phase 2 window covers the full migration plus steady-state
 * observation time. The migrator exits after one pass — the remaining
 * time in Phase 2 shows workers with pages increasingly on node1.
 *
 * We use MPOL_MF_MOVE_ALL because workers share the buffer (MAP_SHARED),
 * meaning mapcount > 1 for pages accessed by multiple threads. Without
 * MOVE_ALL, move_pages() would refuse to migrate those pages.
 *
 * Workers will encounter migration PTEs during this phase:
 *   worker access → Data Abort (migration PTE installed by migrator)
 *   → fault handler → migration_entry_wait() → thread sleeps
 *   → migrator calls folio_unlock() after set_pte_at()
 *   → worker resumes
 * This sleep-wake cycle appears as a p999 spike in the latency reservoir.
 */
static void *migrator(void *arg)
{
    shared_t *sh = (shared_t *)arg;

    /* Pin to node0: keeps migrator off the node1 CPUs used by workers */
    pin_cpu(sh->node0_cpu);

    /* Spin until Phase 2 */
    while (atomic_load_explicit(&sh->phase, memory_order_acquire) < 2)
#ifdef __aarch64__
        __asm__ volatile ("yield");
#else
        __asm__ volatile ("pause");
#endif

    int   chunk = sh->cfg.chunk_pages;
    char *buf   = (char *)sh->buf;

    void **pgs    = malloc((size_t)chunk * sizeof(void *));
    int  *nodes   = malloc((size_t)chunk * sizeof(int));
    int  *status  = malloc((size_t)chunk * sizeof(int));
    if (!pgs || !nodes || !status) {
        warn_msg("migrator malloc");
        free(pgs); free(nodes); free(status);
        return NULL;
    }

    for (int i = 0; i < chunk; i++)
        nodes[i] = sh->dst_node;

    uint64_t attempted = 0, succeeded = 0, failed = 0;
    uint64_t t0 = now_ns();

    /* ── Single forward pass through the buffer ── */
    for (size_t off = 0; off < BUF_PAGES; off += (size_t)chunk) {
        /* Abort early if main signalled Phase 3 before pass completes */
        if (atomic_load_explicit(&sh->phase, memory_order_relaxed) > 2)
            break;

        int n = (off + (size_t)chunk <= BUF_PAGES)
                ? chunk
                : (int)(BUF_PAGES - off);

        for (int i = 0; i < n; i++)
            pgs[i] = buf + (off + (size_t)i) * PAGE_SZ;

        move_pages(0, (unsigned long)n, pgs, nodes, status, MPOL_MF_MOVE_ALL);

        for (int i = 0; i < n; i++) {
            attempted++;
            /*
             * move_pages() sets status[i] to the destination node number
             * on success, or to -errno on failure (e.g. -ENOENT if the
             * page is already on the destination node, -EACCES if locked).
             */
            if (status[i] >= 0)
                succeeded++;
            else
                failed++;
        }
    }

    sh->mig_dur_ns    = now_ns() - t0;
    sh->mig_attempted = attempted;
    sh->mig_succeeded = succeeded;
    sh->mig_failed    = failed;

    free(pgs);
    free(nodes);
    free(status);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────── */
/* CSV output                                                          */
/* ─────────────────────────────────────────────────────────────────── */

static void write_worker_csv(shared_t *sh)
{
    char path[256];
    snprintf(path, sizeof(path), "bench_e_workers_x86_%s.csv", sh->prefix);
    FILE *f = fopen(path, "w");
    if (!f) { warn_msg("open worker CSV"); return; }

    fprintf(f, "config,phase,thread_id,ops,bytes,dur_ns,"
               "p50_ns,p95_ns,p99_ns,p999_ns,mean_ns,stddev_ns,n_samples\n");

    static const char *pname[] = { "baseline", "migration", "steady" };

    for (int ph = 0; ph < 3; ph++) {
        for (int t = 0; t < sh->cfg.nthreads; t++) {
            pstats_t *ps = &sh->ws[t][ph];
            fprintf(f, "%s,%s,%d,%lu,%lu,%lu,"
                       "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n",
                    sh->prefix, pname[ph], t,
                    (unsigned long)ps->ops,
                    (unsigned long)ps->bytes,
                    (unsigned long)ps->dur_ns,
                    ps->p50, ps->p95, ps->p99, ps->p999,
                    ps->mean_ns, ps->stddev_ns, ps->n_samp);
        }
    }
    fclose(f);
    printf("  Worker CSV    → %s\n", path);
}

static void write_migrator_csv(shared_t *sh)
{
    char path[256];
    snprintf(path, sizeof(path), "bench_e_migrator_x86_%s.csv", sh->prefix);
    FILE *f = fopen(path, "w");
    if (!f) { warn_msg("open migrator CSV"); return; }

    double pps = (sh->mig_dur_ns > 0)
                 ? (double)sh->mig_succeeded / ((double)sh->mig_dur_ns / 1e9)
                 : 0.0;
    double fr  = (sh->mig_attempted > 0)
                 ? (double)sh->mig_failed / (double)sh->mig_attempted
                 : 0.0;

    fprintf(f, "config,attempted,succeeded,failed,dur_ns,pages_per_sec,failure_rate\n");
    fprintf(f, "%s,%lu,%lu,%lu,%lu,%.0f,%.4f\n",
            sh->prefix,
            (unsigned long)sh->mig_attempted,
            (unsigned long)sh->mig_succeeded,
            (unsigned long)sh->mig_failed,
            (unsigned long)sh->mig_dur_ns,
            pps, fr);
    fclose(f);
    printf("  Migrator CSV  → %s\n", path);
}

/* ─────────────────────────────────────────────────────────────────── */
/* Console summary                                                     */
/* ─────────────────────────────────────────────────────────────────── */

static void print_summary(shared_t *sh)
{
    static const char *pname[] = { "Baseline ", "Migration", "Steady   " };

    printf("\n  %-10s  %13s  %9s  %9s  %10s\n",
           "Phase", "Ops/s (total)", "GB/s", "p50 ns", "p999 ns");
    printf("  %-10s  %13s  %9s  %9s  %10s\n",
           "─────", "─────────────", "────────", "──────", "───────");

    for (int ph = 0; ph < 3; ph++) {
        double total_ops = 0.0, sum_dur = 0.0;
        double sum_p50 = 0.0, sum_p999 = 0.0;
        int    nt = sh->cfg.nthreads;

        for (int t = 0; t < nt; t++) {
            pstats_t *ps = &sh->ws[t][ph];
            total_ops += (double)ps->ops;
            sum_dur   += (double)ps->dur_ns;
            sum_p50   += ps->p50;
            sum_p999  += ps->p999;
        }
        double avg_dur_s = (nt > 0) ? (sum_dur / nt / 1e9) : 1.0;
        double ops_s     = (avg_dur_s > 0) ? total_ops / avg_dur_s : 0.0;
        double gb_s      = ops_s * 8.0 / 1e9;

        printf("  %-10s  %13.3e  %9.4f  %9.0f  %10.0f\n",
               pname[ph], ops_s, gb_s,
               (nt > 0) ? sum_p50  / nt : 0.0,
               (nt > 0) ? sum_p999 / nt : 0.0);
    }

    double pps = (sh->mig_dur_ns > 0)
                 ? (double)sh->mig_succeeded / ((double)sh->mig_dur_ns / 1e9)
                 : 0.0;
    printf("\n  Migration: %lu attempted, %lu succeeded, %lu failed\n",
           (unsigned long)sh->mig_attempted,
           (unsigned long)sh->mig_succeeded,
           (unsigned long)sh->mig_failed);
    printf("  Migration: %.0f pages/s  duration %.0f ms  failure %.1f%%\n",
           pps,
           (double)sh->mig_dur_ns / 1e6,
           (sh->mig_attempted > 0)
               ? 100.0 * (double)sh->mig_failed / (double)sh->mig_attempted
               : 0.0);
}

/* ─────────────────────────────────────────────────────────────────── */
/* Run one benchmark configuration                                     */
/* ─────────────────────────────────────────────────────────────────── */

static void run_bench(bench_cfg_t *cfg, int src, int dst,
                      int node0_cpu, int *n1cpus, int nn1)
{
    /*
     * Heap-allocate shared_t: the ws[][] reservoir arrays total ~1.9 MB.
     * calloc() zeroes everything — ops/n_samp/n_seen start at 0 correctly.
     */
    shared_t *sh = calloc(1, sizeof(shared_t));
    if (!sh) die("calloc shared_t");

    sh->cfg       = *cfg;
    sh->src_node  = src;
    sh->dst_node  = dst;
    sh->node0_cpu = node0_cpu;
    sh->n_node1_cpus = (nn1 < MAX_NODE_CPUS) ? nn1 : MAX_NODE_CPUS;
    for (int i = 0; i < sh->n_node1_cpus; i++)
        sh->node1_cpus[i] = n1cpus[i];
    snprintf(sh->prefix, sizeof(sh->prefix), "%s", cfg->label);
    atomic_store(&sh->phase, 0);

    /* ── Allocate and bind 512 MB buffer to src_node (node 0) ── */
    printf("\n  Allocating %lu MB buffer on node %d ...\n",
           BUF_SZ / (1024UL * 1024UL), src);
    sh->buf = mmap(NULL, BUF_SZ, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (sh->buf == MAP_FAILED) die("mmap buffer");

    /*
     * mbind() the virtual range to src_node before touching it.
     * With numa=fake=2 this sets the allocation policy for the range;
     * all subsequent page faults will allocate from node0's memory half.
     */
    unsigned long nmask = 1UL << (unsigned)src;
    if (mbind(sh->buf, BUF_SZ, MPOL_BIND, &nmask, 64, MPOL_MF_MOVE) < 0)
        warn_msg("mbind (fake NUMA may ignore policy)");

    /* Fault in all pages — physically allocates them on src_node */
    printf("  Faulting in %lu pages (this is the initial memset) ...\n",
           BUF_PAGES);
    memset(sh->buf, 0xAA, BUF_SZ);

    int pre_ok = sample_placement(sh->buf, BUF_PAGES, src, 256);
    printf("  Pre-migration: %d/256 sampled pages on node %d (src)\n",
           pre_ok, src);
    if (pre_ok < 200)
        printf("  NOTE: fewer than expected pages on node %d — "
               "numa=fake=2 policy may be approximate\n", src);

    /* ── Launch worker threads (all pin to node1 CPUs) ── */
    pthread_t wt[MAX_THREADS];
    warg_t    wa[MAX_THREADS];
    for (int i = 0; i < cfg->nthreads; i++) {
        wa[i].sh  = sh;
        wa[i].tid = i;
        if (pthread_create(&wt[i], NULL, worker, &wa[i]) != 0)
            die("pthread_create worker");
    }

    /* ── Launch migrator (spins at phase < 2) ── */
    pthread_t mt;
    if (pthread_create(&mt, NULL, migrator, sh) != 0)
        die("pthread_create migrator");

    /* ── Phase orchestration (main thread acts as conductor) ── */

    printf("  [Phase 1] Baseline — %d s, workers remote-accessing node%d ...\n",
           cfg->phase1_s, src);
    timing_reset();
    atomic_store_explicit(&sh->phase, 1, memory_order_release);
    sleep(cfg->phase1_s);

    printf("  [Phase 2] Migration — %d s, chunk=%d pages, "
           "migrator node%d→node%d ...\n",
           cfg->phase2_s, cfg->chunk_pages, src, dst);
    atomic_store_explicit(&sh->phase, 2, memory_order_release);
    sleep(cfg->phase2_s);

    int post_ok = sample_placement(sh->buf, BUF_PAGES, dst, 256);
    printf("  Post-migration: %d/256 sampled pages on node %d (dst)\n",
           post_ok, dst);

    printf("  [Phase 3] Steady state — %d s, workers local-accessing node%d ...\n",
           cfg->phase3_s, dst);
    atomic_store_explicit(&sh->phase, 3, memory_order_release);
    sleep(cfg->phase3_s);

    /* Signal all threads to exit */
    atomic_store_explicit(&sh->phase, 4, memory_order_release);

    for (int i = 0; i < cfg->nthreads; i++) pthread_join(wt[i], NULL);
    pthread_join(mt, NULL);

    /* ── Write output files ── */
    write_worker_csv(sh);
    write_migrator_csv(sh);

    char rbuf_path[256];
    snprintf(rbuf_path, sizeof(rbuf_path),
             "bench_e_timing_x86_%s.csv", sh->prefix);
    timing_read(rbuf_path);
    printf("  Ring buffer   → %s\n", rbuf_path);
    printf("  (ring wraps during %d s Phase 2 — data is a statistical sample)\n",
           cfg->phase2_s);

    print_summary(sh);

    munmap(sh->buf, BUF_SZ);
    free(sh);
}

/* ─────────────────────────────────────────────────────────────────── */
/* Main                                                                */
/* ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* ── Preflight checks ── */
    if (numa_available() < 0) {
        fprintf(stderr,
            "ERROR: NUMA not available. Boot with numa=fake=2\n"
            "  Add to GRUB_CMDLINE_LINUX: numa=fake=2\n");
        return 1;
    }
    if (numa_max_node() < 1) {
        fprintf(stderr,
            "ERROR: Need ≥2 NUMA nodes, found %d. Boot with numa=fake=2\n",
            numa_max_node() + 1);
        return 1;
    }
    if (access(DEBUGFS_PATH, R_OK | W_OK) < 0)
        fprintf(stderr,
            "WARN: %s not accessible — ring buffer output disabled\n"
            "  Try: sudo mount -t debugfs none /sys/kernel/debug\n",
            DEBUGFS_PATH);
    if (getuid() != 0)
        fprintf(stderr,
            "WARN: not root — MPOL_MF_MOVE_ALL and debugfs writes may fail\n"
            "  Run: sudo ./mig_bench_e\n");

    /* ── Discover per-node CPUs from sysfs ── */
    int n0cpus[MAX_NODE_CPUS], n1cpus[MAX_NODE_CPUS];
    int nn0 = parse_cpulist("/sys/devices/system/node/node0/cpulist",
                            n0cpus, MAX_NODE_CPUS);
    int nn1 = parse_cpulist("/sys/devices/system/node/node1/cpulist",
                            n1cpus, MAX_NODE_CPUS);

    /* Fallback: assume CPU 0 = node0, CPU 1 = node1 */
    if (nn0 == 0) { n0cpus[0] = 0; nn0 = 1; }
    if (nn1 == 0) {
        /* Memory-only NUMA node — use node0 CPUs (excl. migrator CPU 0)
         * as workers so Bench E can run with multiple worker threads.
         * Pages still migrate node0-mem → node1-mem (real NUMA hop). */
        fprintf(stderr, "  NOTE: node1 has no CPUs (memory-only NUMA node)\n");
        fprintf(stderr, "  Using node0 CPUs 1,2,3 as worker threads.\n");
        nn1 = 0;
        for (int i = 1; i < nn0 && nn1 < MAX_NODE_CPUS; i++)
            n1cpus[nn1++] = n0cpus[i];
        if (nn1 == 0) { n1cpus[0] = 1; nn1 = 1; } /* absolute fallback */
    }

    /* Calibrate TSC before any timing measurements */
    calibrate_tsc();

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   Benchmark E: Multi-Threaded NUMA Migration         ║\n");
    printf("║   x86_64 / Linux 6.1.4 / 512 MB buf / numa=fake=2   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    printf("  Node 0 CPUs  :");
    for (int i = 0; i < nn0; i++) printf(" %d", n0cpus[i]);
    printf("\n  Node 1 CPUs  :");
    for (int i = 0; i < nn1; i++) printf(" %d", n1cpus[i]);
    printf("\n  Migrator CPU : %d  (node 0)\n", n0cpus[0]);
    printf("  Workers pin  : node 1 CPUs\n\n");

    /*
     * ── Run matrix (Table 1 from design document) ──
     *
     *  pat         op      thr  chunk  p1  p2  p3   label
     */
    bench_cfg_t configs[] = {
        { PAT_RAND,  OP_RMW,  4, 512, 10, 30, 10, "E_rand_rmw_t4_c512"  },
        { PAT_SEQ,   OP_READ, 4, 512, 10, 30, 10, "E_seq_read_t4_c512"  },
        { PAT_RAND,  OP_READ, 4, 512, 10, 30, 10, "E_rand_read_t4_c512" },
        { PAT_RAND,  OP_RMW,  1, 512, 10, 30, 10, "E_rand_rmw_t1_c512"  },
        { PAT_RAND,  OP_RMW,  8, 512, 10, 30, 10, "E_rand_rmw_t8_c512"  },
        { PAT_RAND,  OP_RMW,  4,   1, 10, 30, 10, "E_rand_rmw_t4_c1"    },
        { PAT_RAND,  OP_RMW,  4,  64, 10, 30, 10, "E_rand_rmw_t4_c64"   },
    };
    int nconfigs = (int)(sizeof(configs) / sizeof(configs[0]));

    /* Optional: single-config mode via argv[1] (0-based index) */
    int start = 0, end = nconfigs;
    if (argc >= 2) {
        int idx = atoi(argv[1]);
        if (idx < 0 || idx >= nconfigs) {
            fprintf(stderr,
                "Config index %d out of range [0, %d)\n", idx, nconfigs);
            return 1;
        }
        start = idx;
        end   = idx + 1;
    }

    static const char *pat_s[] = { "seq", "rand", "stride16" };
    static const char *op_s[]  = { "read", "write", "rmw" };

    for (int i = start; i < end; i++) {
        bench_cfg_t *c = &configs[i];

        /* Clamp thread count to available worker CPUs */
        if (c->nthreads > nn1) {
            fprintf(stderr,
                "  NOTE: clamping %d workers to %d (available worker CPUs)\n",
                c->nthreads, nn1);
            c->nthreads = nn1;
        }

        printf("┌──────────────────────────────────────────────────────┐\n");
        printf("│  Config %d/%d: %-38s    │\n", i+1, nconfigs, c->label);
        printf("│  %-8s  %-6s  %d workers  chunk=%d pages            │\n",
               pat_s[c->pat], op_s[c->op], c->nthreads, c->chunk_pages);
        printf("│  Phases: %d s / %d s / %d s                            │\n",
               c->phase1_s, c->phase2_s, c->phase3_s);
        printf("└──────────────────────────────────────────────────────┘\n");

        run_bench(c, /*src*/0, /*dst*/1, n0cpus[0], n1cpus, nn1);
        printf("\n");
    }

    printf("══════════════════════════════════════════════════════\n");
    printf("  All Benchmark E runs complete.\n");
    printf("  Outputs: bench_e_workers_x86_*.csv\n");
    printf("           bench_e_migrator_x86_*.csv\n");
    printf("           bench_e_timing_x86_*.csv  (ring buffer samples)\n");
    printf("  Analyze: python3 analyze_bench_e_x86.py\n");
    printf("══════════════════════════════════════════════════════\n\n");
    return 0;
}

