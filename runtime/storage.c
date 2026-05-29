/*
 * storage.c
 */

#include <runtime/storage.h>


uint32_t block_size;
uint64_t num_blocks;

#ifdef DIRECT_STORAGE
#include <stdio.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/mempool.h>
#include <base/syscall.h>
#include <runtime/sync.h>

// Hack to prevent SPDK from pulling in extra headers here
#define SPDK_STDINC_H
#include <sys/uio.h>
#include <spdk/nvme.h>
#include <spdk/env.h>

#include "defs.h"

unsigned long storage_device_latency_us = 100;
bool cfg_storage_enabled;

static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *spdk_namespace;

/* 4KB storage request buffers */
#define REQUEST_BUF_POOL_SZ (PGSIZE_2MB * 20)
#define REQUEST_BUF_SZ (16 * KB)
struct mempool storage_buf_mp;
static struct tcache *storage_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, storage_buf_pt);

#define USER_DMA_CACHE_SIZE 4096
BUILD_ASSERT(is_power_of_two(USER_DMA_CACHE_SIZE));
struct user_dma_page {
	uintptr_t base;
};
static struct user_dma_page user_dma_pages[USER_DMA_CACHE_SIZE];
static DEFINE_SPINLOCK(user_dma_lock);             // 保护 user_dma_pages[] cache 的访问

static bool user_dma_registered_logged;
static bool user_dma_failed_logged;     // 控制日志打印频率，无论成功或失败的日志，在整个生命周期中只打印一次

static DEFINE_SPINLOCK(user_dma_register_lock);    // 保护 spdk_mem_register 的调用

struct nvme_device {
	const char *name;
	unsigned long latency_us;
} known_devices[1] = {
	{
		.name = "INTEL SSDPED1D280GA",
		.latency_us = 10,
	}
};

static size_t user_dma_cache_slot(uintptr_t page_base)
{
	return (page_base >> PGSHIFT_2MB) & (USER_DMA_CACHE_SIZE - 1);
}

static bool user_dma_cache_lookup(uintptr_t reg_base, uintptr_t reg_end)
{
	for (uintptr_t page = reg_base; page < reg_end; page += PGSIZE_2MB) 
		if (user_dma_pages[user_dma_cache_slot(page)].base != page) return false;
	return true;
}

static void user_dma_cache_insert(uintptr_t reg_base, uintptr_t reg_end)
{
	for (uintptr_t page = reg_base; page < reg_end; page += PGSIZE_2MB)
		user_dma_pages[user_dma_cache_slot(page)].base = page;
}

enum storage_quota_op {
	STORAGE_QUOTA_READ,
	STORAGE_QUOTA_WRITE,
};

static inline int storage_quota_admit(enum storage_quota_op op, uint32_t lba_count)
{
	(void)op;
	if (likely(!cfg_storage_quota_enabled)) return 0;
	if (unlikely(block_size == 0)) return -EINVAL;
	if (unlikely(lba_count == 0)) return 0;

	size_t bytes = (size_t)lba_count * block_size;
	if (unlikely(bytes / block_size != lba_count)) return -EINVAL;
	return storage_quota_wait(1, bytes);
}

static inline void storage_quota_cancel(enum storage_quota_op op, uint32_t lba_count)
{
	(void)op;
	if (likely(!cfg_storage_quota_enabled) || unlikely(block_size == 0) ||
	    unlikely(lba_count == 0)) return;

	size_t bytes = (size_t)lba_count * block_size;
	if (likely(bytes / block_size == lba_count)) storage_quota_refund(1, bytes);
}

static bool user_dma_range_vtophys_ok(void *buf, size_t len)
{
	uintptr_t pos = (uintptr_t)buf;
	uintptr_t end = pos + len;

	while (pos < end) 
	{
		uint64_t chunk = end - pos;
		uint64_t iova = spdk_vtophys((void *)pos, &chunk);
		if (unlikely(iova == SPDK_VTOPHYS_ERROR || chunk == 0)) return false;
		pos += chunk;
	}
	return true;
}

static bool user_dma_registration_range(void *buf, size_t len, uintptr_t *reg_base, uintptr_t *reg_end)
{
	uintptr_t addr = (uintptr_t)buf;
	uintptr_t end = addr + len;

	if (unlikely(buf == NULL || len == 0 || end < addr)) return false;
	if (unlikely((addr & PGMASK_4KB) != 0 || (len & PGMASK_4KB) != 0)) return false;

	*reg_base = PGADDR_2MB(addr);
	*reg_end = align_up(end, PGSIZE_2MB);
	return *reg_end > *reg_base;
}

