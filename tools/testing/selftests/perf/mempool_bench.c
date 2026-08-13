/*
 * Benchmark: the per-flow page-run pool (<mem/mempool.h>) against the libc
 * heap it replaced, over the buffer pattern the h2/h3 flow layer actually
 * runs - the HPACK/QPACK tables and the frame/block/tx reassembly buffers,
 * each allocated, grown by doubling, and freed at connection end.
 *
 * Three things are measured, because the pool wins in three different ways and
 * loses in one, and an honest number says which is which:
 *
 *   A. throughput   - CPU time for a stream of connection lifecycles. This is
 *                     where the pool can LOSE: a bitmap next-fit scan is more
 *                     work than a size-class free list, and a sub-page buffer
 *                     costs a whole page.
 *   B. grow copies  - bytes memcpy'd because a grow could not extend in place.
 *                     The pool extends in place whenever the grains after a run
 *                     are free; realloc() moves whenever the heap cannot.
 *   C. resident set - RSS after a burst of connections has come and gone. The
 *                     heap keeps its high-water mark; the pool hands idle pages
 *                     back with madvise(MADV_DONTNEED) in mempool_gc().
 *
 * Timing is CLOCK_MONOTONIC; RSS is read from /proc/self/statm. Build against
 * -DCONFIG_MEASURE=1 to read the pool's own move/reclaim counters.
 */

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <mem/mempool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

static inline u64
ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* resident set size in bytes, from /proc/self/statm (field 2 = RSS pages) */
static size_t
rss_bytes(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	unsigned long total = 0, res = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%lu %lu", &total, &res) != 2)
		res = 0;
	fclose(f);
	return (size_t)res * (size_t)sysconf(_SC_PAGESIZE);
}

static u64 rng;
static inline u32
xrand(void)
{
	u64 x = rng;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	rng = x;
	return (u32)((x * 2685821657736338717ull) >> 33);
}

/*
 * One endpoint's four buffers and the size trajectory each walks. A buffer is
 * described by its first size and the doublings it takes before the connection
 * ends; the driver replays exactly this against either allocator.
 */
struct buf {
	void *p;
	size_t size;
};

struct plan {
	size_t start;
	int    grows;   /* number of ->size doublings over the connection */
};

/* A light connection: small header sets, tables that never fill. */
static const struct plan light[] = {
	{ 4096, 0 },    /* hpack/qpack table: opens at 4K, never grows */
	{ 1024, 1 },    /* part: 1K -> 2K */
	{ 4096, 0 },    /* block: one small header block */
	{ 4096, 0 },    /* tx */
};

/* A heavy connection: big cookies, a header block near the ceiling. */
static const struct plan heavy[] = {
	{ 4096, 4 },    /* table: 4K -> 64K as the encoder fills it */
	{ 1024, 4 },    /* part: 1K -> 16K */
	{ 4096, 6 },    /* block: 4K -> 256K */
	{ 4096, 4 },    /* tx: 4K -> 64K */
};

#define NBUF 4
#define HEAVY_PCT 10    /* 1 in 10 connections is heavy */

/* ---- the pool driver ------------------------------------------------------ */

static u64
run_pool(struct mempool *pool, unsigned conns, u64 *moved_bytes)
{
	struct buf b[NBUF];
	u64 t0, t1, moved = 0;
	unsigned c;
	int i, g;

	t0 = ns_now();
	for (c = 0; c < conns; c++) {
		const struct plan *pl = (xrand() % 100u) < HEAVY_PCT
		                      ? heavy : light;
		int left[NBUF], any;

		/* all four buffers exist before any of them grows */
		for (i = 0; i < NBUF; i++) {
			b[i].size = pl[i].start;
			b[i].p = mempool_alloc(pool, b[i].size);
			left[i] = pl[i].grows;
		}
		/* then they grow interleaved, a doubling each per round, the
		 * way frames arriving on a live connection grow them - so a
		 * grow contends with its neighbours for the grains behind it */
		do {
			any = 0;
			for (i = 0; i < NBUF; i++) {
				size_t ns;
				void *np;

				if (left[i] <= 0)
					continue;
				left[i]--;
				any = 1;
				ns = b[i].size * 2;
				np = mempool_grow(pool, b[i].p, b[i].size, ns);
				if (np != b[i].p)
					moved += b[i].size;
				b[i].p = np;
				b[i].size = ns;
			}
		} while (any);
		for (i = 0; i < NBUF; i++)
			mempool_free(pool, b[i].p, b[i].size);
	}
	t1 = ns_now();
	*moved_bytes = moved;
	(void)g;
	return t1 - t0;
}

