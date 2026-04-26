/*
 * softirq.c - handles backend processing (I/O, timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <base/lock.h>
#include <base/slab.h>
#include <runtime/thread.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "net/defs.h"
#include <dml/dml.h>
int dsa_ready = 0;

static struct slab dsa_req_slab;
static struct tcache *dsa_req_tcache;
static size_t dsa_req_size;
static DEFINE_SPINLOCK(dsa_req_pool_lock);
static DEFINE_PERTHREAD(struct tcache_perthread, dsa_req_pt);
static DEFINE_PERTHREAD(bool, dsa_req_pt_ready);

int dsa_req_pool_init(size_t req_size)
{
	int ret;

	if (req_size < TCACHE_MIN_ITEM_SIZE) return -EINVAL;

	spin_lock(&dsa_req_pool_lock);
	if (dsa_req_tcache) 
	{
		ret = dsa_req_size == req_size ? 0 : -EINVAL;
		spin_unlock(&dsa_req_pool_lock);
		return ret;
	}

	ret = slab_create(&dsa_req_slab, "dsa_req", req_size, 0);
	if (ret) 
	{
		spin_unlock(&dsa_req_pool_lock);
		return ret;
	}

	dsa_req_tcache = slab_create_tcache(&dsa_req_slab, TCACHE_DEFAULT_MAG_SIZE);
	if (!dsa_req_tcache) 
	{
		slab_destroy(&dsa_req_slab);
		spin_unlock(&dsa_req_pool_lock);
		return -ENOMEM;
	}

	dsa_req_size = req_size;
	spin_unlock(&dsa_req_pool_lock);
	return 0;
}

static struct tcache_perthread *dsa_req_get_pt(void)
{
	struct tcache_perthread *pt = perthread_ptr(dsa_req_pt);

	if (unlikely(!perthread_read(dsa_req_pt_ready))) 
	{
		tcache_init_perthread(dsa_req_tcache, pt);
		perthread_store(dsa_req_pt_ready, true);
	}

	return pt;
}

struct dsa_req* dsa_req_alloc(void)
{
	if (unlikely(!dsa_req_tcache)) return NULL;
	return tcache_alloc(dsa_req_get_pt());
}
void dsa_req_free(struct dsa_req* req)
{
	if (!req) return;
	tcache_free(dsa_req_get_pt(), req);
}

static bool softirq_iokernel_pending(struct kthread *k)
{
	return !lrpc_empty(&k->rxq);
}

static bool softirq_timer_pending(struct kthread *k, uint64_t now_tsc)
{
	return ACCESS_ONCE(k->next_timer_tsc) <= now_tsc;
}

/**
 * softirq_pending - is there a softirq pending?
 */
bool softirq_pending(struct kthread *k, uint64_t now_tsc)
{
	return softirq_iokernel_pending(k) || softirq_timer_pending(k, now_tsc) ||
	       storage_available_completions(k) ||
	       (dsa_ready && !list_empty_volatile(&k->pending_dsa_jobs));
}

void dsa_req_enqueue_and_park(struct dsa_req *req)
{
	struct kthread *k = getk();

	spin_lock(&k->lock);
	list_add_tail(&k->pending_dsa_jobs, &req->link);
	thread_park_and_unlock_np(&k->lock);
}

static bool dsa_process_completions(struct kthread *k)   // 检查 DSA 是否完成
{
	bool found = false;
    struct dsa_req *req, *next;
	list_for_each_safe(&k->pending_dsa_jobs, req, next, link)  // 遍历链表检查
	{
        dml_status_t status = dml_check_job(&req->job);  // 非阻塞检查硬件状态
		if (status == DML_STATUS_BEING_PROCESSED) continue; // 还没做完

		if (unlikely(status != DML_STATUS_OK)) 
		{
			log_warn("DSA job completed with status %d, falling back to CPU memcpy", status);
			memcpy(req->job.destination_first_ptr, req->job.source_first_ptr, req->job.source_length);
		}

        list_del(&req->link);                 // 从公告板撕下来
        thread_ready_head_locked(req->waiting_th); // 唤醒线程
		dml_finalize_job(&req->job);
		dsa_req_free(req);
		found = true;
    }
	return found;
}

/**
 * softirq_run_locked - schedule softirq work with kthread lock held
 * @k: the kthread to check for softirq work
 *
 * The kthread's lock must be held when calling this function.
 *
 * Returns true if softirq work was scheduled.
 */
bool softirq_run_locked(struct kthread *k)
{
	uint64_t now_tsc = rdtsc();
	bool work_done = false;

	assert_preempt_disabled();
	assert_spin_lock_held(&k->lock);

	/* check for iokernel softirq work */
	// if (!k->iokernel_busy && softirq_iokernel_pending(k)) {
	// 	k->iokernel_busy = true;
	// 	thread_ready_head_locked(k->iokernel_softirq);
	// 	work_done = true;
	// }

	/* check for directpath softirq work */
	// work_done |= rx_poll_locked(k);

	/* check for timer softirq work */
	if (!k->timer_busy && softirq_timer_pending(k, now_tsc)) {
		k->timer_busy = true;
		thread_ready_head_locked(k->timer_softirq);
		work_done = true;
	}

	/* check for storage softirq work */
	if (!k->storage_busy && storage_available_completions(k)) {
		k->storage_busy = true;
		thread_ready_head_locked(k->storage_softirq);
		work_done = true;
	}

	if (dsa_ready && !list_empty(&k->pending_dsa_jobs)) {
        work_done = dsa_process_completions(k);
    }

	k->last_softirq_tsc = now_tsc;
	return work_done;
}

/**
 * softirq_run - schedule softirq work
 *
 * Returns true if softirq work was scheduled.
 */
bool softirq_run(void)
{
	struct kthread *k;
	uint64_t now_tsc = rdtsc();
	bool work_done;

	k = getk();
	k->last_softirq_tsc = now_tsc;

	if (!softirq_pending(k, now_tsc)) {
		work_done = rx_poll(k);
		putk();
		return work_done;
	}
	spin_lock(&k->lock);
	work_done = softirq_run_locked(k);
	spin_unlock(&k->lock);
	putk();

	return work_done;
}
