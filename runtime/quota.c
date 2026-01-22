#include <iokernel/quota.h>
#include "defs.h"

// bool atomic_conditional_decrement(volatile uint64_t *ptr, uint64_t amount) 
// {
//     uint64_t old_val = __atomic_load_n(ptr, __ATOMIC_RELAXED);
//     while (true) 
//     {
//         if (old_val < amount) return false;
//         uint64_t new_val = old_val - amount;
//         if (__atomic_compare_exchange_n(ptr, &old_val, new_val, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return true;
//     }
// }

static bool try_consume(QuotaInfo *Quota, int64_t cost_iops, int64_t cost_bytes) 
{
    // 先粗略看一眼，如果不够，直接跳过，减少原子写竞争
    int64_t peek_iops  = __atomic_load_n(&Quota->iops.bucket,  __ATOMIC_RELAXED);
    int64_t peek_bytes = __atomic_load_n(&Quota->bytes.bucket, __ATOMIC_RELAXED);
    if (peek_iops < cost_iops || peek_bytes < cost_bytes) return false;

    /* 乐观扣除 -> 检查 -> 失败则回滚 */

    // 尝试扣除 IOPS
    int64_t old_iops = __atomic_fetch_sub(&Quota->iops.bucket, cost_iops, __ATOMIC_SEQ_CST);
    if (old_iops < cost_iops)   // IOPS 不足，回滚
    {
        __atomic_fetch_add(&Quota->iops.bucket, cost_iops, __ATOMIC_SEQ_CST);
        return false;
    }

    // 尝试扣除 Bytes
    int64_t old_bytes = __atomic_fetch_sub(&Quota->bytes.bucket, cost_bytes, __ATOMIC_SEQ_CST);
    if (old_bytes < cost_bytes) // Bytes 不足
    {
        __atomic_fetch_add(&Quota->bytes.bucket, cost_bytes, __ATOMIC_SEQ_CST);  // 回滚 Bytes
        __atomic_fetch_add(&Quota->iops.bucket,  cost_iops,  __ATOMIC_SEQ_CST);  // 并回滚刚才扣掉的 IOPS
        return false;
    }

    // 两个都扣除成功
    return true;
}

static bool try_borrow_global(QuotaInfo *Quota, int64_t cost_iops, int64_t cost_bytes)
{
    int64_t cur_iops  = __atomic_load_n(&Quota->iops.bucket,  __ATOMIC_RELAXED);
    int64_t cur_bytes = __atomic_load_n(&Quota->bytes.bucket, __ATOMIC_RELAXED);
    log_info("try_borrow_global: current bucket iops = %ld, bytes = %ld", cur_iops, cur_bytes);
    if (cur_iops  < 0) cur_iops = 0;    // 如果因并发回滚导致为负，视为 0 处理
    if (cur_bytes < 0) cur_bytes = 0;

    // 计算需求缺口
    int64_t need_iops  = MAX(cost_iops  - cur_iops,  0);
    int64_t need_bytes = MAX(cost_bytes - cur_bytes, 0);
    log_info("try_borrow_global: need to borrow iops = %ld, bytes = %ld", need_iops, need_bytes);
    if (need_iops == 0 && need_bytes == 0) return true;  // 不需要借贷
    

    log_info("here1");
    GlobalQuotaPool* global_pool = &iok.iok_info->global_pool;
    log_info("here2");
    spin_lock(&global_pool->l);
    log_info("here3");
    
    int64_t g_iops  = __atomic_load_n(&global_pool->iops,  __ATOMIC_RELAXED);
    int64_t g_bytes = __atomic_load_n(&global_pool->bytes, __ATOMIC_RELAXED);
    log_info("try_borrow_global: current global pool iops = %ld, bytes = %ld", g_iops, g_bytes);

    if (g_iops < need_iops || g_bytes < need_bytes) 
    {
        spin_unlock(&global_pool->l);
        return false;
    }
    if (cfg_prio_is_lc == 0 && (g_iops < HARDWARE_IOPS * REFILL_TIME * VIP_RSV || g_bytes < HARDWARE_BW * REFILL_TIME * VIP_RSV))   // 权限检查 (VIP 水位)
    {
        spin_unlock(&global_pool->l);
        return false;
    } 

    // 计算借贷量 (Quota 的 10% 或 刚好够用)
    int64_t borrow_iops = need_iops;
    if (g_iops >= __atomic_load_n(&Quota->iops.quota,  __ATOMIC_RELAXED) * BORROW_BATCH)  borrow_iops  = MAX(need_iops,  __atomic_load_n(&Quota->iops.quota,  __ATOMIC_RELAXED) * BORROW_BATCH);
    int64_t borrow_bytes = need_bytes;
    if (g_bytes >= __atomic_load_n(&Quota->bytes.quota, __ATOMIC_RELAXED) * BORROW_BATCH) borrow_bytes = MAX(need_bytes, __atomic_load_n(&Quota->bytes.quota, __ATOMIC_RELAXED) * BORROW_BATCH);
    
    log_info("try_borrow_global: borrowing iops = %ld, bytes = %ld", borrow_iops, borrow_bytes);
    
    // 执行借贷
    __atomic_store_n(&global_pool->iops,  g_iops - borrow_iops,   __ATOMIC_RELAXED);
    __atomic_store_n(&global_pool->bytes, g_bytes - borrow_bytes, __ATOMIC_RELAXED);    
    __atomic_fetch_add(&Quota->iops.bucket,  borrow_iops,  __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&Quota->bytes.bucket, borrow_bytes, __ATOMIC_SEQ_CST);
    
    spin_unlock(&global_pool->l); 

    return true;
}

bool ifQuotaPermit(uint64_t IOsize)
{
    QuotaInfo* Q = &runtime_info->Q;

    bool success = false;

    if (try_consume(Q, 1, IOsize)) 
    {
        log_info("ifQuotaPermit: direct consume succeeded!");
        success = true; 
    }
    else 
    {
        log_info("ifQuotaPermit: direct consume failed, trying to borrow from global pool...");
        if (try_borrow_global(Q, 1, IOsize))
        {
            log_info("ifQuotaPermit: borrow from global pool succeeded!");
            if (try_consume(Q, 1, IOsize)) 
            {
                log_info("ifQuotaPermit: consume after borrow succeeded!");
                success = true;  // 借到了，再扣一次
            }
        }
        else
        {
            log_info("ifQuotaPermit: borrow from global pool failed!");
        }
    }

	return success;
}

// void perform_io(Task *t, uint64_t size) 
// {
//     __atomic_fetch_add(&Q->iops.demand, 1,       __ATOMIC_SEQ_CST);
//     __atomic_fetch_add(&Q->bytes.demand, IOsize, __ATOMIC_SEQ_CST);
//     while (!ifQuotaPermit(size)) thread_yield(); 
//     // Do IO...
// }