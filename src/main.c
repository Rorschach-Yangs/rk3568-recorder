#include "config.h"
#include "ringbuf.h"
#include "adc_sim.h"
#include "storage.h"
#include "ai_detect.h"
#include "ptp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

// 全局统计
system_stats_t g_stats = {0};

// 全局运行标志
static volatile int g_running = 1;

// 线程上下文
typedef struct {
    ringbuf_t      *ringbuf;
    adc_sim_t      *adc;
    storage_ctx_t  *storage;
    ai_ctx_t       *ai;
} thread_ctx_t;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

// 设置实时线程属性
static void set_realtime_priority(int priority, int cpu_affinity)
{
    struct sched_param param;
    param.sched_priority = priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        printf("[WARN] Cannot set SCHED_FIFO (try sudo for RT priority)\n");
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < CPU_CORES; i++) {
        if (cpu_affinity & (1 << i)) CPU_SET(i, &cpuset);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

// ===================== 采集线程 =====================
// 模拟AD7606 DMA采集: 以200kSPS速率生成数据块
static void* acquisition_thread(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t*)arg;
    set_realtime_priority(ACQ_THREAD_PRIORITY, ACQ_CPU_AFFINITY);

    printf("[ACQ] Acquisition thread started on CPU%d (SCHED_FIFO prio=%d)\n",
           __builtin_ctz(ACQ_CPU_AFFINITY), ACQ_THREAD_PRIORITY);
    printf("[ACQ] Target: %d kSPS x %d channels = %.1f MB/s\n",
           ADC_SAMPLE_RATE / 1000, ADC_CHANNELS,
           (double)(ADC_SAMPLE_RATE * ADC_CHANNELS * sizeof(int16_t)) / (1024*1024));

    uint64_t block_interval_ns = (uint64_t)ADC_BLOCK_SAMPLES *
                                  1000000000ULL / ADC_SAMPLE_RATE;
    uint64_t next_wake_ns = ptp_get_timestamp_ns();

    uint64_t block_id = 0;
    uint64_t last_stats_ns = next_wake_ns;
    uint64_t blocks_last = 0;

    while (g_running) {
        next_wake_ns += block_interval_ns;
        ptp_sleep_until_ns(next_wake_ns);

        adc_data_block_t *blk = ringbuf_acquire_free(ctx->ringbuf);
        if (!blk) {
            __atomic_fetch_add(&g_stats.blocks_dropped, 1, __ATOMIC_RELAXED);
            continue;
        }

        uint64_t ts = ptp_get_timestamp_ns();
        adc_sim_generate_block(ctx->adc, blk, block_id, ts);

        ringbuf_commit(ctx->ringbuf);
        __atomic_fetch_add(&g_stats.blocks_acquired, 1, __ATOMIC_RELAXED);
        block_id++;

        uint64_t now = ptp_get_timestamp_ns();
        if (now - last_stats_ns >= 1000000000ULL) {
            uint64_t total = __atomic_load_n(&g_stats.blocks_acquired,
                                             __ATOMIC_RELAXED);
            uint64_t speed = (uint64_t)((double)(total - blocks_last) *
                              ADC_BLOCK_SIZE / 1024.0);
            __atomic_store_n(&g_stats.acq_speed_kbps, speed,
                             __ATOMIC_RELAXED);
            last_stats_ns = now;
            blocks_last = total;
        }

        if (block_id % 2000 == 0) {
            storage_check_and_rotate(ctx->storage);
        }
    }

    printf("[ACQ] Thread stopped. Blocks: %lu, Dropped: %lu\n",
           (unsigned long)block_id,
           (unsigned long)g_stats.blocks_dropped);
    return NULL;
}