static int storage_register_user_dma(void *buf, size_t len)
{
	uintptr_t reg_base, reg_end;
	int rc;

	if (unlikely(!user_dma_registration_range(buf, len, &reg_base, &reg_end))) return -EINVAL;

	spin_lock_np(&user_dma_lock);
	if (user_dma_cache_lookup(reg_base, reg_end))    // 查本地 2MB 页粒度 cache：user_dma_pages[]，避免重复注册；如果 cache 没命中，再进行后续的 mlock + spdk_mem_register
	{
		spin_unlock_np(&user_dma_lock);
		return user_dma_range_vtophys_ok(buf, len) ? 0 : -EFAULT;
	}
	spin_unlock_np(&user_dma_lock);

	rc = syscall_mlock((void *)reg_base, reg_end - reg_base);            // 把覆盖 user buffer 的 2MB 注册区 pin 在内存中，避免 DMA 过程中被换出
	if (unlikely(rc != 0))
	{
		if (!user_dma_failed_logged) 
		{
			user_dma_failed_logged = true;
			log_err("storage: mlock user DMA region failed rc=%d base=%p len=%zu", rc, (void *)reg_base, (size_t)(reg_end - reg_base));
		}
		return rc;
	}

	preempt_disable();
	spin_lock(&user_dma_register_lock);
	rc = spdk_mem_register((void *)reg_base, reg_end - reg_base);         // SPDK/DPDK/VFIO 以 2MB 粒度建立虚拟地址到 IOVA 的映射
	if (unlikely(rc == -EBUSY))   // 说明可能部分页面已经注册过，则按 2MB 页面逐段注册
	{
		uintptr_t seg = reg_base;
		while (seg < reg_end) 
		{
			rc = spdk_mem_register((void *)seg, PGSIZE_2MB);
			if (rc != 0 && rc != -EBUSY) break;
			seg += PGSIZE_2MB;
		}
	}
	spin_unlock(&user_dma_register_lock);
	preempt_enable();
	if (unlikely(rc != 0 && rc != -EBUSY)) 
	{
		if (!user_dma_failed_logged) 
		{
			user_dma_failed_logged = true;
			log_err("storage: spdk_mem_register user buffer failed rc=%d base=%p len=%zu", rc, (void *)reg_base, (size_t)(reg_end - reg_base));
		}
		return rc;
	}
	if (unlikely(!user_dma_range_vtophys_ok(buf, len)))  // 调用 spdk_vtophys() 遍历整个范围，确认每一段都能转换为可 DMA 的 IOVA
	{
		if (!user_dma_failed_logged) 
		{
			user_dma_failed_logged = true;
			log_err("storage: user DMA vtophys validation failed base=%p len=%zu", buf, len);
		}
		return -EFAULT;
	}

	spin_lock_np(&user_dma_lock);
	user_dma_cache_insert(reg_base, reg_end);   // 成功后把 2MB page base 写入 user_dma_pages[] cache，加速后续相同页面的注册
	spin_unlock_np(&user_dma_lock);

	if (!user_dma_registered_logged) 
	{
		user_dma_registered_logged = true;
		log_info("storage: enabled direct DMA into user buffers");
	}
	return 0;
}

int storage_prepare_user_dma(void *buf, size_t len)
{
	return storage_register_user_dma(buf, len);
}

static void seq_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct thread *th = arg;

	if (runtime_info && atomic64_read(&runtime_info->spdk_uipi))
		thread_ready_head(th);
	else
		thread_ready(th);
}

struct storage_completion {
	struct thread *thread;
	int status;
};
static void seq_status_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct storage_completion *ctx = arg;

	ctx->status = spdk_nvme_cpl_is_error(completion) ? -EIO : 0;
	barrier();

	if (runtime_info && atomic64_read(&runtime_info->spdk_uipi))
		thread_ready_head(ctx->thread);
	else
		thread_ready(ctx->thread);
}

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
    log_info("Probing device: %s ... ", trid->traddr);

    const char *target_addr = "0000:5b:00.0";           // 只绑定这个盘（因为目前我只用到一个盘）
    if (strcasecmp(trid->traddr, target_addr) == 0)
    {
        log_info("Matched target address %s. Attaching...\n", target_addr);
        return true;
    }
    else
    {
        log_info("Skipping non-target address %s.\n", trid->traddr);
        return false;   // 不匹配则跳过
    }
}

/**
 * attach_cb - callback run after nvme device has been attached
 *
 */
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	int i, num_ns;
	const struct spdk_nvme_ctrlr_data *ctrlr_data;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	// if (num_ns > 1) {                             // 有多个 namespace 又咋了，使用 1 个不就好了，干嘛要 exit 呢
	// 	perror("more than 1 storage devices");
	// 	exit(1);
	// }
	if (num_ns == 0) {
		perror("no storage device");
		exit(1);
	}
	controller = ctrlr;
	ctrlr_data = spdk_nvme_ctrlr_get_data(ctrlr);
	// spdk_namespace = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
	int nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);  // 使用第一个 active 的 namespace
	spdk_namespace = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);   
	if (spdk_namespace == NULL) {
        perror("Failed to get namespace structure");
        exit(1);
    }
	block_size = spdk_nvme_ns_get_sector_size(spdk_namespace);
	num_blocks = spdk_nvme_ns_get_num_sectors(spdk_namespace);
	log_info("Attached to Namespace ID: %d on Controller: %s\n", nsid, trid->traddr);

	for (i = 0; i < ARRAY_SIZE(known_devices); i++) {
		if (!strncmp((char *)ctrlr_data->mn, known_devices[i].name,
			           strlen(known_devices[i].name))) {
			log_info("storage: recognized device %s", known_devices[i].name);
			storage_device_latency_us = known_devices[i].latency_us;
			break;
		}
	}
}

