/*
 * async.h - generic runtime support for parking threads on async work.
 */
#pragma once
#include <stdbool.h>
#include <base/list.h>
#include <runtime/thread.h>

struct runtime_async_op 
{
	thread_t			*waiting_th;
	struct list_node	link;
	bool				(*poll)(struct runtime_async_op* op);
	void				(*complete)(struct runtime_async_op* op);
};

extern void runtime_async_park(struct runtime_async_op *op);
extern bool runtime_async_would_hide_latency(void);