// ===================== 存储线程 =====================
static void* storage_thread(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t*)arg;
    set_realtime_priority(STORE_THREAD_PRIORITY, STORE_CPU_AFFINITY);

    printf("[STORE] Storage thread started on CPU%d (SCHED_FIFO prio=%d)\n",
           __builtin_ctz(STORE_CPU_AFFINITY), STORE_THREAD_PRIORITY);

    while (g_running || ringbuf_available(ctx->ringbuf) > 0) {
        adc_data_block_t *blk = ringbuf_get_ready(ctx->ringbuf);
        if (!blk) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000};
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
            continue;
        }

        int ret = storage_write_block(ctx->storage, blk);
        if (ret < 0) {
            fprintf(stderr, "[STORE] Write error!\n");
        } else {
            __atomic_fetch_add(&g_stats.blocks_written, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_stats.bytes_written,
                               sizeof(adc_data_block_t), __ATOMIC_RELAXED);
        }

        ringbuf_release(ctx->ringbuf);

        double speed_mbps;
        storage_get_stats(ctx->storage, NULL, NULL, &speed_mbps);
        __atomic_store_n(&g_stats.write_speed_kbps,
                         (uint64_t)(speed_mbps * 1024.0), __ATOMIC_RELAXED);
    }

    storage_flush(ctx->storage);
    printf("[STORE] Thread stopped. Written: %lu blocks\n",
           (unsigned long)g_stats.blocks_written);
    return NULL;
}

// ===================== AI诊断线程 =====================
static void* ai_thread(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t*)arg;
    set_realtime_priority(AI_THREAD_PRIORITY, 0);

    printf("[AI] Edge AI diagnosis thread started (interval <100ms)\n");
    printf("[AI] Using simulated RKNN model on 1 TOPS NPU\n");

    uint64_t last_analyzed_block_id = 0;

    while (g_running) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = AI_DIAG_INTERVAL_MS * 1000000};
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);

        uint64_t current_w = __atomic_load_n(&ctx->ringbuf->write_index,
                                             __ATOMIC_ACQUIRE);
        uint64_t current_r = __atomic_load_n(&ctx->ringbuf->read_index,
                                             __ATOMIC_ACQUIRE);

        if (current_r < current_w && current_r > last_analyzed_block_id) {
            uint64_t idx = (current_r - 1) & ctx->ringbuf->mask;
            adc_data_block_t *blk = &ctx->ringbuf->blocks[idx];

            ai_diagnosis_t diagnosis;
            int fault = ai_detect_analyze(ctx->ai, blk, &diagnosis);

            if (fault) {
                printf("\n╔══════════════════════════════════════════════════════════╗\n");
                printf("║  [AI DIAGNOSIS] Fault Detected!                          ║\n");
                printf("║  %-54s ║\n", diagnosis.description);
                printf("║  Timestamp: %lu.%06lu                                      ║\n",
                       (unsigned long)(diagnosis.timestamp_ns / 1000000000ULL),
                       (unsigned long)((diagnosis.timestamp_ns / 1000ULL) % 1000000ULL));
                printf("║  Bandwidth saved: ~90%% (only diagnosis uploaded)        ║\n");
                printf("╚══════════════════════════════════════════════════════════╝\n\n");
            }

            last_analyzed_block_id = current_r;
        }
    }

    printf("[AI] Thread stopped.\n");
    return NULL;
}

// ===================== 状态显示线程 =====================
static void* stats_thread(void *arg)
{
    (void)arg;
    printf("\n");
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│  RK3568 High-Speed Data Recorder — Edge AI Terminal        │\n");
    printf("│  8-ch AD7606 @200kSPS | SATA SSD | PTP | AI Diagnosis       │\n");
    printf("├────────────┬────────────┬────────────┬────────────┬─────────┤\n");
    printf("│ ACQ(kB/s)  │ WR(kB/s)   │ Overruns   │ Dropped    │ Faults  │\n");
    printf("├────────────┼────────────┼────────────┼────────────┼─────────┤\n");

    while (g_running) {
        sleep(1);

        uint64_t acq_kbps  = __atomic_load_n(&g_stats.acq_speed_kbps, __ATOMIC_RELAXED);
        uint64_t wr_kbps   = __atomic_load_n(&g_stats.write_speed_kbps, __ATOMIC_RELAXED);
        uint64_t overruns  = __atomic_load_n(&g_stats.ringbuf_overruns, __ATOMIC_RELAXED);
        uint64_t dropped   = __atomic_load_n(&g_stats.blocks_dropped, __ATOMIC_RELAXED);
        uint64_t faults    = __atomic_load_n(&g_stats.faults_detected, __ATOMIC_RELAXED);

        printf("\r│ %10lu │ %10lu │ %10lu │ %10lu │ %7lu │",
               (unsigned long)acq_kbps,
               (unsigned long)wr_kbps,
               (unsigned long)overruns,
               (unsigned long)dropped,
               (unsigned long)faults);
        fflush(stdout);
    }

    printf("\n└────────────┴────────────┴────────────┴────────────┴─────────┘\n");
    return NULL;
}