/**
 * __storage_write - write a payload to the nvme device
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
// rmw（Read-Modify-Write）参数用于判断是否先进行一次读盘操作（如果需要的话）
// src 为 NULL 则写入全 0
// 将 [src, src+siz) 这段数据写到相对于 base_lba 偏移 oft 字节的位置
static int __storage_write(const void* src, size_t siz, uint64_t base_lba, off_t oft, bool rmw)
{
	if (!cfg_storage_enabled) 
	{
		log_err("__storage_write(): storage not enabled!");
		return -ENODEV;
	}
	if (unlikely(siz == 0)) return 0; // 快速返回，无需进行毫无意义的空操作

	uint64_t target_lba       = base_lba + (oft / block_size);                     // 真正受影响的起始 LBA
	off_t    inner_oft        = oft % block_size;                                  // 在 target_lba 内部的相对字节偏移
	uint32_t target_lba_count = (inner_oft + siz + block_size - 1) / block_size;   // 覆盖 siz 大小的数据总共需要跨越多少个 block
	size_t   req_size         = target_lba_count * block_size;                     // DMA Buffer 所需的实际大小

	bool use_thread_cache = req_size <= REQUEST_BUF_SZ;
	bool admit_rmw_read = rmw && siz < req_size;
	int rc;

	if (admit_rmw_read)
	{
		rc = storage_quota_admit(STORAGE_QUOTA_READ, target_lba_count);
		if (unlikely(rc != 0)) return rc;
	}

	rc = storage_quota_admit(STORAGE_QUOTA_WRITE, target_lba_count);
	if (unlikely(rc != 0))
	{
		if (admit_rmw_read) storage_quota_cancel(STORAGE_QUOTA_READ, target_lba_count);
		return rc;
	}

	struct kthread* k = getk();
	struct storage_q* q = &k->storage_q;

	void *spdk_payload;
    if (likely(use_thread_cache)) 
        spdk_payload = tcache_alloc(perthread_ptr(storage_buf_pt));
    else
        spdk_payload = spdk_zmalloc(req_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (unlikely(spdk_payload == NULL))
	{
		log_err("__storage_write(): failed to allocate spdk_payload buffer");
		storage_quota_cancel(STORAGE_QUOTA_WRITE, target_lba_count);
		if (admit_rmw_read) storage_quota_cancel(STORAGE_QUOTA_READ, target_lba_count);
        putk();
        return -ENOMEM;
    }

    if (rmw && siz < req_size)   // 如果是 RMW 且没有覆盖整个盘上区域（有未被新数据完全覆盖的盘上旧数据区域），先执行 Read
	{
        spin_lock(&q->lock);
        rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, spdk_payload, target_lba, target_lba_count, seq_complete, thread_self(), 0);               
        if (unlikely(rc != 0)) 
		{
			log_err("__storage_write(): failed to issue read command for RMW with rc=%d", rc);
            spin_unlock(&q->lock);
            storage_quota_cancel(STORAGE_QUOTA_READ, target_lba_count);
            storage_quota_cancel(STORAGE_QUOTA_WRITE, target_lba_count);
            rc = -EIO;
            goto done_np;
        }
        q->outstanding_reqs++;
        thread_park_and_unlock_np(&q->lock); 
        // 此时，spdk_payload 里已经装满了磁盘上原本的数据
		preempt_disable();
    } 
	else if (!rmw && siz < req_size) memset(spdk_payload, 0, req_size);   // 防止后面是脏数据

	// 将用户的新数据覆盖到 DMA 内存中
    if (src) 
        memcpy((char*)spdk_payload + inner_oft, src, siz); 
	else 
        memset((char*)spdk_payload + inner_oft,  0,  siz);

    spin_lock(&q->lock);
    rc = spdk_nvme_ns_cmd_write(spdk_namespace, q->spdk_qp_handle, spdk_payload, target_lba, target_lba_count, seq_complete, thread_self(), 0);
    if (unlikely(rc != 0)) 
	{
		log_err("__storage_write(): spdk_nvme_ns_cmd_write failed with rc=%d", rc);
        spin_unlock(&q->lock);
        storage_quota_cancel(STORAGE_QUOTA_WRITE, target_lba_count);
        rc = -EIO;
        goto done_np;
    }
    q->outstanding_reqs++;
	// uint64_t start_us = microtime(), end_us;
    thread_park_and_unlock_np(&q->lock);
	// end_us = microtime();
    preempt_disable();

done_np:
    if (likely(use_thread_cache))
        tcache_free(perthread_ptr(storage_buf_pt), spdk_payload);
    else
        spdk_free(spdk_payload);

    preempt_enable();

	// log_info("thread yield time: %lu us", end_us - start_us);
    return rc;
}

int storage_write(const void* payload, uint64_t lba, uint32_t lba_count)                       // buffer 的长度至少为 lba_count*storage_block_size() Byte
{
	return __storage_write(payload, lba_count * block_size, lba, 0, false);
}
int storage_write_obj(const void* obj, size_t siz, uint64_t lba_start, off_t oft)           // buffer 的长度至少为 siz  （会进行rmw，如果需要的话）
{
    return __storage_write(obj, siz, lba_start, oft, true);
}
int storage_write_obj_no_rmw(const void* obj, size_t siz, uint64_t lba_start, off_t oft)     // buffer 的长度至少为 siz
{
    return __storage_write(obj, siz, lba_start, oft, false);
}

/**
 * storage_read - read a payload from the nvme device
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
// copy_size 用来控制 memcpy 的长度
// spdk buffer -> dest （一次 memcpy）
// 将盘上相对于 base_lba 偏移 oft 字节开始的 siz 大小的数据，读取到 dest 中
static int __storage_read(void* dest, size_t siz, uint64_t base_lba, off_t oft)
{
	if (!cfg_storage_enabled) 
	{
		log_err("__storage_read(): storage not enabled!");
		return -ENODEV;    
	}
	if (unlikely(siz == 0)) return 0;
	if (dest == NULL)
	{
		log_err("__storage_read(): dest is NULL");
		return -EINVAL;
	}

	uint64_t target_lba       = base_lba + (oft / block_size);
    off_t    inner_oft        = oft % block_size; 
    uint32_t target_lba_count = (inner_oft + siz + block_size - 1) / block_size;
    size_t   req_size         = target_lba_count * block_size;

    bool use_thread_cache = req_size <= REQUEST_BUF_SZ;

    int rc = storage_quota_admit(STORAGE_QUOTA_READ, target_lba_count);
    if (unlikely(rc != 0)) return rc;

    struct kthread* k = getk();
    struct storage_q* q = &k->storage_q;

	void *spdk_payload;
    if (likely(use_thread_cache)) 
        spdk_payload = tcache_alloc(perthread_ptr(storage_buf_pt));
	else 
        spdk_payload = spdk_zmalloc(req_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

    if (unlikely(spdk_payload == NULL))
	{
		log_err("__storage_read(): failed to allocate spdk_payload buffer");
        storage_quota_cancel(STORAGE_QUOTA_READ, target_lba_count);
        putk();
        return -ENOMEM;
    }

    spin_lock(&q->lock);
    rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, spdk_payload, target_lba, target_lba_count, seq_complete, thread_self(), 0);

    if (unlikely(rc != 0)) 
	{
		log_err("__storage_read(): spdk_nvme_ns_cmd_read failed with rc=%d", rc);
        spin_unlock(&q->lock);
        storage_quota_cancel(STORAGE_QUOTA_READ, target_lba_count);
        rc = -EIO;
        goto done_np;
    }

    q->outstanding_reqs++;
	// uint64_t before_tsc = rdtsc();
    thread_park_and_unlock_np(&q->lock);
	// uint64_t after_tsc = rdtsc();
	// log_info("before yield: %lu, after yield: %lu, thread yield time = %lu us", before_tsc, after_tsc, (after_tsc - before_tsc) / cycles_per_us);
	// log_info("IO uthread start: %lu", k->storage_softirq->ready_tsc);
    memcpy(dest, (char*)spdk_payload + inner_oft, siz);   // 仅仅将用户请求的精确大小和相对偏移拷回目标内存
    preempt_disable();

done_np:
    if (likely(use_thread_cache))
        tcache_free(perthread_ptr(storage_buf_pt), spdk_payload);
    else
        spdk_free(spdk_payload);
    preempt_enable();

    return rc;
}

int storage_read(void* dest, uint64_t lba, uint32_t lba_count)               // buffer 的长度应至少为 lba_count*storage_block_size()
{
	return __storage_read(dest, lba_count * block_size, lba, 0);
}
int storage_read_obj(void* obj, size_t siz, uint64_t lba_start, off_t oft)   // buffer 的长度应至少为 siz
{
    return __storage_read(obj, siz, lba_start, oft);
}

int storage_read_aligned(void* dest, uint64_t lba, uint32_t lba_count)
{
	if (!cfg_storage_enabled) return -ENODEV;
	if (unlikely(lba_count == 0)) return 0;
	if (unlikely(dest == NULL || block_size == 0)) return -EINVAL;
	if (unlikely(((uintptr_t)dest & (block_size - 1)) != 0)) return -EINVAL;

	int rc;

	size_t bytes = (size_t)lba_count * block_size;
	if (unlikely(bytes / block_size != lba_count)) return -EINVAL;

	if (unlikely(!user_dma_range_vtophys_ok(dest, bytes))) 
	{
		rc = storage_register_user_dma(dest, bytes);
		if (unlikely(rc != 0)) return rc;
	}

	rc = storage_quota_admit(STORAGE_QUOTA_READ, lba_count);
	if (unlikely(rc != 0)) return rc;

	struct storage_completion completion = { .thread = thread_self(), .status = -EIO, };
	struct kthread *k = getk();
	struct storage_q *q = &k->storage_q;
	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, dest, lba, lba_count, seq_status_complete, &completion, 0);
	if (unlikely(rc != 0))
	{
		log_err("storage_read_aligned(): spdk_nvme_ns_cmd_read failed with rc=%d", rc);
		spin_unlock(&q->lock);
		storage_quota_cancel(STORAGE_QUOTA_READ, lba_count);
		putk();
		return -EIO;
	}

	q->outstanding_reqs++;
	thread_park_and_unlock_np(&q->lock);
	return completion.status;
}

int storage_write_user_dma(const void *src, uint64_t lba, uint32_t lba_count)
{
	if (!cfg_storage_enabled) return -ENODEV;
	if (unlikely(lba_count == 0)) return 0;
	if (unlikely(src == NULL || block_size == 0)) return -EINVAL;
	if (unlikely(((uintptr_t)src & (block_size - 1)) != 0)) return -EINVAL;

	int rc;

	size_t bytes = (size_t)lba_count * block_size;
	if (unlikely(bytes / block_size != lba_count)) return -EINVAL;

	if (unlikely(!user_dma_range_vtophys_ok((void *)src, bytes))) 
	{
		rc = storage_register_user_dma((void *)src, bytes);
		if (unlikely(rc != 0)) return rc;
	}

	rc = storage_quota_admit(STORAGE_QUOTA_WRITE, lba_count);
	if (unlikely(rc != 0)) return rc;

	struct storage_completion completion = { .thread = thread_self(), .status = -EIO, };
	struct kthread *k = getk();
	struct storage_q *q = &k->storage_q;
	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_write(spdk_namespace, q->spdk_qp_handle, (void *)src, lba, lba_count, seq_status_complete, &completion, 0);
	if (unlikely(rc != 0))
	{
		log_err("storage_write_user_dma(): spdk_nvme_ns_cmd_write failed with rc=%d", rc);
		spin_unlock(&q->lock);
		storage_quota_cancel(STORAGE_QUOTA_WRITE, lba_count);
		putk();
		return -EIO;
	}

	q->outstanding_reqs++;
	thread_park_and_unlock_np(&q->lock);
	return completion.status;
}

struct storage_batch_completion {
	struct thread* thread;
	unsigned int remaining;
	int status;
};
static void batch_status_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct storage_batch_completion *ctx = arg;

	if (spdk_nvme_cpl_is_error(completion)) ctx->status = -EIO;
	barrier();

	if (--ctx->remaining == 0) 
	{
		if (runtime_info && atomic64_read(&runtime_info->spdk_uipi)) thread_ready_head(ctx->thread);
		else thread_ready(ctx->thread);
	}
}
int storage_read_aligned_batch(struct storage_batch_read *reqs, unsigned int nr)
{
	struct storage_batch_completion completion = {};
	struct kthread *k;
	struct storage_q *q;
	unsigned int submitted = 0;
	uint32_t admitted_lbas = 0;
	uint32_t submitted_lbas = 0;
	int rc;

	if (!cfg_storage_enabled) return -ENODEV;
	if (unlikely(nr == 0)) return 0;
	if (unlikely(reqs == NULL)) return -EINVAL;

	for (unsigned int i = 0; i < nr; i++) 
	{
		size_t bytes;

		if (unlikely(reqs[i].lba_count == 0)) return -EINVAL;
		if (unlikely(reqs[i].dest == NULL || block_size == 0)) return -EINVAL;
		if (unlikely(((uintptr_t)reqs[i].dest & (block_size - 1)) != 0)) return -EINVAL;

		bytes = (size_t)reqs[i].lba_count * block_size;
		if (unlikely(bytes / block_size != reqs[i].lba_count)) return -EINVAL;
		if (unlikely(!user_dma_range_vtophys_ok(reqs[i].dest, bytes))) 
		{
			rc = storage_register_user_dma(reqs[i].dest, bytes);
			if (unlikely(rc != 0)) return rc;
		}
		if (unlikely(admitted_lbas + reqs[i].lba_count < admitted_lbas)) return -EINVAL;
		admitted_lbas += reqs[i].lba_count;
	}

	rc = storage_quota_admit(STORAGE_QUOTA_READ, admitted_lbas);
	if (unlikely(rc != 0)) return rc;

	completion.thread = thread_self();
	completion.status = 0;

	k = getk();
	q = &k->storage_q;

	spin_lock(&q->lock);
	for (unsigned int i = 0; i < nr; i++) 
	{
		rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, reqs[i].dest, reqs[i].lba, reqs[i].lba_count, batch_status_complete, &completion, 0);
		if (unlikely(rc != 0)) 
		{
			if (i == 0) 
			{
				spin_unlock(&q->lock);
				storage_quota_cancel(STORAGE_QUOTA_READ, admitted_lbas);
				putk();
				return -EIO;
			}
			completion.status = -EIO;
			break;
		}
		q->outstanding_reqs++;
		submitted++;
		submitted_lbas += reqs[i].lba_count;
	}
	if (unlikely(submitted_lbas < admitted_lbas))
		storage_quota_cancel(STORAGE_QUOTA_READ, admitted_lbas - submitted_lbas);
	if (unlikely(submitted == 0)) 
	{
		spin_unlock(&q->lock);
		putk();
		return -EIO;
	}
	completion.remaining = submitted;
	thread_park_and_unlock_np(&q->lock);
	return completion.status;
}

static int storage_softirq_one(struct storage_q *q)
{
	int ret;

	assert_spin_lock_held(&q->lock);

	ret = spdk_nvme_qpair_process_completions(q->spdk_qp_handle, RUNTIME_RX_BATCH_SIZE);
	q->outstanding_reqs -= ret;
	return ret;
}

void storage_softirq(void *arg)
{
	struct kthread *k = arg;
	struct storage_q *q = &k->storage_q;
	int ret;

	if (!cfg_storage_enabled)
		return;

	while (true) {
		preempt_disable();
		do {
			spin_lock(&q->lock);
			ret = storage_softirq_one(q);
			spin_unlock(&q->lock);
		} while (!preempt_needed() && ret > 0);
		k->storage_busy = false;
		thread_park_and_preempt_enable();
	}
}

/**
 * storage_init_thread - initializes storage (per-thread)
 */
