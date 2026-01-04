/*
 * softirq.c - handles backend processing (I/O, timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>

#include "defs.h"
#include "net/defs.h"
#include <dml/dml.h>

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
	       storage_available_completions(k);
}

struct list_head pending_dsa_jobs[100];
int dsa_ready = 0;
static bool dsa_process_completions(struct kthread *k)   // 检查 DSA 是否完成
{
	bool found = false;
    struct dsa_req *req, *next;
    
    // 遍历链表检查
	list_for_each_safe(&pending_dsa_jobs[this_thread_id()], req, next, link)
	{
        dml_status_t status = dml_check_job(&req->job);  // 非阻塞检查硬件状态
        // if (status == DML_STATUS_BEING_PROCESSED) continue; // 还没做完
		if (status != DML_STATUS_OK) continue; // 还没成功
        
        // 做完了
        list_del(&req->link);                 // 从公告板撕下来
        thread_ready_head_locked(req->waiting_th); // 唤醒线程
		found = true;
		log_info("DSA job completed for kthread %u, add uthread %p to runqueue", this_thread_id(), req->waiting_th);
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
	if (!k->iokernel_busy && softirq_iokernel_pending(k)) {
		k->iokernel_busy = true;
		thread_ready_head_locked(k->iokernel_softirq);
		work_done = true;
	}

	/* check for directpath softirq work */
	work_done |= rx_poll_locked(k);

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
		log_info("SPDK IO finished, add storage_softirq uthread to the rq of kthread %u", this_thread_id());
		work_done = true;
	}

	if (dsa_ready && !list_empty(&pending_dsa_jobs[this_thread_id()])) {
        work_done = dsa_process_completions(k);
		log_info("DSA completions processed for kthread %u", this_thread_id());
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
