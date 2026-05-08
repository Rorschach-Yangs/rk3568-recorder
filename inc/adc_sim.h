#ifndef ADC_SIM_H
#define ADC_SIM_H

#include "config.h"

// 模拟8通道AD7606同步采集 — 生成各类工业波形

// 波形类型(用于模拟不同传感器输入)
typedef enum {
    WAVE_SINE_50HZ,        // 50Hz 正弦波(电力)
    WAVE_SINE_60HZ,        // 60Hz 正弦波(电力)
    WAVE_TRIANGULAR,       // 三角波(振动)
    WAVE_SAWTOOTH,         // 锯齿波
    WAVE_SQUARE,           // 方波
    WAVE_BEARING_FAULT,    // 轴承故障特征频率
    WAVE_HARMONIC_RICH,    // 富含谐波的波形
    WAVE_TYPE_COUNT
} wave_type_t;

// ADC模拟器上下文
typedef struct {
    double phase[ADC_CHANNELS];           // 各通道当前相位
    double frequency[ADC_CHANNELS];       // 各通道基频
    double amplitude[ADC_CHANNELS];       // 各通道幅值(V)
    wave_type_t wave_type[ADC_CHANNELS];  // 各通道波形类型
    int fault_channel;                    // 当前注入故障的通道(-1=无)
    fault_type_t injected_fault;          // 注入的故障类型
    uint64_t fault_start_sample;          // 故障开始采样点
    uint64_t fault_duration;              // 故障持续采样点
} adc_sim_t;

void adc_sim_init(adc_sim_t *sim);
void adc_sim_generate_block(adc_sim_t *sim, adc_data_block_t *blk,
                            uint64_t block_id, uint64_t timestamp_ns);

// 注入故障: 在指定通道上模拟故障波形
void adc_sim_inject_fault(adc_sim_t *sim, int channel, fault_type_t fault,
                          uint64_t duration_samples);

#endif // ADC_SIM_H