int storage_init_thread(void)
{
	struct kthread *k = myk();
	struct hardware_queue_spec *hs = &iok.threads[kthread_idx(k)].storage_hwq;
	struct storage_q *q = &k->storage_q;
	thread_t *th;

	uint32_t max_xfer_size, entries, depth, *consumer_idx;
	shmptr_t cq_shm;
	struct spdk_nvme_cpl *cpl;
	struct spdk_nvme_io_qpair_opts opts;
	void *qp_handle;

	if (!cfg_storage_enabled)
		return 0;

	th = thread_create(storage_softirq, k);
	if (!th)
		return -ENOMEM;
	// log_info("[storage_init_thread] Created storage softirq thread %p for kthread %d", th, kthread_idx(k));

	k->storage_softirq = th;
	spdk_nvme_ctrlr_get_default_io_qpair_opts(controller, &opts, sizeof(opts));
	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(spdk_namespace);
	entries = (4096 - 1) / max_xfer_size + 2;
	depth = 64;
	if (depth * entries > opts.io_queue_size) {
		log_info("controller IO queue size %u less than required",
			 opts.io_queue_size);
		log_info(
			"Consider using lower queue depth or small IO size because "
			"IO requests may be queued at the NVMe driver.");
	}
	entries += 1;
	if (depth * entries > opts.io_queue_requests)
		opts.io_queue_requests = depth * entries;


	/* Allocate CQ of size io_queue_size * sizeof(struct spdk_nvme_cpl) */
	opts.cq.buffer_size = opts.io_queue_size * sizeof(*cpl);
	cpl = iok_shm_alloc(opts.cq.buffer_size, PGSIZE_4KB, &cq_shm);
	if (!cpl) {
		log_err("could not allocate storage CQ buf in shared mem");
		return -ENOMEM;
	}
	opts.cq.vaddr = cpl;
	qp_handle =
		spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
	if (qp_handle == NULL) {
		log_err("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed");
		return -1;
	}

	nvme_setup_shenango(qp_handle, &consumer_idx,  &k->q_ptrs->storage_tail);

	/* intialize struct storage_q */
	spin_lock_init(&q->lock);
	q->outstanding_reqs = 0;
	q->spdk_qp_handle = qp_handle;
	q->hq.descriptor_table = cpl;
	q->hq.consumer_idx = consumer_idx;
	q->hq.shadow_tail = &k->q_ptrs->storage_tail;
	q->hq.descriptor_log_size = __builtin_ctz(sizeof(*cpl));
	BUILD_ASSERT(is_power_of_two(sizeof(*cpl)));
	q->hq.nr_descriptors = opts.io_queue_size;
	q->hq.parity_byte_offset = offsetof(struct spdk_nvme_cpl, status);
	q->hq.parity_bit_mask = 0x1;

	/* inform iokernel of queue info */
	hs->descriptor_table = cq_shm;
	hs->consumer_idx = ptr_to_shmptr(
		&netcfg.tx_region, &k->q_ptrs->storage_tail, sizeof(uint32_t));
	hs->descriptor_log_size = q->hq.descriptor_log_size;
	hs->nr_descriptors = q->hq.nr_descriptors;
	hs->parity_byte_offset = q->hq.parity_byte_offset;
	hs->parity_bit_mask = q->hq.parity_bit_mask;
	hs->hwq_type = HWQ_SPDK_NVME;

	tcache_init_perthread(storage_buf_tcache,
			      perthread_ptr(storage_buf_pt));

	return 0;
}

