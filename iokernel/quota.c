#include <base/log.h>
#include "defs.h"
#include <iokernel/quota.h>
#include <math.h>

static GlobalQuotaPool* global_pool;

void initGlobalQuotaPool(GlobalQuotaPool* gp)
{
    spin_lock_init(&gp->l);
    gp->iops  = HARDWARE_IOPS * REFILL_TIME;
    gp->bytes = HARDWARE_BW   * REFILL_TIME;

    global_pool = gp;
}

static double my_gamma(double R)   // 计算动态扩缩容系数 Gamma
{
    if (R > R2)       // 激进扩容: G_max * ((R - R2)/(1 - R2))^2
    {
        double x = (R - R2) / (1.0 - R2);
        if (x > 1.0) x = 1.0;
        return G_MAX * x * x;
    }
    else if (R < R1)  // 缓慢缩容: G_min * ((R1 - R)/R1)^2
    {
        double x = (R1 - R) / R1;
        if (x > 1.0) x = 1.0;
        return G_MIN * x * x;
    }
        
    return 0.0;       // 稳定区
}

double calc_target(int type, QuotaInfo* Quota)  // 计算 task 在下一周期的理想配额，type 为 0 表示 IOPS, 1 表示 Byte
{
    QuotaDim* q         = (type == 0) ? &Quota->iops         : &Quota->bytes;
    double MaxAvailable = (type == 0) ? TOTAL_IOPS_ALLOCABLE : TOTAL_BW_ALLOCABLE;

    if (q->init == 0) 
    {
        q->target = q->ewma = MaxAvailable * DEFAULT_RATIO;  // 初始值
        q->init = 1;
    }
    else
    {
        int64_t oldQuota = __atomic_load_n(&q->quota, __ATOMIC_RELAXED);        // 上轮分配的 quota 值
        int64_t demand = __atomic_exchange_n(&q->demand, 0, __ATOMIC_SEQ_CST);  // 原子读取并清零
        q->ewma = (1.0 - ALPHA) * demand + ALPHA * q->ewma;                     // EWMA 更新
        if (oldQuota <= 0) 
            q->target = MAX(q->ewma, MaxAvailable * DEFAULT_RATIO); // 或者 MAX(ewma, 1)
        else 
        {
            double R = q->ewma / oldQuota;
            q->target = q->ewma * (1.0 + my_gamma(R));
        }
    }

    return q->target;
}

void refillQuota(void)
{
    // log_info("Refilling Quotas...");
    double total_D_op = 0, D_H_op = 0, D_L_op = 0;
    double total_D_bw = 0, D_H_bw = 0, D_L_bw = 0;

    struct proc *p;
    for (int i = 0; i < dp.nr_clients; i++)   // 遍历所有 task
	{
        p = dp.clients[i];
        if (!p->has_storage) continue;
		if (unlikely(!p->runtime_info)) continue;

        QuotaInfo* Quota = &p->runtime_info->Q;
        double res1 = calc_target(0, Quota), res2 = calc_target(1, Quota);
        if (p->sched_cfg.priority == 1) 
        {
            D_H_op += res1;
            D_H_bw += res2;
        }
        else
        {
            D_L_op += res1;
            D_L_bw += res2;
        }                    
    }

    total_D_op = D_H_op + D_L_op;
    double L_op = total_D_op / TOTAL_IOPS_ALLOCABLE;
    double Q_H_op, Q_L_op;
    if (L_op > 1.0) 
    {
        Q_H_op = MIN(D_H_op, TOTAL_IOPS_ALLOCABLE - MIN(D_L_op, TOTAL_IOPS_ALLOCABLE * THETA));
        Q_L_op = MIN(D_L_op, TOTAL_IOPS_ALLOCABLE - Q_H_op);
    }
    
    total_D_bw = D_H_bw + D_L_bw;
    double L_bw = total_D_bw / TOTAL_BW_ALLOCABLE;
    double Q_H_bw, Q_L_bw;
    if (L_bw > 1.0) 
    {
        Q_H_bw = MIN(D_H_bw, TOTAL_BW_ALLOCABLE - MIN(D_L_bw, TOTAL_BW_ALLOCABLE * THETA));
        Q_L_bw = MIN(D_L_bw, TOTAL_BW_ALLOCABLE - Q_H_bw);
    }

    double final_sum_op = 0, final_sum_bw = 0;
    for (int i = 0; i < dp.nr_clients; i++)   // 遍历所有 task
	{
        p = dp.clients[i];
        if (!p->has_storage) continue;
		if (unlikely(!p->runtime_info)) continue;
        
        QuotaInfo* Quota = &p->runtime_info->Q;

        double assign_op = 0;
        if (L_op <= 1.0) assign_op = ceil(Quota->iops.target); 
        else 
        {
            if (p->sched_cfg.priority == 1) assign_op = (D_H_op > 0) ? ceil((Quota->iops.target / D_H_op) * Q_H_op) : 0;
            else                            assign_op = (D_L_op > 0) ? ceil((Quota->iops.target / D_L_op) * Q_L_op) : 0;
        }
        __atomic_store_n(&Quota->iops.quota,  (int64_t)assign_op, __ATOMIC_SEQ_CST);
        __atomic_store_n(&Quota->iops.bucket, (int64_t)assign_op, __ATOMIC_SEQ_CST);
        final_sum_op += assign_op;

        double assign_bw = 0;
        if (L_bw <= 1.0) assign_bw = ceil(Quota->bytes.target);
        else 
        {
            if (p->sched_cfg.priority == 1) assign_bw = (D_H_bw > 0) ? ceil((Quota->bytes.target / D_H_bw) * Q_H_bw) : 0;
            else                            assign_bw = (D_L_bw > 0) ? ceil((Quota->bytes.target / D_L_bw) * Q_L_bw) : 0;
        }
        __atomic_store_n(&Quota->bytes.quota,  (int64_t)assign_bw, __ATOMIC_SEQ_CST);
        __atomic_store_n(&Quota->bytes.bucket, (int64_t)assign_bw, __ATOMIC_SEQ_CST);
        final_sum_bw += assign_bw;
    }

    double g_ops_val = HARDWARE_IOPS * REFILL_TIME - final_sum_op, g_bw_val = HARDWARE_BW * REFILL_TIME - final_sum_bw;

    spin_lock(&global_pool->l);
    __atomic_store_n(&global_pool->iops,  (int64_t)g_ops_val, __ATOMIC_RELAXED);
    __atomic_store_n(&global_pool->bytes, (int64_t)g_bw_val,  __ATOMIC_RELAXED);
    spin_unlock(&global_pool->l);
    if (final_sum_op > 0 || final_sum_bw > 0) log_info("Refill done: total assigned IOPS quota = %.0f, BW quota = %.0f", final_sum_op, final_sum_bw);
}