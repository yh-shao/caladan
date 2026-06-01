/*
 * test_storage_async_iops.c - fixed-QD raw storage benchmark for Caladan.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/atomic.h>
#include <base/log.h>
#include <base/stddef.h>
#include <base/time.h>
#include <runtime/runtime.h>
#include <runtime/storage.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
#include <runtime/timer.h>
#include <spdk/env.h>

#define DEFAULT_POLLERS		1
#define DEFAULT_QD		64
#define DEFAULT_SECONDS		20
#define DEFAULT_REQUEST_BYTES	4096
#define DEFAULT_POLL_BATCH	0

struct bench_cfg {
	unsigned int pollers;
	unsigned int qd;
	unsigned int seconds;
	bool write;
	bool random;
	uint64_t range_mb;
	uint64_t base_lba;
	uint32_t request_bytes;
	uint32_t poll_batch;
};

struct io_slot {
	struct storage_async_req req;
	struct poller_arg *poller;
	int status;
};

struct poller_arg {
	unsigned int id;
	waitgroup_t *wg;
	barrier_t *start_barrier;
	uint64_t deadline_us;
	uint64_t max_units;
	uint64_t base_lba;
	uint32_t lba_count;
	struct io_slot *slots;
	uint64_t rnd;
	uint64_t next_seq;
	uint64_t completed;
	uint64_t errors;
	uint32_t inflight;
	uint32_t cq_head;
	uint32_t cq_tail;
	struct io_slot **cq;
	bool stop_submit;
} __aligned(CACHE_LINE_SIZE);

static struct bench_cfg cfg = {
	.pollers = DEFAULT_POLLERS,
	.qd = DEFAULT_QD,
	.seconds = DEFAULT_SECONDS,
	.write = false,
	.random = true,
	.range_mb = 0,
	.base_lba = 0,
	.request_bytes = DEFAULT_REQUEST_BYTES,
	.poll_batch = DEFAULT_POLL_BATCH,
};

static uint64_t splitmix64_next(uint64_t *state)
{
	uint64_t z;

	*state += 0x9e3779b97f4a7c15UL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
	return z ^ (z >> 31);
}

static uint64_t rand_below(uint64_t rnd, uint64_t range)
{
	return (uint64_t)(((__uint128_t)rnd * range) >> 64);
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long val;

	errno = 0;
	val = strtoull(s, &end, 0);
	if (errno || *end != '\0')
		return -EINVAL;
	*out = val;
	return 0;
}

static uint64_t next_lba(struct poller_arg *p)
{
	uint64_t unit;

	if (cfg.random)
		unit = rand_below(splitmix64_next(&p->rnd), p->max_units);
	else
		unit = p->next_seq++ % p->max_units;
	return p->base_lba + unit * p->lba_count;
}

static int submit_slot(struct io_slot *slot)
{
	struct poller_arg *p = slot->poller;

	slot->req.lba = next_lba(p);
	if (cfg.write)
		return storage_async_write(&slot->req);
	return storage_async_read(&slot->req);
}

static void complete_slot(struct storage_async_req *req, void *arg, int status)
{
	struct io_slot *slot = arg;
	struct poller_arg *p = slot->poller;
	uint32_t tail = p->cq_tail++;

	slot->status = status;
	barrier();
	p->cq[tail & (cfg.qd - 1)] = slot;
}

static void *alloc_slot_buf(void)
{
	void *buf;

	buf = spdk_zmalloc(cfg.request_bytes, cfg.request_bytes, NULL,
			  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (buf && cfg.write)
		memset(buf, 0x5a, cfg.request_bytes);
	return buf;
}

static void free_slots(struct poller_arg *p)
{
	if (!p->slots)
		return;

	for (unsigned int i = 0; i < cfg.qd; i++)
		if (p->slots[i].req.buf)
			spdk_free(p->slots[i].req.buf);
	free(p->slots);
}

static void poller_handler(void *arg)
{
	struct poller_arg *p = arg;
	uint64_t completed = 0;
	uint64_t errors = 0;
	int ret;

	p->slots = calloc(cfg.qd, sizeof(*p->slots));
	BUG_ON(!p->slots);
	BUG_ON((cfg.qd & (cfg.qd - 1)) != 0);
	p->cq = calloc(cfg.qd, sizeof(*p->cq));
	BUG_ON(!p->cq);

	for (unsigned int i = 0; i < cfg.qd; i++) {
		struct io_slot *slot = &p->slots[i];

		slot->poller = p;
		slot->req.buf = alloc_slot_buf();
		BUG_ON(!slot->req.buf);
		slot->req.lba_count = p->lba_count;
		slot->req.cb = complete_slot;
		slot->req.cb_arg = slot;
	}

	barrier_wait(p->start_barrier);

	for (unsigned int i = 0; i < cfg.qd; i++) {
		ret = submit_slot(&p->slots[i]);
		if (unlikely(ret != 0)) {
			log_err("poller %u initial submit failed: ret=%d", p->id, ret);
			errors++;
			p->stop_submit = true;
			break;
		}
		p->inflight++;
	}

	while (p->inflight > 0) {
		ret = storage_async_poll(cfg.poll_batch);
		if (unlikely(ret < 0)) {
			log_err("poller %u poll failed: ret=%d", p->id, ret);
			errors++;
			p->stop_submit = true;
			break;
		}
		if (ret == 0)
			cpu_relax();

		while (p->cq_head != p->cq_tail) {
			struct io_slot *slot = p->cq[p->cq_head & (cfg.qd - 1)];

			p->cq_head++;
			p->inflight--;
			if (unlikely(slot->status != 0)) {
				errors++;
				p->stop_submit = true;
				continue;
			}

			completed++;

			if ((completed & 0xffUL) == 0 &&
			    microtime() >= p->deadline_us) {
				p->stop_submit = true;
				continue;
			}
			if (p->stop_submit)
				continue;

			ret = submit_slot(slot);
			if (unlikely(ret != 0)) {
				log_err("poller %u resubmit failed: ret=%d", p->id, ret);
				errors++;
				p->stop_submit = true;
				continue;
			}
			p->inflight++;
		}
	}

	p->completed = completed;
	p->errors = errors;
	log_info("poller[%u] kthread=%u completed=%ld errors=%ld",
		 p->id, get_current_affinity(), p->completed, p->errors);
	free(p->cq);
	free_slots(p);
	waitgroup_done(p->wg);
}

static uint64_t range_units_for_device(uint64_t num_lbas, uint32_t lba_count)
{
	uint64_t available_lbas = num_lbas - cfg.base_lba;
	uint64_t units = available_lbas / lba_count;

	if (cfg.range_mb != 0) {
		uint64_t range_bytes = cfg.range_mb * 1024UL * 1024UL;
		uint64_t requested_units = range_bytes / cfg.request_bytes;

		if (requested_units == 0)
			requested_units = 1;
		if (requested_units < units)
			units = requested_units;
	}

	return units;
}

static void main_handler(void *arg)
{
	waitgroup_t wg;
	barrier_t start_barrier;
	struct poller_arg *pollers;
	uint64_t num_lbas, max_units, start_us, elapsed_us, total, errors;
	uint64_t effective_range_mb;
	uint32_t sector_size, lba_count;
	double seconds, iops, mibps;
	int ret;

	sector_size = storage_block_size();
	num_lbas = storage_num_blocks();
	BUG_ON(sector_size == 0);
	BUG_ON(cfg.request_bytes == 0 || cfg.request_bytes % sector_size != 0);
	lba_count = cfg.request_bytes / sector_size;
	BUG_ON(cfg.base_lba >= num_lbas);
	BUG_ON(num_lbas - cfg.base_lba < lba_count);
	max_units = range_units_for_device(num_lbas, lba_count);
	BUG_ON(max_units == 0);
	effective_range_mb = max_units * (uint64_t)cfg.request_bytes /
			     (1024UL * 1024UL);

	log_info("async config: pollers=%u qd=%u seconds=%u op=%s pattern=%s range_mb=%lu effective_range_mb=%lu request_bytes=%u poll_batch=%u",
		 cfg.pollers, cfg.qd, cfg.seconds, cfg.write ? "write" : "read",
		 cfg.random ? "rand" : "seq", cfg.range_mb, effective_range_mb,
		 cfg.request_bytes, cfg.poll_batch);
	log_info("device: sector_size=%u num_lbas=%lu base_lba=%lu lba_count=%u random_units=%lu runtime_max_cores=%d active_cores=%d",
		 sector_size, num_lbas, cfg.base_lba, lba_count, max_units,
		 runtime_max_cores(), runtime_active_cores());
	if (cfg.random && effective_range_mb < 524288)
		log_warn("random range is below 512GiB; PM9A3 randread IOPS may be underestimated");

	pollers = calloc(cfg.pollers, sizeof(*pollers));
	BUG_ON(!pollers);

	waitgroup_init(&wg);
	barrier_init(&start_barrier, cfg.pollers + 1);
	waitgroup_add(&wg, cfg.pollers);

	start_us = microtime();
	for (unsigned int i = 0; i < cfg.pollers; i++) {
		pollers[i].id = i;
		pollers[i].wg = &wg;
		pollers[i].start_barrier = &start_barrier;
		pollers[i].deadline_us = start_us + (uint64_t)cfg.seconds * 1000000UL;
		pollers[i].max_units = max_units;
		pollers[i].base_lba = cfg.base_lba;
		pollers[i].lba_count = lba_count;
		pollers[i].rnd = 0x9f4a7c15d1ce4e5bUL ^ ((uint64_t)i << 32);
		ret = thread_spawn(poller_handler, &pollers[i]);
		BUG_ON(ret);
	}

	barrier_wait(&start_barrier);
	waitgroup_wait(&wg);
	elapsed_us = microtime() - start_us;

	total = 0;
	errors = 0;
	for (unsigned int i = 0; i < cfg.pollers; i++) {
		total += pollers[i].completed;
		errors += pollers[i].errors;
	}
	seconds = (double)elapsed_us * 0.000001;
	iops = seconds ? (double)total / seconds : 0.0;
	mibps = iops * (double)cfg.request_bytes / (1024.0 * 1024.0);
	log_info("async result: ios=%lu elapsed_us=%lu iops=%.3f MiBps=%.3f errors=%ld",
		 total, elapsed_us, iops, mibps, errors);

	BUG_ON(errors != 0);
	free(pollers);
}

static void usage(const char *prog)
{
	printf("usage: %s <config> [pollers] [qd] [seconds] [op] [pattern] [range_mb] [base_lba] [request_bytes] [poll_batch]\n", prog);
	printf("  op:      read or write (default: read)\n");
	printf("  pattern: rand or seq (default: rand)\n");
	printf("  range_mb: 0 means whole namespace after base_lba (default: 0)\n");
}

int main(int argc, char *argv[])
{
	uint64_t val;
	int ret;

	if (argc < 2 || argc > 11) {
		usage(argv[0]);
		return -EINVAL;
	}

	if (argc > 2) {
		ret = parse_u64(argv[2], &val);
		if (ret || val == 0 || val > NTHREAD)
			return -EINVAL;
		cfg.pollers = val;
	}
	if (argc > 3) {
		ret = parse_u64(argv[3], &val);
		if (ret || val == 0 || val > 4096)
			return -EINVAL;
		cfg.qd = val;
	}
	if (argc > 4) {
		ret = parse_u64(argv[4], &val);
		if (ret || val == 0 || val > 3600)
			return -EINVAL;
		cfg.seconds = val;
	}
	if (argc > 5) {
		if (!strcmp(argv[5], "read")) {
			cfg.write = false;
		} else if (!strcmp(argv[5], "write")) {
			cfg.write = true;
		} else {
			return -EINVAL;
		}
	}
	if (argc > 6) {
		if (!strcmp(argv[6], "rand")) {
			cfg.random = true;
		} else if (!strcmp(argv[6], "seq")) {
			cfg.random = false;
		} else {
			return -EINVAL;
		}
	}
	if (argc > 7) {
		ret = parse_u64(argv[7], &cfg.range_mb);
		if (ret)
			return -EINVAL;
	}
	if (argc > 8) {
		ret = parse_u64(argv[8], &cfg.base_lba);
		if (ret)
			return -EINVAL;
	}
	if (argc > 9) {
		ret = parse_u64(argv[9], &val);
		if (ret || val == 0 || val > (16UL * 1024UL))
			return -EINVAL;
		cfg.request_bytes = val;
	}
	if (argc > 10) {
		ret = parse_u64(argv[10], &val);
		if (ret || val > 4096)
			return -EINVAL;
		cfg.poll_batch = val;
	}

	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		printf("failed to start runtime\n");
		return ret;
	}

	return 0;
}
