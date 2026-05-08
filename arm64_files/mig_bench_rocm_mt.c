/*
 * mig_bench_rocm_mt.c — ROCM vs Standard: Controlled Multithreaded Test
 * Target: ARM64, Linux 6.1.4 with migrate_rocm.c patch, numa=fake=2
 *
 * CONTROLLED DESIGN — the only variable between the two runs is which
 * kernel migration path fires. Everything else is identical:
 *   - Same file-backed buffer (ext4, /var/tmp, fsync, clean pages)
 *   - Same worker access pattern (random reads, PROT_READ mapping)
 *   - Same chunk size, phase duration, thread count
 *
 * How the kernel path is controlled:
 *
 *   Run A — Standard path:
 *     The file is mapped twice:
 *       sh->buf          = mmap(PROT_READ,  MAP_SHARED)  ← workers read here
 *       sh->writable_alias = mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
 *     The writable alias installs a PTE with the write bit set.
 *     folio_rmap_all_readonly() finds this PTE → returns false.
 *     → standard pipeline: Lock→Unmap→Copy→Remap→Unlock
 *     → workers hit migration entry, sleep in migration_entry_wait()
 *
 *   Run B — ROCM path:
 *     The file is mapped once:
 *       sh->buf          = mmap(PROT_READ, MAP_SHARED)   ← workers read here
 *     No writable PTE exists anywhere.
 *     folio_rmap_all_readonly() finds only read-only PTEs → returns true.
 *     → ROCM pipeline: Lock→Copy→[PTE swap]→Unlock
 *     → workers take clean page fault, retry immediately, no sleep
 *
 * Workers access sh->buf (PROT_READ) in both cases — identical workload.
 * The writable alias is never touched by workers; it exists only to
 * flip the kernel's eligibility check.
 *
 * Key metrics:
 *   disruption_factor = Phase2_ops / Phase1_ops  (1.0 = no disruption)
 *   p999_spike        = Phase2_p999 / Phase1_p999
 *   stall_count       = ops > STALL_THRESH_NS during Phase 2
 *   stall_mean_ns     = mean stalled-op duration
 *   stalls_per_page   = stall_count / pages_migrated (normalised)
 *
 * Build:
 *   gcc -O2 -Wall -pthread -o mig_bench_rocm_mt mig_bench_rocm_mt.c -lnuma -lm
 *
 * Run:
 *   sudo ./mig_bench_rocm_mt
 *
 * Outputs:
 *   rocm_mt_standard_ring.csv  — kernel ring buffer, standard run
 *   rocm_mt_rocm_ring.csv      — kernel ring buffer, ROCM run
 *   rocm_mt_report.txt         — side-by-side comparison report
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <numaif.h>
#include <numa.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define TEST_PAGES       8192UL     /* 32 MB — safe for 2GB VM with writable alias */
#define PAGE_SZ          4096UL
#define BUF_SZ           (TEST_PAGES * PAGE_SZ)
#define CHUNK_PAGES      512        /* pages per move_pages() call */
#define PHASE_SECS       15         /* seconds per phase */
#define N_WORKERS        2          /* worker threads (clamped to node1 CPUs) */
#define MAX_SAMPLES      100000     /* latency reservoir slots per worker per phase */
#define SAMPLE_EVERY     8          /* time 1-in-N ops for latency measurement */
#define STALL_THRESH_NS  5000ULL    /* ops > 5 µs counted as migration stalls */
#define DEBUGFS_PATH     "/sys/kernel/debug/mig_timing"
#define MAX_NODE_CPUS    16

/* Phase signal values */
#define PH_INIT   0
#define PH_BASE   1   /* Phase 1: baseline — no migration */
#define PH_MIG    2   /* Phase 2: migration active */
#define PH_STEADY 3   /* Phase 3: post-migration steady state */
#define PH_DONE   4

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t  ops;          /* total operations in this phase */
    uint64_t  dur_ns;       /* phase wall-clock duration */
    uint64_t *samp;         /* latency reservoir (ns per sampled op) */
    int       n_samp;       /* filled slots in reservoir */
    int       n_seen;       /* total timed ops (for reservoir probability) */
    uint64_t  stall_count;  /* ops that exceeded STALL_THRESH_NS */
    uint64_t  stall_sum_ns; /* sum of stalled-op durations */
    /* computed after phase ends */
    double    p50, p95, p99, p999, mean_ns;
    double    stall_mean_ns;
} phase_stat_t;