/* ---- the libc-heap driver (what the code did before) ---------------------- */

static u64
run_heap(unsigned conns, u64 *moved_bytes)
{
	struct buf b[NBUF];
	u64 t0, t1, moved = 0;
	unsigned c;
	int i, g;

	t0 = ns_now();
	for (c = 0; c < conns; c++) {
		const struct plan *pl = (xrand() % 100u) < HEAVY_PCT
		                      ? heavy : light;
		int left[NBUF], any;

		for (i = 0; i < NBUF; i++) {
			b[i].size = pl[i].start;
			b[i].p = malloc(b[i].size);
			left[i] = pl[i].grows;
		}
		do {
			any = 0;
			for (i = 0; i < NBUF; i++) {
				size_t ns;
				void *np;

				if (left[i] <= 0)
					continue;
				left[i]--;
				any = 1;
				ns = b[i].size * 2;
				np = realloc(b[i].p, ns);
				/* a realloc that returned a new address copied
				 * the old bytes - the move the pool avoids when
				 * the grains behind the run are free */
				if (np != b[i].p)
					moved += b[i].size;
				b[i].p = np;
				b[i].size = ns;
			}
		} while (any);
		for (i = 0; i < NBUF; i++)
			free(b[i].p);
	}
	t1 = ns_now();
	*moved_bytes = moved;
	(void)g;
	return t1 - t0;
}

/* ---- C: resident set after a burst comes and goes ------------------------- */

/*
 * Hold @live connections' worth of buffers at once (the working set), touch
 * every page so it is really resident, then free all but a few stragglers and
 * see what each allocator gives back. This is the proxy-under-load shape: a
 * burst fills memory, the burst passes, the memory should return.
 */
