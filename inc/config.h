#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// ===================== 硬件平台配置 =====================
#define PLATFORM_NAME           "RK3568"
#define CPU_CORES               4
#define NPU_TOPS                1.0

// ===================== ADC 采集配置 =====================
#define ADC_CHANNELS            8           // AD7606 8通道
#define ADC_SAMPLE_RATE         200000      // 200kSPS per channel
#define ADC_RESOLUTION_BITS     16          // 16-bit
#define ADC_VREF                 5.0        // 参考电压
#define ADC_RANGE               (-10.0)     // ±10V 输入范围
#define ADC_RANGE_MAX           10.0

// 每次采集的数据块大小: 8ch × 2bytes × 1000 samples = 16KB
#define ADC_BLOCK_SAMPLES       1000
#define ADC_BLOCK_SIZE          (ADC_CHANNELS * sizeof(int16_t) * ADC_BLOCK_SAMPLES)
#define ADC_BLOCKS_PER_SECOND   (ADC_SAMPLE_RATE / ADC_BLOCK_SAMPLES)  // 200 blocks/s

// ===================== 环形缓冲区配置 =====================
#define RINGBUF_BLOCK_COUNT     512         // 512个块, ~8MB 缓冲区
#define RINGBUF_TOTAL_SIZE      (RINGBUF_BLOCK_COUNT * ADC_BLOCK_SIZE)

// ===================== SATA SSD 存储配置 =====================
#define STORAGE_DATA_DIR        "/mnt/ssd/data"
#define STORAGE_MAX_SIZE_GB     1000        // 1TB 循环存储
#define STORAGE_FILE_PREFIX     "adc_data_"
#define STORAGE_FILE_MAX_SIZE   (256 * 1024 * 1024)  // 256MB per file
#define STORAGE_WRITE_CHUNK     (1024 * 1024)         // 1MB write chunk
#define STORAGE_TARGET_SPEED    450         // 目标写入速度 MB/s

// ===================== 边缘AI诊断配置 =====================
#define AI_DIAG_INTERVAL_MS     100         // 诊断间隔 <100ms
#define AI_FFT_SIZE             1024        // FFT点数
#define AI_ANOMALY_THRESHOLD    3.0         // 异常检测阈值(标准差倍数)

// 故障类型枚举
typedef enum {
    FAULT_NONE = 0,
    FAULT_OVERVOLTAGE,          // 过压
    FAULT_UNDERVOLTAGE,         // 欠压
    FAULT_FREQ_ANOMALY,        // 频率异常
    FAULT_HARMONIC_DISTORTION,  // 谐波畸变
    FAULT_TRANSIENT_SPIKE,     // 瞬态尖峰
    FAULT_PHASE_IMBALANCE,     // 相位不平衡
    FAULT_BEARING_FAULT,       // 轴承故障(振动)
    FAULT_LOOSENESS,           // 松动故障(振动)
    FAULT_TYPE_COUNT
} fault_type_t;

// ===================== PTP 授时配置 =====================
#define PTP_DEVICE              "/dev/ptp0"
#define PTP_ACCURACY_NS         1000        // 微秒级精度

// ===================== 系统调优配置 =====================
#define ACQ_THREAD_PRIORITY     80          // SCHED_FIFO 优先级
#define STORE_THREAD_PRIORITY   70
#define AI_THREAD_PRIORITY      60
#define ACQ_CPU_AFFINITY        (1 << 2)    // 绑定到CPU2
#define STORE_CPU_AFFINITY      (1 << 3)    // 绑定到CPU3

// ADC 数据块结构体 (DMA 传输单元)
typedef struct __attribute__((packed, aligned(4096))) {
    uint64_t    block_id;                   // 块序号(单调递增)
    uint64_t    ptp_timestamp_ns;           // PTP 纳秒时间戳
    uint32_t    sample_count;              // 本块采样点数
    uint32_t    crc32;                     // 数据校验
    int16_t     data[ADC_CHANNELS][ADC_BLOCK_SAMPLES]; // 8通道 × 1000采样
} adc_data_block_t;

// AI 诊断结果
typedef struct {
    uint64_t    timestamp_ns;              // 故障时间戳
    fault_type_t fault_type;               // 故障类型
    uint8_t     channel;                   // 故障通道
    float       severity;                  // 严重程度 0.0~1.0
    float       rms_value[ADC_CHANNELS];   // 各通道RMS值
    float       peak_value[ADC_CHANNELS];  // 各通道峰值
    float       thd;                       // 总谐波失真
    char        description[128];          // 故障描述
} ai_diagnosis_t;

// 统计信息 (使用uint64_t保证原子操作兼容)
typedef struct {
    volatile uint64_t blocks_acquired;     // 已采集块数
    volatile uint64_t blocks_written;      // 已写入块数
    volatile uint64_t blocks_dropped;      // 丢块数
    volatile uint64_t bytes_written;       // 已写入字节数
    volatile uint64_t faults_detected;     // 检测到故障数
    volatile uint64_t write_speed_kbps;    // 实时写入速度 (KB/s, *1024=MB/s)
    volatile uint64_t acq_speed_kbps;      // 实时采集速度 (KB/s)
    volatile uint64_t ringbuf_overruns;    // 缓冲区溢出次数
} system_stats_t;

extern system_stats_t g_stats;

#endif // CONFIG_H
