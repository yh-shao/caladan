#include <errno.h>
#include <iokernel/quota.h>
#include <base/time.h>
#include <runtime/timer.h>
#include "defs.h"

bool cfg_storage_quota_enabled;
bool cfg_storage_quota_borrow_global_enabled = true;
uint64_t cfg_storage_quota_refill_us = QUOTA_DEFAULT_REFILL_US;
uint64_t cfg_storage_quota_iops = HARDWARE_IOPS;
uint64_t cfg_storage_quota_bytes = HARDWARE_BW;

#define QUOTA_BORROW_MIN_IOPS  1024
#define QUOTA_BORROW_MIN_BYTES (4ULL * 1024 * 1024)

struct quota_bucket {
    int64_t tokens;
    int64_t quota;
    uint64_t epoch;
};

struct storage_quota_local {
    struct quota_bucket iops;
    struct quota_bucket bytes;
    uint64_t wake_epoch;
    bool initialized;
};

static DEFINE_PERTHREAD(struct storage_quota_local, storage_quota_local);

static inline int64_t quota_max_i64(int64_t a, int64_t b) { return a > b ? a : b; }

static inline bool quota_runtime_ready(void)
{
    return cfg_storage_quota_enabled && runtime_info != NULL;
}

static inline void quota_refresh_one(struct quota_bucket *local, QuotaDim *shared)
{
    uint64_t epoch = __atomic_load_n(&shared->epoch, __ATOMIC_ACQUIRE);
    if (likely(local->epoch == epoch)) return;

    int64_t quota = __atomic_load_n(&shared->quota, __ATOMIC_RELAXED);
    if (quota < 0) quota = 0;

    local->quota = quota;
    local->tokens = 0;
    local->epoch = epoch;
}

static inline void quota_refresh_local_locked(void)
{
    struct storage_quota_local *local_quota = perthread_ptr(storage_quota_local);
    QuotaInfo *q = &runtime_info->Q;

    quota_refresh_one(&local_quota->iops, &q->iops);
    quota_refresh_one(&local_quota->bytes, &q->bytes);
    local_quota->wake_epoch = atomic64_read(&q->wake_epoch);
    local_quota->initialized = true;
}

static inline void quota_refresh_local(void)
{
    QuotaInfo *q = &runtime_info->Q;

    spin_lock(&q->lock);
    quota_refresh_local_locked();
    spin_unlock(&q->lock);
}

void storage_quota_init_runtime(void)
{
    memset(perthread_ptr(storage_quota_local), 0, sizeof(struct storage_quota_local));
    if (!runtime_info) return;

    QuotaInfo *q = &runtime_info->Q;
    memset(q, 0, sizeof(*q));
    spin_lock_init(&q->lock);
    atomic64_write(&q->enabled, cfg_storage_quota_enabled ? 1 : 0);

    if (!cfg_storage_quota_enabled) return;

    int64_t iops_quota = (int64_t)((cfg_storage_quota_iops * QUOTA_DEFAULT_REFILL_US + TO_US - 1) / TO_US);
    int64_t bytes_quota = (int64_t)((cfg_storage_quota_bytes * QUOTA_DEFAULT_REFILL_US + TO_US - 1) / TO_US);
    if (iops_quota <= 0) iops_quota = 1;
    if (bytes_quota <= 0) bytes_quota = 1;

    __atomic_store_n(&q->iops.bucket, iops_quota, __ATOMIC_RELEASE);
    __atomic_store_n(&q->bytes.bucket, bytes_quota, __ATOMIC_RELEASE);
    __atomic_store_n(&q->iops.quota, iops_quota, __ATOMIC_RELEASE);
    __atomic_store_n(&q->bytes.quota, bytes_quota, __ATOMIC_RELEASE);
    __atomic_store_n(&q->iops.epoch, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&q->bytes.epoch, 1, __ATOMIC_RELEASE);
    atomic64_write(&q->wake_epoch, 1);
}

void storage_quota_account(uint32_t iops, uint64_t bytes)
{
    if (!quota_runtime_ready()) return;
    __atomic_fetch_add(&runtime_info->Q.iops.demand, (int64_t)iops, __ATOMIC_RELAXED);
    __atomic_fetch_add(&runtime_info->Q.bytes.demand, (int64_t)bytes, __ATOMIC_RELAXED);
}

