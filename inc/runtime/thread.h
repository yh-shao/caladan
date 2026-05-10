/*
 * thread.h - support for user-level threads
 */

#pragma once

#include <base/compiler.h>
#include <base/list.h>
#include <base/thread.h>
#include <base/trapframe.h>
#include <base/types.h>
#include <runtime/preempt.h>
#include <iokernel/control.h>

struct thread;
typedef void (*thread_fn_t)(void *arg);
typedef struct thread thread_t;

/*
 * Internal thread structure, only intended for building low level primitives.
 */
struct thread {
	bool	main_thread:1;
	bool	has_fsbase:1;
	bool	thread_ready:1;
	bool	link_armed:1;
	bool	junction_thread;
	bool	thread_running;
	bool	in_syscall;
	/* modified by interrupt handler; should not be shared with other bitfields */
	bool	xsave_area_in_use:1;
	atomic8_t	interrupt_state;
	uint8_t		runtime_fsbase_depth;   // 表示当前 uthread 是否处于“主动切换到 runtime FS base 的区域”内，支持嵌套 guard
	struct thread_tf	*entry_regs;
	unsigned long	junction_tstate_buf[8];
	struct stack	*stack;
	uint16_t	last_cpu;
	uint16_t	cur_kthread;
	uint64_t	ready_tsc;
	uint64_t	total_cycles;
	struct thread_tf	tf;
	struct list_node	link;
	struct list_node	interruptible_link;
	size_t			waitq_micros;
	uint64_t	tlsvar;
	uint64_t	fsbase;
	unsigned long	junction_cold_state_buf[32];
};

extern uint64_t thread_get_total_cycles(thread_t *th);

/*
 * Low-level routines, these are helpful for bindings and synchronization
 * primitives.
 */

extern void thread_park_and_unlock_np(spinlock_t *l);
extern void thread_park_and_preempt_enable(void);
extern void thread_ready(thread_t *thread);
extern void thread_ready_head(thread_t *thread);
extern thread_t *thread_create(thread_fn_t fn, void *arg);
extern thread_t *thread_create_with_buf(thread_fn_t fn, void **buf, size_t len);
extern void thread_set_fsbase(thread_t *th, uint64_t fsbase);
extern void thread_free(thread_t *th);

DECLARE_PERTHREAD(thread_t *, __self);
DECLARE_PERTHREAD_ALIAS(thread_t * const, __self, __const_self);
DECLARE_PERTHREAD(uint64_t, runtime_fsbase);

static inline unsigned int get_current_affinity(void)
{
	return this_thread_id();
}

/**
 * thread_self - gets the currently running thread
 */
static inline thread_t *thread_self(void)
{
	return perthread_read_const_p(__const_self);
}


/*
 * High-level routines, use this API most of the time.
 */

extern void thread_yield(void);
extern int thread_spawn(thread_fn_t fn, void *arg);
extern void thread_exit(void) __noreturn;