/**
 * storage_init - initializes storage
 *
 */
int storage_init(void)
{
	int shm_id, rc;
	struct spdk_env_opts opts;
	void *buf;

	if (!cfg_storage_enabled)
		return 0;

	spdk_env_opts_init(&opts);
	opts.name = "shenango runtime";
	shm_id = rand_crc32c((uintptr_t)myk());
	if (shm_id < 0)
		shm_id = -shm_id;
	opts.shm_id = shm_id;

	if (spdk_env_init(&opts) < 0) {
		log_err("Unable to initialize SPDK env");
		return 1;
	}

	rc = spdk_mem_register(netcfg.tx_region.base, netcfg.tx_region.len);
	if (rc != 0) {
		log_err("storage: failed to register SHM area");
		return 1;
	}

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		log_err("spdk_nvme_probe() failed");
		return 1;
	}

	if (controller == NULL) {
		log_err("no NVMe controllers found");
		return 1;
	}

	buf = mem_map_anom(NULL, REQUEST_BUF_POOL_SZ, PGSIZE_2MB, 0);
	if (buf == MAP_FAILED)
		return -ENOMEM;

	rc = spdk_mem_register(buf, REQUEST_BUF_POOL_SZ);
	if (rc)
		return rc;

	rc = mempool_create(&storage_buf_mp, buf, REQUEST_BUF_POOL_SZ,
			    PGSIZE_2MB, REQUEST_BUF_SZ);
	if (rc)
		return rc;

	storage_buf_tcache = mempool_create_tcache(
		&storage_buf_mp, "storagebufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!storage_buf_tcache)
		return -ENOMEM;

	log_info("SPDK storage init successfully!");
	return 0;
}

