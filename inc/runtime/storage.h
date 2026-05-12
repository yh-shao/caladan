/*
 * storage.h - Storage
 */

#pragma once

#include <base/stddef.h>

extern int storage_write(const void* payload, uint64_t lba, uint32_t lba_count);
extern int storage_write_obj(const void* obj, size_t siz, uint64_t lba_start, off_t oft);
extern int storage_write_obj_no_rmw(const void* obj, size_t siz, uint64_t lba_start, off_t oft);

extern int storage_read(void* dest, uint64_t lba, uint32_t lba_count);
extern int storage_read_obj(void* obj, size_t siz, uint64_t lba_start, off_t oft);
extern int storage_prepare_user_dma(void* buf, size_t len);                            // 提前把用户 buffer 变成 SPDK/NVMe 可 DMA 的内存
extern int storage_read_aligned(void* dest, uint64_t lba, uint32_t lba_count);         // 直接用用户 buffer 作为 spdk_nvme_ns_cmd_read() 的 payload，直接把盘上数据读取到 user buffer 里
extern int storage_write_user_dma(const void* src, uint64_t lba, uint32_t lba_count);  // 直接把用户 buffer 作为 spdk_nvme_ns_cmd_write() 的 payload，直接把用户 buffer 里的数据写到盘上

bool DMA_read_block(void* buf, uint64_t lba);
bool DMA_write_block(const void* buf, uint64_t lba);

extern int read_blocks_from_disk(uint64_t lba_start, uint32_t lba_count, void* blockentries[]);
extern int write_blocks_to_disk(uint64_t lba_start, uint32_t lba_count, void* blockentries[]);

extern int readObj_sync(void* dest, size_t siz, uint64_t lba, uint32_t lba_count);

/*
 * storage_block_size - get the size of a block from the nvme device
 */
static inline uint32_t storage_block_size(void)
{
	extern uint32_t block_size;
	return block_size;
}

/*
 * storage_num_blocks - gets the number of blocks from the nvme device
 */
static inline uint64_t storage_num_blocks(void)
{
	extern uint64_t num_blocks;
	return num_blocks;
}