// ===================== Main =====================
int main(int argc, char *argv[])
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  RK3568 工业级高速数据记录仪 (边缘AI诊断终端)               ║\n");
    printf("║  Platform: Rockchip RK3568 (4xA55 + 1TOPS NPU)             ║\n");
    printf("║  ADC: AD7606 8-ch @200kSPS | Storage: SATA SSD             ║\n");
    printf("║  Arch: Acquisition -> RingBuf -> Storage + Edge AI         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    const char *data_dir = STORAGE_DATA_DIR;
    if (argc > 1) data_dir = argv[1];

    printf("Data directory: %s\n", data_dir);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ringbuf_t ringbuf;
    if (ringbuf_init(&ringbuf) < 0) {
        fprintf(stderr, "Failed to initialize ring buffer\n");
        return 1;
    }

    adc_sim_t adc;
    adc_sim_init(&adc);

    if (ptp_init() < 0) {
        fprintf(stderr, "Failed to initialize PTP\n");
        return 1;
    }

    storage_ctx_t *storage = storage_init(data_dir);
    if (!storage) {
        fprintf(stderr, "Failed to initialize storage\n");
        return 1;
    }

    ai_ctx_t *ai = ai_detect_init();
    if (!ai) {
        fprintf(stderr, "Failed to initialize AI engine\n");
        return 1;
    }

    thread_ctx_t tctx = {
        .ringbuf = &ringbuf,
        .adc = &adc,
        .storage = storage,
        .ai = ai
    };

    pthread_t acq_tid, store_tid, ai_tid, stats_tid;

    pthread_create(&stats_tid, NULL, stats_thread, &tctx);
    pthread_create(&store_tid, NULL, storage_thread, &tctx);
    pthread_create(&ai_tid, NULL, ai_thread, &tctx);
    pthread_create(&acq_tid, NULL, acquisition_thread, &tctx);

    printf("\n[MAIN] All threads started. Press Ctrl+C to stop.\n\n");

    while (g_running) {
        pause();
    }

    printf("\n[MAIN] Shutting down...\n");

    pthread_join(acq_tid, NULL);
    pthread_join(ai_tid, NULL);
    pthread_join(store_tid, NULL);
    pthread_cancel(stats_tid);
    pthread_join(stats_tid, NULL);

    ai_detect_destroy(ai);
    storage_destroy(storage);
    ptp_close();
    ringbuf_destroy(&ringbuf);

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  Final Statistics\n");
    printf("  Blocks Acquired:  %lu\n",
           (unsigned long)g_stats.blocks_acquired);
    printf("  Blocks Written:   %lu\n",
           (unsigned long)g_stats.blocks_written);
    printf("  Blocks Dropped:   %lu\n",
           (unsigned long)g_stats.blocks_dropped);
    printf("  Total Data:       %.2f GB\n",
           (double)g_stats.bytes_written / (1024.0*1024.0*1024.0));
    printf("  Faults Detected:  %lu\n",
           (unsigned long)g_stats.faults_detected);
    printf("  Buffer Overruns:  %lu\n",
           (unsigned long)g_stats.ringbuf_overruns);
    printf("═══════════════════════════════════════════════════\n");

    return 0;
}