static void
bench_rss(unsigned live)
{
	struct mempool pool;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(0, (size_t)2 << 30, 0);
	struct buf (*heap)[NBUF];
	struct buf (*pb)[NBUF];
	size_t base, heap_peak, heap_free, heap_after, pool_peak, pool_after;
	unsigned c;
	int i;

	heap = calloc(live, sizeof(*heap));
	pb = calloc(live, sizeof(*pb));

	/* -- heap -- */
	base = rss_bytes();
	for (c = 0; c < live; c++)
		for (i = 0; i < NBUF; i++) {
			size_t sz = heavy[i].start << heavy[i].grows;
			heap[c][i].size = sz;
			heap[c][i].p = malloc(sz);
			if (heap[c][i].p)
				memset(heap[c][i].p, 0x5a, sz);   /* fault it in */
		}
	heap_peak = rss_bytes();
	for (c = 0; c < live; c++)
		for (i = 0; i < NBUF; i++) {
			if (c % 32)                       /* keep 1/32 alive */
				free(heap[c][i].p);
			else
				heap[c][i].p = NULL;
		}
	heap_free = rss_bytes();                          /* what a server sees */
#ifdef __GLIBC__
	malloc_trim(0);                                   /* only if it asks */
#endif
	heap_after = rss_bytes();

	/* -- pool -- */
	mempool_init(&pool, &pol);
	base = rss_bytes();
	for (c = 0; c < live; c++)
		for (i = 0; i < NBUF; i++) {
			size_t sz = heavy[i].start << heavy[i].grows;
			pb[c][i].size = sz;
			pb[c][i].p = mempool_alloc(&pool, sz);
			if (pb[c][i].p)
				memset(pb[c][i].p, 0x5a, sz);
		}
	pool_peak = rss_bytes();
	for (c = 0; c < live; c++)
		for (i = 0; i < NBUF; i++)
			if (c % 32 && pb[c][i].p)
				mempool_free(&pool, pb[c][i].p, pb[c][i].size);
	mempool_gc(&pool, 0);                             /* dwell 0: release now */
	pool_after = rss_bytes();
	mempool_fini(&pool);

	printf("\nC. resident set after a burst of %u connections passes\n", live);
	printf("   (each holds 4 heavy buffers; 1/32 kept alive)\n");
	printf("   %-28s %9s %11s\n", "", "peak MiB", "settled MiB");
	printf("   %-28s %9.1f %11.1f\n", "libc heap (as a server runs)",
	       heap_peak / 1048576.0, heap_free / 1048576.0);
	printf("   %-28s %9.1f %11.1f\n", "libc heap (after malloc_trim)",
	       heap_peak / 1048576.0, heap_after / 1048576.0);
	printf("   %-28s %9.1f %11.1f\n", "mempool (after gc)",
	       pool_peak / 1048576.0, pool_after / 1048576.0);
	printf("   held after the burst:  heap %.0f MiB (no trim) / %.0f MiB "
	       "(trim),  pool %.0f MiB\n",
	       heap_free / 1048576.0, heap_after / 1048576.0,
	       pool_after / 1048576.0);
	(void)base;
	free(heap);
	free(pb);
}

int
main(int argc, char **argv)
{
	unsigned conns = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0)
	                          : 200000;
	unsigned reps = 5, r;
	struct mempool pool;
	struct mempool_policy pol = MEMPOOL_POLICY_GRADUAL(0, (size_t)1 << 30,
	                                                   30000);
	u64 heap_ns = ~0ull, pool_ns = ~0ull, hmoved = 0, pmoved = 0, dummy;

	printf("mempool vs libc heap - the h2/h3 buffer lifecycle\n");
	printf("page %ld B, %u connections/rep, best of %u\n",
	       sysconf(_SC_PAGESIZE), conns, reps);

	/* warm both, then take the best of several reps to shed scheduler noise */
	for (r = 0; r < reps; r++) {
		u64 t;
		rng = 0x9e3779b97f4a7c15ull;
		t = run_heap(conns, &hmoved);
		if (t < heap_ns)
			heap_ns = t;
	}
	mempool_init(&pool, &pol);
	for (r = 0; r < reps; r++) {
		u64 t;
		rng = 0x9e3779b97f4a7c15ull;
		t = run_pool(&pool, conns, &pmoved);
		if (t < pool_ns)
			pool_ns = t;
	}
	mempool_fini(&pool);
	(void)dummy;

	printf("\nA. throughput (lower ns is better)\n");
	printf("   %-20s %12s %14s\n", "", "ns/conn", "conns/sec");
	printf("   %-20s %12.1f %14.0f\n", "libc heap",
	       (double)heap_ns / conns, 1e9 * conns / heap_ns);
	printf("   %-20s %12.1f %14.0f\n", "mempool",
	       (double)pool_ns / conns, 1e9 * conns / pool_ns);
	printf("   mempool is %.2fx the heap's time\n",
	       (double)pool_ns / heap_ns);

	printf("\nB. grow copies (bytes memcpy'd because a grow had to move)\n");
	printf("   libc heap realloc moved:  %.1f MiB over the run\n",
	       hmoved / 1048576.0);
	printf("   mempool grow moved:       %.1f MiB over the run\n",
	       pmoved / 1048576.0);

	bench_rss(2000);
	return 0;
}