static bool __dma_block_transfer(void* buf, uint64_t lba, bool is_write)   // is_write: true 代表写盘(buf -> disk)，false 代表读盘(disk -> buf)
{
    if (!cfg_storage_enabled) 
    {
        log_err("__dma_block_transfer(): storage is not enabled");
        return false;
    }
    if (buf == NULL) 
    {
        log_err("__dma_block_transfer(): buf is NULL");
        return false;
    }

    int quota_rc = storage_quota_admit(is_write ? STORAGE_QUOTA_WRITE : STORAGE_QUOTA_READ, 1);
    if (unlikely(quota_rc != 0)) return false;

    struct kthread   *k = getk();
    struct storage_q *q = &k->storage_q;

    spin_lock(&q->lock);
    
    int rc;
    if (is_write) 
        rc = spdk_nvme_ns_cmd_write(spdk_namespace, q->spdk_qp_handle, buf, lba, 1, seq_complete, thread_self(), 0);
	else 
        rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, buf, lba, 1, seq_complete, thread_self(), 0);
    
    if (unlikely(rc != 0)) 
    {
        log_err("spdk_nvme_ns_cmd_%s failed for lba %lu", is_write ? "write" : "read", lba);
        spin_unlock(&q->lock);
        storage_quota_cancel(is_write ? STORAGE_QUOTA_WRITE : STORAGE_QUOTA_READ, 1);
        putk(); 
        return false;
    }

    q->outstanding_reqs++;
    thread_park_and_unlock_np(&q->lock);
    return true;
}
bool DMA_read_block(void* buf, uint64_t lba)        { return __dma_block_transfer(buf, lba, false); }  // 读盘：将 Disk block 中的数据直接 DMA 到 SPDK buffer 
bool DMA_write_block(const void* buf, uint64_t lba) { return __dma_block_transfer(buf, lba, true);  }  // 写盘：将 SPDK buffer 中的数据直接 DMA 到 Disk block 

