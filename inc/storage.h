#ifndef STORAGE_H
#define STORAGE_H

#include "config.h"

typedef struct storage_ctx storage_ctx_t;

storage_ctx_t* storage_init(const char *data_dir);
void storage_destroy(storage_ctx_t *ctx);

// 写入一个数据块到SSD (带O_DIRECT + write-through)
int storage_write_block(storage_ctx_t *ctx, const adc_data_block_t *blk);

// 循环覆盖: 当磁盘空间不足时自动删除最旧文件
int storage_check_and_rotate(storage_ctx_t *ctx);

// 获取存储统计
void storage_get_stats(storage_ctx_t *ctx, uint64_t *total_files,
                       uint64_t *total_bytes, double *speed_mbps);

// 手动触发flush (掉电保护)
int storage_flush(storage_ctx_t *ctx);

#endif // STORAGE_H
