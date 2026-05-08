#include "ringbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int ringbuf_init(ringbuf_t *rb)
{
    // 使用匿名mmap分配对齐内存,模拟DMA缓冲区
    size_t size = RINGBUF_BLOCK_COUNT * sizeof(adc_data_block_t);
    rb->blocks = mmap(NULL, size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                      -1, 0);

    if (rb->blocks == MAP_FAILED) {
        // HugeTLB失败时回退到普通匿名映射
        rb->blocks = mmap(NULL, size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
        if (rb->blocks == MAP_FAILED) {
            perror("mmap ringbuf");
            return -1;
        }
    }

    memset(rb->blocks, 0, size);
    rb->write_index = 0;
    rb->read_index = 0;
    rb->mask = RINGBUF_BLOCK_COUNT - 1;

    printf("[RINGBUF] Initialized: %lu blocks, %.2f MB total (mmap'd)\n",
           (unsigned long)RINGBUF_BLOCK_COUNT,
           (double)size / (1024 * 1024));
    return 0;
}

void ringbuf_destroy(ringbuf_t *rb)
{
    size_t size = RINGBUF_BLOCK_COUNT * sizeof(adc_data_block_t);
    munmap(rb->blocks, size);
    printf("[RINGBUF] Destroyed\n");
}

// 判断是否有空闲块
static inline int ringbuf_is_full(ringbuf_t *rb)
{
    uint64_t w = __atomic_load_n(&rb->write_index, __ATOMIC_ACQUIRE);
    uint64_t r = __atomic_load_n(&rb->read_index, __ATOMIC_ACQUIRE);
    return (w - r) >= RINGBUF_BLOCK_COUNT;
}

adc_data_block_t* ringbuf_acquire_free(ringbuf_t *rb)
{
    if (ringbuf_is_full(rb)) {
        __atomic_fetch_add(&g_stats.ringbuf_overruns, 1, __ATOMIC_RELAXED);
        return NULL;  // 缓冲区满 → 丢块
    }
    uint64_t idx = rb->write_index & rb->mask;
    return &rb->blocks[idx];
}

void ringbuf_commit(ringbuf_t *rb)
{
    __atomic_fetch_add(&rb->write_index, 1, __ATOMIC_RELEASE);
}

adc_data_block_t* ringbuf_get_ready(ringbuf_t *rb)
{
    uint64_t w = __atomic_load_n(&rb->write_index, __ATOMIC_ACQUIRE);
    uint64_t r = __atomic_load_n(&rb->read_index, __ATOMIC_ACQUIRE);
    if (r >= w) return NULL;
    uint64_t idx = r & rb->mask;
    return &rb->blocks[idx];
}

void ringbuf_release(ringbuf_t *rb)
{
    __atomic_fetch_add(&rb->read_index, 1, __ATOMIC_RELEASE);
}

uint64_t ringbuf_available(ringbuf_t *rb)
{
    uint64_t w = __atomic_load_n(&rb->write_index, __ATOMIC_ACQUIRE);
    uint64_t r = __atomic_load_n(&rb->read_index, __ATOMIC_ACQUIRE);
    return (w >= r) ? (w - r) : 0;
}

uint64_t ringbuf_free_count(ringbuf_t *rb)
{
    return RINGBUF_BLOCK_COUNT - ringbuf_available(rb);
}