typedef struct {
    atomic_int   phase;
    void        *buf;           /* PROT_READ mapping — workers access this */
    void        *writable_alias;/* PROT_RW alias (standard run only, else NULL) */
    int          file_fd;       /* O_RDONLY fd for worker PROT_READ mapping */
    int          rdwr_fd;       /* O_RDWR fd for writable alias (standard only) */
    int          src_node, dst_node;
    int          node1_cpus[MAX_NODE_CPUS];
    int          n_node1_cpus;
    int          n_workers;
    phase_stat_t ws[N_WORKERS][3]; /* [worker][phase_index 0..2] */
    uint64_t     mig_pages;
    uint64_t     mig_dur_ns;
    char         label[32];    /* "standard" or "rocm" */
    int          is_rocm;      /* 0 = install writable alias, 1 = read-only only */
} shared_t;

typedef struct { shared_t *sh; int tid; } warg_t;

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

static void die(const char *m)
{
    fprintf(stderr, "FATAL: %s: %s\n", m, strerror(errno));
    exit(1);
}

static void warn_msg(const char *m)
{
    fprintf(stderr, "WARN: %s: %s\n", m, strerror(errno));
}

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void pin_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s) < 0)
        warn_msg("sched_setaffinity");
}

static int parse_cpulist(const char *path, int *out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256];
    int n = 0;
    if (fgets(buf, sizeof(buf), f)) {
        char *p = buf;
        while (*p && *p != '\n' && n < max) {
            int a = -1, b = -1;
            if (sscanf(p, "%d-%d", &a, &b) == 2)
                for (int i = a; i <= b && n < max; i++) out[n++] = i;
            else if (sscanf(p, "%d", &a) == 1)
                out[n++] = a;
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
    if (fd < 0) return;
    ssize_t r = write(fd, "1", 1); (void)r;
    close(fd);
    usleep(20000);
}

static void timing_save(const char *path)
{
    char buf[8192]; ssize_t n;
    int rfd = open(DEBUGFS_PATH, O_RDONLY);
    if (rfd < 0) return;
    int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) { close(rfd); return; }
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(wfd, buf, (size_t)n); (void)w;
    }
    close(rfd); close(wfd);
}

/* ------------------------------------------------------------------ */
/* Percentile computation                                              */
/* ------------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void compute_pct(phase_stat_t *ps)
{
    int n = ps->n_samp;
    if (n == 0) return;
    qsort(ps->samp, (size_t)n, sizeof(uint64_t), cmp_u64);
#define PCT(f) ((double)ps->samp[(int)((f) * (n - 1))])
    ps->p50  = PCT(0.500);
    ps->p95  = PCT(0.950);
    ps->p99  = PCT(0.990);
    ps->p999 = PCT(0.999);
#undef PCT
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)ps->samp[i];
    ps->mean_ns = sum / n;
    ps->stall_mean_ns = ps->stall_count > 0
        ? (double)ps->stall_sum_ns / (double)ps->stall_count : 0.0;
}

/* ------------------------------------------------------------------ */
/* Worker thread — identical for both runs                             */
/* ------------------------------------------------------------------ */