static bool quota_consume_local(uint32_t iops, uint64_t bytes)
{
    struct storage_quota_local *local_quota = perthread_ptr(storage_quota_local);

    if (!local_quota->initialized ||
        local_quota->iops.epoch != __atomic_load_n(&runtime_info->Q.iops.epoch, __ATOMIC_ACQUIRE) ||
        local_quota->bytes.epoch != __atomic_load_n(&runtime_info->Q.bytes.epoch, __ATOMIC_ACQUIRE))
        quota_refresh_local();

    int64_t cost_iops = (int64_t)iops;
    int64_t cost_bytes = (int64_t)bytes;
    if (local_quota->iops.tokens < cost_iops || local_quota->bytes.tokens < cost_bytes)
        return false;

    local_quota->iops.tokens -= cost_iops;
    local_quota->bytes.tokens -= cost_bytes;
    return true;
}

static bool quota_consume_local_ready(struct storage_quota_local *local_quota,
                                      uint32_t iops, uint64_t bytes)
{
    int64_t cost_iops = (int64_t)iops;
    int64_t cost_bytes = (int64_t)bytes;

    if (local_quota->iops.tokens < cost_iops || local_quota->bytes.tokens < cost_bytes)
        return false;

    local_quota->iops.tokens -= cost_iops;
    local_quota->bytes.tokens -= cost_bytes;
    return true;
}

static int64_t quota_batch_amount(int64_t need, int64_t quota, int64_t floor)
{
    if (need <= 0) return 0;

    int64_t batch = need;
    if (quota > 0)
    {
        int64_t quota_batch = (int64_t)(quota * BORROW_BATCH);
        if (quota_batch > batch) batch = quota_batch;
    }
    if (batch < floor) batch = floor;
    if (batch < need) batch = need;
    return batch;
}

static bool quota_try_pull_shared(uint32_t iops, uint64_t bytes)
{
    struct storage_quota_local *local_quota = perthread_ptr(storage_quota_local);
    QuotaInfo *q = &runtime_info->Q;

    spin_lock(&q->lock);
    quota_refresh_local_locked();
    if (quota_consume_local_ready(local_quota, iops, bytes))
    {
        spin_unlock(&q->lock);
        return true;
    }

    int64_t need_iops = quota_max_i64((int64_t)iops - local_quota->iops.tokens, 0);
    int64_t need_bytes = quota_max_i64((int64_t)bytes - local_quota->bytes.tokens, 0);
    if (need_iops == 0 && need_bytes == 0)
    {
        spin_unlock(&q->lock);
        return true;
    }

    int64_t pull_iops = quota_batch_amount(need_iops, local_quota->iops.quota, 0);
    int64_t pull_bytes = quota_batch_amount(need_bytes, local_quota->bytes.quota, 0);

    int64_t old_iops = __atomic_load_n(&q->iops.bucket, __ATOMIC_RELAXED);
    int64_t old_bytes = __atomic_load_n(&q->bytes.bucket, __ATOMIC_RELAXED);
    if (old_iops < need_iops || old_bytes < need_bytes)
    {
        spin_unlock(&q->lock);
        return false;
    }

    if (pull_iops > old_iops) pull_iops = old_iops;
    if (pull_bytes > old_bytes) pull_bytes = old_bytes;
    __atomic_store_n(&q->iops.bucket, old_iops - pull_iops, __ATOMIC_RELEASE);
    __atomic_store_n(&q->bytes.bucket, old_bytes - pull_bytes, __ATOMIC_RELEASE);

    local_quota->iops.tokens += pull_iops;
    local_quota->bytes.tokens += pull_bytes;
    bool ok = quota_consume_local_ready(local_quota, iops, bytes);
    spin_unlock(&q->lock);
    return ok;
}

