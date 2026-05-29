#pragma once
#include <sys/types.h>
#include <base/lock.h>
#include <stddef.h>

// #define SCALE 1000LL 
// #define TO_STORE(val)   ((int64_t)((val) * SCALE))      // 存储时扩大 SCALE 倍，以保留小数精度
// #define TO_USE(val)     ((double)(val) / SCALE)         // 使用时还原


#define REFILL_TIME       0.1                     // 时间窗口：0.1s
#define TO_US             1000000ULL              // 秒转微秒

#define HARDWARE_IOPS     1721252                 // 测得 IOPS
#define HARDWARE_BW       7037 * 1024 * 1024ULL   // 测得 带宽 (Byte/s)

// #define DEFAULT_IOPS      100                     // 初始时给每个 task 分配的 IOPS    quota
// #define DEFAULT_BW        1024 * 1024             // 初始时给每个 task 分配的 IO size quota
#define DEFAULT_RATIO     0.001

#define GLOBALPOOL_RATIO  0.2                     // Global Pool 预留比例 (20%)
#define TOTAL_IOPS_ALLOCABLE   (HARDWARE_IOPS * REFILL_TIME * (1.0 - GLOBALPOOL_RATIO))    // 可分配的IOPS
#define TOTAL_BW_ALLOCABLE     (HARDWARE_BW   * REFILL_TIME * (1.0 - GLOBALPOOL_RATIO))    // 可分配的带宽

#define ALPHA             0.4                     // EWMA 历史权重
#define R1                0.6                     // 缩容阈值
#define R2                0.85                    // 扩容阈值
#define G_MAX             1                       // 最大扩容系数 (+100%)
#define G_MIN             -0.5                    // 最大缩容系数 (-50%)
#define THETA             0.2                     // 低优先级最小保底比例
#define VIP_RSV           0.1                     // Global Pool 借贷时的 VIP 水位线

#define BORROW_BATCH      0.1                     // 每次借贷的批量比例 (10%)

#define QUOTA_DEFAULT_REFILL_US ((uint64_t)(REFILL_TIME * TO_US))

typedef struct {
    volatile int64_t bucket;    // runtime 从该共享桶批量领取本地 token，避免每次 IO 原子扣减
    volatile int64_t quota;     // 当前分配配额
    volatile uint64_t epoch;    // iokernel 每次重新分配配额时递增，runtime 据此刷新本地 token
    volatile int64_t demand;    // 周期内申请使用量
    
    /* Admin 专用字段 (仅 Admin 进程读写) */
    double ewma;                // 历史加权平均需求
    double target;              // 下一周期理想配额
    int    init;                // 是否已初始化
} QuotaDim;

typedef struct {
    int         priority;  // TODO：删掉
    atomic64_t   enabled;  // runtime 控制开关；为 0 时 iokernel 不做 quota refill
    spinlock_t   lock;     // 保护 iokernel refill 与 runtime 批量取 token 的共享桶状态
    QuotaDim    iops;
    QuotaDim    bytes;
    atomic64_t   wake_epoch; // iokernel 通知 runtime 有新的 quota epoch，可重新检查 quota waiters
} QuotaInfo;
// static int iops_offset  = offsetof(QuotaInfo, iops);
// static int bytes_offset = offsetof(QuotaInfo, bytes);

typedef struct {
	spinlock_t       l;    
    volatile int64_t iops;
    volatile int64_t bytes;
} GlobalQuotaPool;    // 某个时间窗口内 Global Quota Pool 中的余额

void initGlobalQuotaPool(GlobalQuotaPool* gp);
void refillQuota(void);
bool ifQuotaPermit(uint64_t IOsize);