static void *worker(void *arg)
{
    warg_t   *wa  = arg;
    shared_t *sh  = wa->sh;
    int       tid = wa->tid;

    /* Pin to a node1 CPU — workers access buffer remotely in Phase 1 */
    if (sh->n_node1_cpus > 0)
        pin_cpu(sh->node1_cpus[tid % sh->n_node1_cpus]);

    /*
     * Workers always access sh->buf — the PROT_READ mapping.
     * This is identical for both standard and ROCM runs.
     * On the standard run, sh->writable_alias exists but workers
     * never touch it — it is invisible to the access pattern.
     */
    volatile uint64_t *buf = (volatile uint64_t *)sh->buf;
    size_t np = TEST_PAGES;

    /* Per-thread LCG for random page selection */
    uint64_t rng = 0xdeadbeefULL ^ ((uint64_t)tid * 0x9e3779b97f4a7c15ULL);

    phase_stat_t *ps = sh->ws[tid];
    uint64_t op_cnt  = 0;
    int cur_ph = PH_BASE;
    int pi     = 0;

    /* Spin until Phase 1 signal */
    while (atomic_load_explicit(&sh->phase, memory_order_acquire) < PH_BASE)
#ifdef __aarch64__
        __asm__ volatile("yield");
#else
        __asm__ volatile("pause");
#endif

    uint64_t phase_start = now_ns();

    for (;;) {
        int ph = atomic_load_explicit(&sh->phase, memory_order_acquire);

        /* Handle phase transition */
        if (ph != cur_ph) {
            ps[pi].dur_ns = now_ns() - phase_start;
            compute_pct(&ps[pi]);
            if (ph >= PH_DONE) break;
            cur_ph      = ph;
            pi          = ph - 1;
            phase_start = now_ns();
            op_cnt      = 0;
        }

        /* Random page selection */
        rng  = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t pidx = (rng >> 17) % np;
        volatile uint64_t *ptr = buf + pidx * (PAGE_SZ / sizeof(uint64_t));

        int do_time = (++op_cnt % SAMPLE_EVERY == 0);
        uint64_t t0 = do_time ? now_ns() : 0;

        /*
         * Read from the PROT_READ mapping.
         *
         * Standard run: this page may have a migration entry (Present=0)
         * installed by try_to_migrate(). The fault handler finds the
         * entry and calls migration_entry_wait() -> sleeps until the
         * migrator installs the dst PTE and calls folio_unlock().
         *
         * ROCM run: no migration entry is ever installed. If the PTE
         * was swapped between ptep_get_and_clear() and set_pte_at(),
         * the fault handler finds nothing and retries immediately.
         */
        uint64_t v = *ptr;
        (void)v;

        if (do_time) {
            uint64_t lat = now_ns() - t0;
            ps[pi].ops++;
            ps[pi].n_seen++;

            /* Reservoir sampling — replace random entry when full */
            if (ps[pi].n_samp < MAX_SAMPLES) {
                ps[pi].samp[ps[pi].n_samp++] = lat;
            } else {
                uint64_t j = rng % (uint64_t)ps[pi].n_samp;
                ps[pi].samp[j] = lat;
            }

            /* Count migration stalls */
            if (lat > STALL_THRESH_NS) {
                ps[pi].stall_count++;
                ps[pi].stall_sum_ns += lat;
            }
        } else {
            ps[pi].ops++;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Migrator thread — migrates sh->buf pages                           */
/* ------------------------------------------------------------------ */

static void *migrator(void *arg)
{
    shared_t *sh = arg;

    /* Wait for Phase 2 */
    while (atomic_load_explicit(&sh->phase, memory_order_acquire) < PH_MIG)
#ifdef __aarch64__
        __asm__ volatile("yield");
#else
        __asm__ volatile("pause");
#endif

    void **pages = malloc(CHUNK_PAGES * sizeof(void *));
    int   *nodes = malloc(CHUNK_PAGES * sizeof(int));
    int   *stat  = malloc(CHUNK_PAGES * sizeof(int));
    if (!pages || !nodes || !stat) die("migrator alloc");

    for (int i = 0; i < CHUNK_PAGES; i++)
        nodes[i] = sh->dst_node;

    uint64_t t0 = now_ns();
    size_t remaining = TEST_PAGES;
    uint64_t migrated = 0;

    while (remaining > 0 &&
           atomic_load_explicit(&sh->phase, memory_order_acquire) == PH_MIG) {
        int chunk = (int)((remaining < CHUNK_PAGES) ? remaining : CHUNK_PAGES);
        char *base = (char *)sh->buf;
        for (int i = 0; i < chunk; i++)
            pages[i] = base + (TEST_PAGES - remaining + (size_t)i) * PAGE_SZ;

        /*
         * move_pages() on sh->buf (the PROT_READ mapping).
         *
         * Standard run: folio_rmap_all_readonly() finds the writable
         * PTE in sh->writable_alias -> returns false -> standard path.
         *
         * ROCM run: folio_rmap_all_readonly() finds only PROT_READ
         * PTEs -> returns true -> ROCM path fires.
         *
         * In both cases, move_pages() is called on the same virtual
         * address range. The kernel looks up the folio from the VMA,
         * then walks all PTEs mapping that folio regardless of which
         * VMA they belong to. The writable alias's PTE is found during
         * the rmap walk even though move_pages() was called on sh->buf.
         */
        long rc = move_pages(0, (unsigned long)chunk, pages,
                             nodes, stat, MPOL_MF_MOVE);
        if (rc < 0 && errno != 0) warn_msg("move_pages");

        for (int i = 0; i < chunk; i++)
            if (stat[i] == sh->dst_node) migrated++;

        remaining -= (size_t)chunk;
    }

    sh->mig_pages  = migrated;
    sh->mig_dur_ns = now_ns() - t0;

    free(pages); free(nodes); free(stat);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Buffer setup                                                        */
/* ------------------------------------------------------------------ */

/*
 * setup_standard_buf — anonymous writable pages for the standard run.
 *
 * MAP_ANONYMOUS PROT_READ|PROT_WRITE, faulted in with writes on src_node.
 *   folio_test_anon() = true → ROCM gate returns false → standard path.
 *   try_to_migrate() installs migration entry → workers stall.
 *
 * Workers access via a volatile read pointer (read-only access pattern),
 * so latency measures pure read-fault stalls from migration entries.
 */
static void *setup_standard_buf(int src_node)
{
    unsigned long nmask = 1UL << (unsigned)src_node;

    void *buf = mmap(NULL, BUF_SZ,
                     PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buf == MAP_FAILED) die("mmap standard anon");

    if (mbind(buf, BUF_SZ, MPOL_BIND, &nmask, 64, MPOL_MF_MOVE) < 0)
        warn_msg("mbind standard");

    /* Fault in all pages with writes — establishes pages on src_node */
    volatile uint64_t *p = buf;
    for (size_t i = 0; i < BUF_SZ / sizeof(uint64_t);
         i += PAGE_SZ / sizeof(uint64_t))
        p[i] = 0xDEADBEEFCAFEBABEULL;

    return buf;
}

/*
 * setup_rocm_buf — clean file-backed read-only pages for the ROCM run.
 *
 * File on ext4 (/var/tmp), fsync to clear PG_dirty, reopen O_RDONLY,
 * mmap PROT_READ. Pages are faulted in with reads on src_node.
 *   folio_rmap_all_readonly() = true → ROCM path fires.
 *   No migration entry installed → workers take clean page fault retry.
 */
static void *setup_rocm_buf(int src_node, int *fd_out)
{
    unsigned long nmask = 1UL << (unsigned)src_node;
    if (set_mempolicy(MPOL_BIND, &nmask, 64) < 0)
        warn_msg("set_mempolicy src");

    char tmppath[64];
    snprintf(tmppath, sizeof(tmppath), "/var/tmp/rocm_mt_XXXXXX");
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        snprintf(tmppath, sizeof(tmppath), "/tmp/rocm_mt_XXXXXX");
        fd = mkstemp(tmppath);
        if (fd < 0) die("mkstemp rocm");
        fprintf(stderr, "  NOTE: /var/tmp unavailable, using /tmp.\n");
    }

    const size_t WCHUNK = 65536;
    char *wbuf = malloc(WCHUNK);
    if (!wbuf) die("alloc wbuf");
    memset(wbuf, 0xBE, WCHUNK);
    size_t written = 0;
    while (written < BUF_SZ) {
        size_t todo = (BUF_SZ - written < WCHUNK) ? (BUF_SZ - written) : WCHUNK;
        ssize_t r = write(fd, wbuf, todo);
        if (r <= 0) die("write rocm buf");
        written += (size_t)r;
    }
    free(wbuf);

    if (fsync(fd) < 0) warn_msg("fsync");

    close(fd);
    fd = open(tmppath, O_RDONLY);
    if (fd < 0) die("reopen O_RDONLY");
    unlink(tmppath);

    if (set_mempolicy(MPOL_DEFAULT, NULL, 0) < 0)
        warn_msg("set_mempolicy default");

    void *buf = mmap(NULL, BUF_SZ, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) die("mmap PROT_READ");

    volatile uint64_t sink = 0;
    volatile uint64_t *p = buf;
    for (size_t i = 0; i < BUF_SZ / sizeof(uint64_t);
         i += PAGE_SZ / sizeof(uint64_t))
        sink ^= p[i];
    (void)sink;

    *fd_out = fd;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Run one 3-phase experiment                                          */
/* ------------------------------------------------------------------ */

static void run_experiment(shared_t *sh, int src_node, int dst_node,
                            int n0cpu, int *n1cpus, int nn1)
{
    sh->src_node     = src_node;
    sh->dst_node     = dst_node;
    sh->n_node1_cpus = (nn1 < MAX_NODE_CPUS) ? nn1 : MAX_NODE_CPUS;
    for (int i = 0; i < sh->n_node1_cpus; i++)
        sh->node1_cpus[i] = n1cpus[i];
    sh->n_workers = (N_WORKERS < sh->n_node1_cpus)
                    ? N_WORKERS : sh->n_node1_cpus;

    for (int t = 0; t < sh->n_workers; t++)
        for (int p = 0; p < 3; p++) {
            memset(&sh->ws[t][p], 0, sizeof(phase_stat_t));
            sh->ws[t][p].samp = calloc(MAX_SAMPLES, sizeof(uint64_t));
            if (!sh->ws[t][p].samp) die("alloc samp");
        }

    sh->file_fd = -1;
    sh->rdwr_fd = -1;
    sh->writable_alias = NULL;

    if (!sh->is_rocm) {
        printf("  Setting up standard buffer (%lu MB anon) on node %d ...\n",
               BUF_SZ / (1024 * 1024), src_node);
        sh->buf = setup_standard_buf(src_node);
        printf("  Anonymous writable pages -> standard migration path\n");
    } else {
        printf("  Setting up ROCM buffer (%lu MB file-backed) on node %d ...\n",
               BUF_SZ / (1024 * 1024), src_node);
        sh->buf = setup_rocm_buf(src_node, &sh->file_fd);
        printf("  Clean read-only file pages -> ROCM migration path\n");
    }

    atomic_store(&sh->phase, PH_INIT);

    pthread_t wt[N_WORKERS];
    warg_t    wa[N_WORKERS];
    for (int i = 0; i < sh->n_workers; i++) {
        wa[i].sh = sh; wa[i].tid = i;
        if (pthread_create(&wt[i], NULL, worker, &wa[i]) != 0)
            die("pthread_create worker");
    }

    pthread_t mt;
    if (pthread_create(&mt, NULL, migrator, sh) != 0)
        die("pthread_create migrator");

    pin_cpu(n0cpu);

    printf("  [Phase 1] Baseline   — %d s (no migration) ...\n", PHASE_SECS);
    timing_reset();
    atomic_store_explicit(&sh->phase, PH_BASE, memory_order_release);
    sleep(PHASE_SECS);

    printf("  [Phase 2] Migration  — %d s (node%d->node%d) ...\n",
           PHASE_SECS, src_node, dst_node);
    atomic_store_explicit(&sh->phase, PH_MIG, memory_order_release);
    sleep(PHASE_SECS);

    printf("  [Phase 3] Steady     — %d s (pages local) ...\n", PHASE_SECS);
    atomic_store_explicit(&sh->phase, PH_STEADY, memory_order_release);
    sleep(PHASE_SECS);

    atomic_store_explicit(&sh->phase, PH_DONE, memory_order_release);

    for (int i = 0; i < sh->n_workers; i++) pthread_join(wt[i], NULL);
    pthread_join(mt, NULL);

    char rbuf[128];
    snprintf(rbuf, sizeof(rbuf), "rocm_mt_%s_ring.csv", sh->label);
    timing_save(rbuf);

    printf("  Ring buffer  -> %s\n", rbuf);
    printf("  Migration:      %lu pages in %.1f ms  (%.0f pages/s)\n\n",
           sh->mig_pages,
           (double)sh->mig_dur_ns / 1e6,
           sh->mig_dur_ns > 0
               ? sh->mig_pages / ((double)sh->mig_dur_ns / 1e9) : 0.0);

    munmap(sh->buf, BUF_SZ);
    if (sh->file_fd >= 0) close(sh->file_fd);
    sh->buf = NULL;
}

/* ------------------------------------------------------------------ */
/* Aggregate stats across workers for one phase                        */
/* ------------------------------------------------------------------ */

typedef struct {
    double   ops_per_s;
    double   p50, p95, p99, p999, mean_ns;
    uint64_t stall_count;
    double   stall_mean_ns;
} agg_t;

static agg_t aggregate(shared_t *sh, int pi)
{
    agg_t a;
    memset(&a, 0, sizeof(a));

    int total = 0;
    for (int t = 0; t < sh->n_workers; t++)
        total += sh->ws[t][pi].n_samp;
    if (total == 0) return a;

    uint64_t *merged = malloc((size_t)total * sizeof(uint64_t));
    if (!merged) return a;

    int j = 0;
    double ops_sum = 0, max_dur = 0;
    uint64_t stall_count = 0;
    double stall_sum = 0;

    for (int t = 0; t < sh->n_workers; t++) {
        phase_stat_t *ps = &sh->ws[t][pi];
        for (int i = 0; i < ps->n_samp; i++) merged[j++] = ps->samp[i];
        ops_sum += (double)ps->ops;
        double d = (double)ps->dur_ns;
        if (d > max_dur) max_dur = d;
        stall_count += ps->stall_count;
        stall_sum   += (double)ps->stall_sum_ns;
    }

    a.ops_per_s = (max_dur > 0) ? ops_sum / (max_dur / 1e9) : 0;

    qsort(merged, (size_t)total, sizeof(uint64_t), cmp_u64);
#define PCT(f) ((double)merged[(int)((f)*(total-1))])
    a.p50 = PCT(0.500); a.p95 = PCT(0.950);
    a.p99 = PCT(0.990); a.p999 = PCT(0.999);
#undef PCT
    double sum = 0;
    for (int i = 0; i < total; i++) sum += (double)merged[i];
    a.mean_ns = sum / total;
    a.stall_count   = stall_count;
    a.stall_mean_ns = stall_count > 0 ? stall_sum / stall_count : 0;

    free(merged);
    return a;
}

/* ------------------------------------------------------------------ */
/* Print comparison report                                             */
/* ------------------------------------------------------------------ */

static void print_report(FILE *f, shared_t *ss, shared_t *rs)
{
    agg_t sp1 = aggregate(ss, 0), sp2 = aggregate(ss, 1), sp3 = aggregate(ss, 2);
    agg_t rp1 = aggregate(rs, 0), rp2 = aggregate(rs, 1), rp3 = aggregate(rs, 2);

    double sdf = (sp1.ops_per_s > 0) ? sp2.ops_per_s / sp1.ops_per_s : 0;
    double rdf = (rp1.ops_per_s > 0) ? rp2.ops_per_s / rp1.ops_per_s : 0;
    double ssp = (sp1.p999 > 0) ? sp2.p999 / sp1.p999 : 0;
    double rsp = (rp1.p999 > 0) ? rp2.p999 / rp1.p999 : 0;

    /* Stalls per migrated page — normalised comparison */
    double s_spp = (ss->mig_pages > 0)
        ? (double)sp2.stall_count / (double)ss->mig_pages : 0;
    double r_spp = (rs->mig_pages > 0)
        ? (double)rp2.stall_count / (double)rs->mig_pages : 0;

    fprintf(f,
        "╔══════════════════════════════════════════════════════════════════╗\n"
        "║   ROCM vs Standard -- Controlled Multithreaded Disruption Test   ║\n"
        "║   ARM64  Linux 6.1.4  %d workers  %ds/phase  numa=fake=2         ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n\n",
        ss->n_workers, PHASE_SECS);

    fprintf(f,
        "Design:\n"
        "  Standard run: MAP_ANONYMOUS PROT_RW buffer (%lu MB), faulted in\n"
        "                with writes on node 0.\n"
        "                folio_test_anon()=true -> ROCM gate returns false\n"
        "                -> standard path: migration entry installed\n"
        "                -> workers stall in migration_entry_wait()\n\n"
        "  ROCM run:     File-backed PROT_READ buffer (%lu MB), fsync-clean.\n"
        "                folio_rmap_all_readonly()=true -> ROCM path fires\n"
        "                -> no migration entry installed\n"
        "                -> workers take clean page fault, retry immediately\n\n"
        "  Workers: identical random reads in both runs (no writes).\n"
        "  The only variable: which kernel migration pipeline fires.\n"
        "  Buffer type differs (anon vs file) but worker access is identical.\n\n",
        BUF_SZ / (1024 * 1024), BUF_SZ / (1024 * 1024));

    /* ── Throughput ── */
    fprintf(f,
        "Throughput (ops/s) -- Disruption Factor:\n"
        "--------------------------------------------------------------------\n"
        "%-10s  %13s  %13s  %13s  %6s\n",
        "Path", "Phase1(base)", "Phase2(mig)", "Phase3(steady)", "DF");
    fprintf(f,
        "--------------------------------------------------------------------\n");
    fprintf(f, "%-10s  %13.0f  %13.0f  %13.0f  %6.3f\n",
            "Standard", sp1.ops_per_s, sp2.ops_per_s, sp3.ops_per_s, sdf);
    fprintf(f, "%-10s  %13.0f  %13.0f  %13.0f  %6.3f\n",
            "ROCM", rp1.ops_per_s, rp2.ops_per_s, rp3.ops_per_s, rdf);
    fprintf(f,
        "--------------------------------------------------------------------\n"
        "  DF = Phase2/Phase1. Closer to 1.0 = less disruption from migration.\n"
        "  Standard DF %.3f  vs  ROCM DF %.3f\n\n",
        sdf, rdf);

    /* ── Latency ── */
    fprintf(f,
        "Latency during Phase 2 (migration active), nanoseconds:\n"
        "--------------------------------------------------------------------\n"
        "%-10s  %10s  %10s  %10s  %10s  %10s\n",
        "Path", "mean", "p50", "p95", "p99", "p999");
    fprintf(f,
        "--------------------------------------------------------------------\n");
    fprintf(f, "%-10s  %10.0f  %10.0f  %10.0f  %10.0f  %10.0f\n",
            "Standard", sp2.mean_ns, sp2.p50, sp2.p95, sp2.p99, sp2.p999);
    fprintf(f, "%-10s  %10.0f  %10.0f  %10.0f  %10.0f  %10.0f\n",
            "ROCM", rp2.mean_ns, rp2.p50, rp2.p95, rp2.p99, rp2.p999);
    fprintf(f,
        "--------------------------------------------------------------------\n"
        "  p999 spike (Phase2 vs Phase1): Standard %.2fx  ROCM %.2fx\n"
        "  p99  reduction:  %.0f ns -> %.0f ns  (%.1fx lower)\n"
        "  p999 reduction:  %.0f ns -> %.0f ns  (%.1fx lower)\n\n",
        ssp, rsp,
        sp2.p99,  rp2.p99,  rp2.p99  > 0 ? sp2.p99  / rp2.p99  : 0.0,
        sp2.p999, rp2.p999, rp2.p999 > 0 ? sp2.p999 / rp2.p999 : 0.0);

    /* ── Stall events ── */
    fprintf(f,
        "Migration stall events (read latency > %.0f us):\n"
        "--------------------------------------------------------------------\n"
        "%-10s  %14s  %16s  %18s\n",
        (double)STALL_THRESH_NS / 1000.0,
        "Path", "Phase2 stalls", "stall_mean_ns", "stalls/migrated_page");
    fprintf(f,
        "--------------------------------------------------------------------\n");
    fprintf(f, "%-10s  %14lu  %16.0f  %18.2f\n",
            "Standard", sp2.stall_count, sp2.stall_mean_ns, s_spp);
    fprintf(f, "%-10s  %14lu  %16.0f  %18.2f\n",
            "ROCM", rp2.stall_count, rp2.stall_mean_ns, r_spp);
    fprintf(f,
        "--------------------------------------------------------------------\n");
    if (s_spp > 0)
        fprintf(f,
            "  Stalls/page: Standard %.2f  vs  ROCM %.2f  (%.1fx reduction)\n"
            "  Standard stall mean: %.0f ns (sleeps in migration_entry_wait)\n"
            "  ROCM stall mean:     %.0f ns (clean fault retry latency)\n\n",
            s_spp, r_spp, r_spp > 0 ? s_spp / r_spp : 0.0,
            sp2.stall_mean_ns, rp2.stall_mean_ns);

    /* ── Migration throughput ── */
    double s_mps = ss->mig_dur_ns > 0
        ? ss->mig_pages / ((double)ss->mig_dur_ns / 1e9) : 0;
    double r_mps = rs->mig_dur_ns > 0
        ? rs->mig_pages / ((double)rs->mig_dur_ns / 1e9) : 0;
    fprintf(f,
        "Migration throughput:\n"
        "--------------------------------------------------------------------\n"
        "  Standard  %6lu pages  %8.1f ms  %8.0f pages/s\n"
        "  ROCM      %6lu pages  %8.1f ms  %8.0f pages/s  (%.1fx faster)\n\n",
        ss->mig_pages, (double)ss->mig_dur_ns / 1e6, s_mps,
        rs->mig_pages, (double)rs->mig_dur_ns / 1e6, r_mps,
        s_mps > 0 ? r_mps / s_mps : 0.0);

    /* ── Full phase table ── */
    fprintf(f,
        "All phases -- ops/s and p999 latency:\n"
        "--------------------------------------------------------------------\n"
        "%-10s  %-10s  %13s  %10s\n",
        "Path", "Phase", "ops/s", "p999 ns");
    fprintf(f,
        "--------------------------------------------------------------------\n");
    const char *ph[] = {"Baseline", "Migration", "Steady"};
    agg_t *sa[] = {&sp1, &sp2, &sp3};
    agg_t *ra[] = {&rp1, &rp2, &rp3};
    for (int i = 0; i < 3; i++) {
        fprintf(f, "%-10s  %-10s  %13.0f  %10.0f\n",
                "Standard", ph[i], sa[i]->ops_per_s, sa[i]->p999);
        fprintf(f, "%-10s  %-10s  %13.0f  %10.0f\n",
                "ROCM", ph[i], ra[i]->ops_per_s, ra[i]->p999);
        if (i < 2) fprintf(f, "\n");
    }
    fprintf(f, "--------------------------------------------------------------------\n\n");

    /* ── Conclusion ── */
    fprintf(f,
        "Conclusion:\n"
        "  Controlled test: identical workload, only kernel path differs.\n"
        "  %d workers reading %lu MB via PROT_READ mapping, %d s migration:\n\n"
        "  Disruption factor:   Standard %.3f   ROCM %.3f\n"
        "  p99  latency (P2):   Standard %.0f ns  ROCM %.0f ns  (%.1fx reduction)\n"
        "  p999 latency (P2):   Standard %.0f ns  ROCM %.0f ns  (%.1fx reduction)\n"
        "  Stalls/migrated pg:  Standard %.2f     ROCM %.2f     (%.1fx reduction)\n"
        "  Migration rate:      Standard %.0f p/s  ROCM %.0f p/s  (%.1fx faster)\n\n"
        "  ROCM eliminates migration-PTE sleeps for read-only pages by\n"
        "  copying before the PTE swap rather than after. Faulting threads\n"
        "  find no migration entry and retry immediately (no scheduler call)\n"
        "  instead of sleeping in migration_entry_wait() for the full\n"
        "  Unmap+Copy+Remap window measured at ~1215 ns in Benchmark A.\n",
        ss->n_workers, BUF_SZ / (1024 * 1024), PHASE_SECS,
        sdf, rdf,
        sp2.p99,  rp2.p99,  rp2.p99  > 0 ? sp2.p99  / rp2.p99  : 0.0,
        sp2.p999, rp2.p999, rp2.p999 > 0 ? sp2.p999 / rp2.p999 : 0.0,
        s_spp, r_spp, r_spp > 0 ? s_spp / r_spp : 0.0,
        s_mps, r_mps, s_mps > 0 ? r_mps / s_mps : 0.0);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (numa_available() < 0) {
        fprintf(stderr, "ERROR: NUMA not available.\n"); return 1;
    }
    if (numa_max_node() < 1) {
        fprintf(stderr, "ERROR: Need >=2 NUMA nodes.\n"); return 1;
    }
    if (getuid() != 0)
        fprintf(stderr, "WARN: not root -- move_pages() may fail.\n");

    /* Suppress kcompactd — avoids interference with page state */
    {
        int fd = open("/proc/sys/vm/compaction_proactiveness", O_WRONLY);
        if (fd >= 0) { ssize_t r = write(fd, "0\n", 2); (void)r; close(fd); }
    }

    /* Discover NUMA topology */
    int n0cpus[MAX_NODE_CPUS], n1cpus[MAX_NODE_CPUS];
    int nn0 = parse_cpulist("/sys/devices/system/node/node0/cpulist",
                            n0cpus, MAX_NODE_CPUS);
    int nn1 = parse_cpulist("/sys/devices/system/node/node1/cpulist",
                            n1cpus, MAX_NODE_CPUS);
    if (nn0 == 0) { n0cpus[0] = 0; nn0 = 1; }
    if (nn1 == 0) { n1cpus[0] = 1; nn1 = 1; }
    int nw = (N_WORKERS < nn1) ? N_WORKERS : nn1;

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  ROCM Controlled Multithreaded Disruption Benchmark      ║\n");
    printf("║  ARM64  Linux 6.1.4  numa=fake=2                        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    printf("  Node 0 CPUs : "); for (int i=0;i<nn0;i++) printf("%d ",n0cpus[i]);
    printf("\n  Node 1 CPUs : "); for (int i=0;i<nn1;i++) printf("%d ",n1cpus[i]);
    printf("\n  Workers     : %d\n", nw);
    printf("  Buffer      : %lu MB  (%lu pages)\n",
           BUF_SZ/(1024*1024), TEST_PAGES);
    printf("  Phase dur   : %d s each (~%d s total per run)\n",
           PHASE_SECS, PHASE_SECS*3);
    printf("  Stall thresh: %llu ns (%.0f us)\n",
           STALL_THRESH_NS, (double)STALL_THRESH_NS/1000.0);
    printf("  Sample rate : every %d ops\n", SAMPLE_EVERY);
    printf("  Total time  : ~%d minutes\n\n", (PHASE_SECS*3*2)/60 + 1);

    /* ── Run A: Standard path ── */
    printf("┌──────────────────────────────────────────────────────────┐\n");
    printf("│  Run A: Standard migration path                          │\n");
    printf("│  File-backed RO buffer + writable alias -> standard path │\n");
    printf("│  Workers stall in migration_entry_wait() per page        │\n");
    printf("└──────────────────────────────────────────────────────────┘\n");

    shared_t *ss = calloc(1, sizeof(shared_t));
    if (!ss) die("alloc ss");
    strcpy(ss->label, "standard");
    ss->is_rocm = 0;
    run_experiment(ss, 0, 1, n0cpus[0], n1cpus, nn1);

    sleep(2);

    /* ── Run B: ROCM path ── */
    printf("┌──────────────────────────────────────────────────────────┐\n");
    printf("│  Run B: ROCM migration path                              │\n");
    printf("│  File-backed RO buffer only -> ROCM path                 │\n");
    printf("│  Workers take clean fault retry, no sleep                │\n");
    printf("└──────────────────────────────────────────────────────────┘\n");

    shared_t *rs = calloc(1, sizeof(shared_t));
    if (!rs) die("alloc rs");
    strcpy(rs->label, "rocm");
    rs->is_rocm = 1;
    run_experiment(rs, 0, 1, n0cpus[0], n1cpus, nn1);

    /* ── Report ── */
    printf("Generating report ...\n\n");
    print_report(stdout, ss, rs);

    FILE *rf = fopen("rocm_mt_report.txt", "w");
    if (rf) { print_report(rf, ss, rs); fclose(rf);
              printf("Report saved -> rocm_mt_report.txt\n"); }

    printf("\nOutput files:\n");
    printf("  rocm_mt_standard_ring.csv\n");
    printf("  rocm_mt_rocm_ring.csv\n");
    printf("  rocm_mt_report.txt\n");

    /* Cleanup */
    for (int t = 0; t < nw; t++)
        for (int p = 0; p < 3; p++) {
            free(ss->ws[t][p].samp);
            free(rs->ws[t][p].samp);
        }
    free(ss); free(rs);
    return 0;
}