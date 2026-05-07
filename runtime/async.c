/*
 * async.c - generic runtime support for parking threads on async work.
 */

#include <base/assert.h>
#include <runtime/async.h>
#include <runtime/thread.h>

#include "defs.h"

void runtime_async_park(struct runtime_async_op* op)
{
	struct kthread *k = getk();
	op->waiting_th = thread_self();
	spin_lock(&k->lock);
	list_add_tail(&k->pending_async_ops, &op->link);
	thread_park_and_unlock_np(&k->lock);
}

bool runtime_async_would_hide_latency(void)  // 判断当前核上是否有其它 uthread 等待被执行
{
	struct kthread *k = getk();
	bool has_ready_work = ACCESS_ONCE(k->rq_head) != ACCESS_ONCE(k->rq_tail);
	putk();
	return has_ready_work;
}

bool runtime_async_pending(struct kthread *k)
{
	return !list_empty_volatile(&k->pending_async_ops);
}

bool runtime_async_process(struct kthread *k)
{
	assert_spin_lock_held(&k->lock);

	bool found = false;
	struct runtime_async_op *op, *next;
	list_for_each_safe(&k->pending_async_ops, op, next, link) 
	{
		if (!op->poll(op)) continue;

		list_del(&op->link);
		thread_ready_head_locked(op->waiting_th);
		if (op->complete) op->complete(op);
		found = true;
	}

	return found;
}