struct syncio_ctx {
    volatile bool done;
    int status;
};
static void sync_read_cb(void* arg, const struct spdk_nvme_cpl* cpl)
{
    struct syncio_ctx* ctx = (struct syncio_ctx*)arg;

    if (spdk_nvme_cpl_is_error(cpl)) 
        ctx->status = -EIO;
    else 
        ctx->status = 0;
    
	barrier();
    ctx->done = true;
}

int readObj_sync(void* dest, size_t siz, uint64_t lba, uint32_t lba_count)
{
    if (!cfg_storage_enabled) return -ENODEV;
	
	size_t req_size = lba_count * block_size;
	bool use_thread_cache = req_size <= REQUEST_BUF_SZ;

	int rc = storage_quota_admit(STORAGE_QUOTA_READ, lba_count);
	if (unlikely(rc != 0)) return rc;

	struct kthread* k = getk();
	struct storage_q* q = &k->storage_q;

    void *spdk_payload;
	if (likely(use_thread_cache)) 
        spdk_payload = tcache_alloc(perthread_ptr(storage_buf_pt));
	else  
        spdk_payload = spdk_zmalloc(req_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	if (unlikely(spdk_payload == NULL)) 
    {
		storage_quota_cancel(STORAGE_QUOTA_READ, lba_count);
		preempt_enable();
		return -ENOMEM;
	}

    struct syncio_ctx ctx = { .done = false, .status = 0 };

	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_read(spdk_namespace, q->spdk_qp_handle, spdk_payload, lba, lba_count, sync_read_cb, &ctx, 0);
    if (unlikely(rc != 0)) 
    {
		spin_unlock(&q->lock);
		storage_quota_cancel(STORAGE_QUOTA_READ, lba_count);
		rc = -EIO;
		goto done_np;
	}

    while (!ctx.done) 
    {
        spdk_nvme_qpair_process_completions(q->spdk_qp_handle, 0);
        cpu_relax();
    }
    
    spin_unlock(&q->lock);
    memcpy(dest, spdk_payload, siz);
    rc = ctx.status;

done_np:
	if (likely(use_thread_cache))
		tcache_free(perthread_ptr(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);

	preempt_enable();
	return rc;
}

typedef struct BlockEntry
{
    uint64_t    lba;
    char*       data;
    bool        valid;    // 是否从盘上读取了数据
    bool        dirty;    // 是否需要写回磁盘
    spinlock_t  mtx;
} BlockEntry;
struct block_sgl_ctx 
{
    void*        *blockentries;     // 若干个 BlockEntry 地址构成的数组
    uint32_t     num_blocks;        // 一共多少个 block
    uint32_t     block_size;        // = BLOCK_SIZE
    uint32_t     cur_index;         // 当前走到第几个 block
};
struct vectorIO_ctx 
{
    struct block_sgl_ctx sgl;
	thread_t *th;
	int status;
};

void block_reset_sgl(void *arg, uint32_t offset)
{
    struct vectorIO_ctx *ctx = (struct vectorIO_ctx *)arg;
    ctx->sgl.cur_index = offset / ctx->sgl.block_size;
}
int block_next_sge(void *arg, void **address, uint32_t *length)
{
    struct vectorIO_ctx *ctx = (struct vectorIO_ctx *)arg;
    struct block_sgl_ctx *sgl = &ctx->sgl;

    if (sgl->cur_index >= sgl->num_blocks) return -1;  // 没有更多 buffer 了

    BlockEntry* blockentry = (BlockEntry*)sgl->blockentries[sgl->cur_index++];
    *address = blockentry->data;
    *length  = sgl->block_size;
    return 0;
}

void vectorIO_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct vectorIO_ctx *ctx = (struct vectorIO_ctx *)arg;
	ctx->status = spdk_nvme_cpl_is_error(cpl) ? -EIO : 0;
	barrier();
	if (runtime_info && atomic64_read(&runtime_info->spdk_uipi))
		thread_ready_head(ctx->th);
	else
		thread_ready(ctx->th);
}

int read_blocks_from_disk(uint64_t lba_start, uint32_t lba_count, void* blockentries[])
{
	// log_info("read_blocks_from_disk() START: gonna read %u blocks from LBA %lu to blockcache", lba_count, lba_start);

	if (!cfg_storage_enabled) return -1;
	if (unlikely(lba_count == 0)) return -1;

	struct vectorIO_ctx ctx = {};
	ctx.th = thread_self();
	ctx.sgl.blockentries = blockentries;
	ctx.sgl.num_blocks = lba_count;
	ctx.sgl.block_size = block_size;
	ctx.sgl.cur_index = 0;
	ctx.status = -EIO;

	int rc = storage_quota_admit(STORAGE_QUOTA_READ, lba_count);
	if (unlikely(rc != 0)) return rc;

	struct kthread   *k = getk();
    struct storage_q *q = &k->storage_q;
	spin_lock(&q->lock);
	rc = spdk_nvme_ns_cmd_readv(spdk_namespace, q->spdk_qp_handle, lba_start, lba_count, vectorIO_complete, &ctx, 0, block_reset_sgl, block_next_sge);
	if (unlikely(rc != 0))
	{
		spin_unlock(&q->lock);
		storage_quota_cancel(STORAGE_QUOTA_READ, lba_count);
		putk();
		return -1;
	}
	q->outstanding_reqs++;
	thread_park_and_unlock_np(&q->lock);
	preempt_disable();

	// log_info("read_blocks_from_disk() DONE: have read %u blocks from LBA %lu to blockcache", lba_count, lba_start);
	int status = ctx.status;
	preempt_enable();
	return status;
}

int write_blocks_to_disk(uint64_t lba_start, uint32_t lba_count, void* blockentries[])
{
    if (!cfg_storage_enabled) return -1;
    if (unlikely(lba_count == 0)) return -1;

    struct vectorIO_ctx ctx = {};
    ctx.th = thread_self();
    ctx.sgl.blockentries = blockentries;
    ctx.sgl.num_blocks = lba_count;
    ctx.sgl.block_size = block_size;
	ctx.sgl.cur_index = 0;
	ctx.status = -EIO;

	int rc = storage_quota_admit(STORAGE_QUOTA_WRITE, lba_count);
	if (unlikely(rc != 0)) return rc;

    struct kthread *k = getk();
    struct storage_q *q = &k->storage_q;
    spin_lock(&q->lock);

    rc = spdk_nvme_ns_cmd_writev(spdk_namespace, q->spdk_qp_handle, lba_start, lba_count, vectorIO_complete, &ctx, 0, block_reset_sgl, block_next_sge);
    if (unlikely(rc != 0)) 
    {
        spin_unlock(&q->lock);
		storage_quota_cancel(STORAGE_QUOTA_WRITE, lba_count);
        putk();
        return -1;
    }

    q->outstanding_reqs++;
    thread_park_and_unlock_np(&q->lock);
	preempt_disable();

	int status = ctx.status;
	preempt_enable();
    return status;
}

#else
int storage_write(const void *payload, uint64_t lba, uint32_t lba_count)
{
	return -ENODEV;
}

int storage_read(void *dest, uint64_t lba, uint32_t lba_count)
{
	return -ENODEV;
}

int storage_read_aligned(void *dest, uint64_t lba, uint32_t lba_count)
{
	return -ENODEV;
}

int storage_read_aligned_batch(struct storage_batch_read *reqs, unsigned int nr)
{
	return -ENODEV;
}

int storage_init(void)
{
	return 0;
}

int storage_init_thread(void)
{
	return 0;
}

#endif
