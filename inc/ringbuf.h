#ifndef RINGBUF_H
#define RINGBUF_H

#include "config.h"

// 无锁环形缓冲区 — 模拟DMA零拷贝架构
typedef struct {
    adc_data_block_t *blocks;              // mmap映射的内存池
    volatile uint64_t write_index;         // 采集线程写入位置
    volatile uint64_t read_index;          // 存储线程读取位置
    uint64_t          mask;               // 索引掩码(RINGBUF_BLOCK_COUNT-1)
} ringbuf_t;

int  ringbuf_init(ringbuf_t *rb);
void ringbuf_destroy(ringbuf_t *rb);

// 采集线程: 获取一个空闲块用于DMA写入 (返回NULL表示缓冲区满)
adc_data_block_t* ringbuf_acquire_free(ringbuf_t *rb);

// 采集线程: 提交已完成DMA传输的块
void ringbuf_commit(ringbuf_t *rb);

// 存储线程: 获取下一个可读取的块 (返回NULL表示无数据)
adc_data_block_t* ringbuf_get_ready(ringbuf_t *rb);

// 存储线程: 释放已写入磁盘的块
void ringbuf_release(ringbuf_t *rb);

// 查询缓冲区使用状态
uint64_t ringbuf_available(ringbuf_t *rb);
uint64_t ringbuf_free_count(ringbuf_t *rb);

#endif // RINGBUF_H