static bool quota_try_borrow_global(uint32_t iops, uint64_t bytes)
{
    if (!cfg_storage_quota_borrow_global_enabled) return false;
    if (!iok.iok_info) return false;

    QuotaInfo *q = &runtime_info->Q;
    spin_lock(&q->lock);
    quota_refresh_local_locked();
    struct storage_quota_local *local_quota = perthread_ptr(storage_quota_local);
    if (quota_consume_local_ready(local_quota, iops, bytes))
    {
        spin_unlock(&q->lock);
        return true;
    }

    int64_t need_iops = quota_max_i64((int64_t)iops - local_quota->iops.tokens, 0);
    int64_t need_bytes = quota_max_i64((int64_t)bytes - local_quota->bytes.tokens, 0);
    if (need_iops == 0 && need_bytes == 0)
    {
        spin_unlock(&q->lock);
        return true;
    }

    int64_t borrow_iops = quota_batch_amount(need_iops, local_quota->iops.quota, QUOTA_BORROW_MIN_IOPS);
    int64_t borrow_bytes = quota_batch_amount(need_bytes, local_quota->bytes.quota, QUOTA_BORROW_MIN_BYTES);

    GlobalQuotaPool *global = (GlobalQuotaPool *)&iok.iok_info->global_pool;
    spin_lock(&global->l);

    int64_t g_iops = __atomic_load_n(&global->iops, __ATOMIC_RELAXED);
    int64_t g_bytes = __atomic_load_n(&global->bytes, __ATOMIC_RELAXED);
    int64_t reserve_iops = 0, reserve_bytes = 0;

    if (!cfg_prio_is_lc)
    {
        reserve_iops = (int64_t)(cfg_storage_quota_iops * VIP_RSV * QUOTA_DEFAULT_REFILL_US / TO_US);
        reserve_bytes = (int64_t)(cfg_storage_quota_bytes * VIP_RSV * QUOTA_DEFAULT_REFILL_US / TO_US);
    }

    int64_t cap_iops = g_iops - reserve_iops;
    int64_t cap_bytes = g_bytes - reserve_bytes;
    if ((need_iops > 0 && cap_iops < need_iops) ||
        (need_bytes > 0 && cap_bytes < need_bytes))
    {
        spin_unlock(&global->l);
        spin_unlock(&q->lock);
        return false;
    }
    if (borrow_iops > 0 && borrow_iops > cap_iops) borrow_iops = cap_iops;
    if (borrow_bytes > 0 && borrow_bytes > cap_bytes) borrow_bytes = cap_bytes;

    __atomic_store_n(&global->iops, g_iops - borrow_iops, __ATOMIC_RELAXED);
    __atomic_store_n(&global->bytes, g_bytes - borrow_bytes, __ATOMIC_RELAXED);
    spin_unlock(&global->l);

    local_quota->iops.tokens += borrow_iops;
    local_quota->bytes.tokens += borrow_bytes;
    bool ok = quota_consume_local_ready(local_quota, iops, bytes);
    spin_unlock(&q->lock);
    return ok;
}

static bool quota_try_acquire_tokens(uint32_t iops, uint64_t bytes)
{
    if (quota_consume_local(iops, bytes)) return true;
    if (quota_try_pull_shared(iops, bytes)) return true;
    if (quota_try_borrow_global(iops, bytes)) return true;
    return false;
}

bool storage_quota_try_acquire(uint32_t iops, uint64_t bytes)
{
    if (!quota_runtime_ready()) return true;
    if (unlikely(bytes > (uint64_t)INT64_MAX)) return false;

    storage_quota_account(iops, bytes);
    preempt_disable();
    bool ok = quota_try_acquire_tokens(iops, bytes);
    preempt_enable();
    return ok;
}

void storage_quota_refund(uint32_t iops, uint64_t bytes)
{
    if (!quota_runtime_ready() || unlikely(bytes > (uint64_t)INT64_MAX)) return;

    preempt_disable();
    QuotaInfo *q = &runtime_info->Q;
    spin_lock(&q->lock);
    quota_refresh_local_locked();
    struct storage_quota_local *local_quota = perthread_ptr(storage_quota_local);

    if (local_quota->iops.tokens <= INT64_MAX - (int64_t)iops)
        local_quota->iops.tokens += (int64_t)iops;
    else
        local_quota->iops.tokens = INT64_MAX;

    if (local_quota->bytes.tokens <= INT64_MAX - (int64_t)bytes)
        local_quota->bytes.tokens += (int64_t)bytes;
    else
        local_quota->bytes.tokens = INT64_MAX;
    spin_unlock(&q->lock);
    preempt_enable();
}

int storage_quota_wait(uint32_t iops, uint64_t bytes)
{
    if (!quota_runtime_ready()) return 0;
    if (unlikely(bytes > (uint64_t)INT64_MAX)) return -EOVERFLOW;
    storage_quota_account(iops, bytes);
    uint64_t sleep_us = cfg_storage_quota_refill_us ? cfg_storage_quota_refill_us : QUOTA_DEFAULT_REFILL_US;

    for (;;)
    {
        preempt_disable();
        bool ok = quota_try_acquire_tokens(iops, bytes);
        preempt_enable();
        if (ok) return 0;

        timer_sleep(sleep_us);
    }
}

bool ifQuotaPermit(uint64_t IOsize)
{
    return storage_quota_try_acquire(1, IOsize);
}
