#include "adc_sim.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 随机噪声 (简单LCG)
static inline double rand_noise(void)
{
    static unsigned seed = 42;
    seed = seed * 1103515245 + 12345;
    return (double)(seed & 0x7FFFFFFF) / (double)0x7FFFFFFF * 0.02 - 0.01;
}

void adc_sim_init(adc_sim_t *sim)
{
    memset(sim, 0, sizeof(*sim));
    // 配置8通道模拟不同传感器
    // CH0-CH2: 电压 (50Hz正弦, 3相)
    // CH3:    电压 (60Hz正弦, 备用)
    // CH4-CH5: 振动 (加速度传感器, 含轴承特征频率)
    // CH6:    温度相关电压
    // CH7:    电流

    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        sim->phase[ch] = (double)rand() / RAND_MAX * 2.0 * M_PI;
    }

    // 三相电压通道
    sim->frequency[0] = 50.0;   sim->amplitude[0] = 5.0;
    sim->wave_type[0] = WAVE_SINE_50HZ;

    sim->frequency[1] = 50.0;   sim->amplitude[1] = 5.0;
    sim->wave_type[1] = WAVE_SINE_50HZ;  // 相位滞后120°自动处理

    sim->frequency[2] = 50.0;   sim->amplitude[2] = 5.0;
    sim->wave_type[2] = WAVE_SINE_50HZ;

    // 备用60Hz通道
    sim->frequency[3] = 60.0;   sim->amplitude[3] = 3.3;
    sim->wave_type[3] = WAVE_SINE_60HZ;

    // 振动通道 (模拟加速度传感器)
    sim->frequency[4] = 120.0;  sim->amplitude[4] = 2.0;
    sim->wave_type[4] = WAVE_BEARING_FAULT;

    sim->frequency[5] = 80.0;   sim->amplitude[5] = 1.5;
    sim->wave_type[5] = WAVE_HARMONIC_RICH;

    // 温度/电流
    sim->frequency[6] = 50.0;   sim->amplitude[6] = 8.0;
    sim->wave_type[6] = WAVE_TRIANGULAR;

    sim->frequency[7] = 50.0;   sim->amplitude[7] = 4.0;
    sim->wave_type[7] = WAVE_SAWTOOTH;

    sim->fault_channel = -1;
    sim->injected_fault = FAULT_NONE;
    sim->fault_start_sample = 0;
    sim->fault_duration = 0;

    printf("[ADC_SIM] 8-channel AD7606 simulator ready (200kSPS)\n");
    printf("          CH0-2: 3-phase 50Hz AC voltage\n");
    printf("          CH3:   60Hz AC\n");
    printf("          CH4-5: Vibration (bearing fault freq)\n");
    printf("          CH6:   Temperature signal\n");
    printf("          CH7:   Current signal\n");
}

// 生成单个采样值
static inline int16_t generate_sample(wave_type_t wtype, double *phase,
                                      double freq, double amp,
                                      double sample_rate,
                                      int64_t global_sample,
                                      int fault_channel, int current_ch,
                                      fault_type_t fault)
{
    double t = (double)global_sample / sample_rate;
    double omega = 2.0 * M_PI * freq;
    double value = 0.0;
    double phase_offset = 0.0;

    // 三相相位偏移
    if (current_ch == 1) phase_offset = 2.0 * M_PI / 3.0;
    if (current_ch == 2) phase_offset = 4.0 * M_PI / 3.0;

    switch (wtype) {
    case WAVE_SINE_50HZ:
    case WAVE_SINE_60HZ:
        value = amp * sin(omega * t + *phase + phase_offset);
        // 叠加谐波(模拟电力系统真实情况)
        value += amp * 0.05 * sin(3.0 * omega * t + *phase);   // 3次谐波5%
        value += amp * 0.02 * sin(5.0 * omega * t + *phase);   // 5次谐波2%
        break;

    case WAVE_TRIANGULAR:
        *phase = fmod(omega * t, 2.0 * M_PI);
        value = amp * (2.0 / M_PI * asin(sin(*phase)));
        break;

    case WAVE_SAWTOOTH:
        *phase = fmod(omega * t, 2.0 * M_PI);
        value = amp * (1.0 - 2.0 * (*phase / (2.0 * M_PI)));
        break;

    case WAVE_SQUARE:
        value = amp * (sin(omega * t + *phase) >= 0 ? 1.0 : -1.0);
        break;

    case WAVE_BEARING_FAULT:
        // 模拟轴承故障: 基频 + 特征频率 + 冲击脉冲
        value = amp * (0.6 * sin(omega * t + *phase) +
                       0.3 * sin(5.2 * omega * t + *phase) +   // BPFO特征
                       0.1 * sin(12.0 * omega * t + *phase));  // 高阶谐波
        // 周期性冲击
        if (fmod(t * freq / 5.0, 1.0) < 0.02) value += amp * 2.0;
        break;

    case WAVE_HARMONIC_RICH:
        value = amp * (0.5 * sin(omega * t + *phase) +
                       0.2 * sin(2.0 * omega * t) +
                       0.15 * sin(3.0 * omega * t) +
                       0.1 * sin(5.0 * omega * t) +
                       0.05 * sin(7.0 * omega * t));
        break;

    default:
        value = amp * sin(omega * t + *phase);
        break;
    }

    // 叠加底噪
    value += rand_noise() * amp * 0.01;

    // 故障注入
    if (current_ch == fault_channel && fault != FAULT_NONE) {
        switch (fault) {
        case FAULT_OVERVOLTAGE:
            value *= 1.5;
            break;
        case FAULT_UNDERVOLTAGE:
            value *= 0.4;
            break;
        case FAULT_FREQ_ANOMALY:
            value = amp * 1.2 * sin(2.5 * omega * t + *phase);  // 频率突变
            break;
        case FAULT_TRANSIENT_SPIKE:
            if (fmod(t * freq / 10.0, 1.0) < 0.005)
                value = amp * 5.0;  // 冲击尖峰
            break;
        case FAULT_HARMONIC_DISTORTION:
            value += amp * 0.8 * sin(7.0 * omega * t) +
                     amp * 0.5 * sin(11.0 * omega * t);
            break;
        case FAULT_BEARING_FAULT:
            if (fmod(t * freq / 3.0, 1.0) < 0.01) value += amp * 3.0;
            break;
        case FAULT_LOOSENESS:
            value += amp * 0.3 * sin(0.5 * omega * t);  // 次谐波(松动特征)
            break;
        default:
            break;
        }
    }

    // 限幅到ADC量程
    if (value > ADC_RANGE_MAX) value = ADC_RANGE_MAX;
    if (value < ADC_RANGE) value = ADC_RANGE;

    // 转换为16-bit整数
    int16_t raw = (int16_t)((value / ADC_RANGE_MAX) * 32767.0);
    return raw;
}

void adc_sim_generate_block(adc_sim_t *sim, adc_data_block_t *blk,
                            uint64_t block_id, uint64_t timestamp_ns)
{
    blk->block_id = block_id;
    blk->ptp_timestamp_ns = timestamp_ns;
    blk->sample_count = ADC_BLOCK_SAMPLES;
    blk->crc32 = 0;  // 简化处理,实际应计算CRC

    int64_t base_sample = (int64_t)block_id * ADC_BLOCK_SAMPLES;

    // 检查是否需要注入随机故障 (1/500 概率)
    if (sim->fault_channel < 0 && (block_id % 500 == 100 || block_id % 500 == 300)) {
        int ch = rand() % ADC_CHANNELS;
        fault_type_t faults[] = {
            FAULT_OVERVOLTAGE, FAULT_UNDERVOLTAGE,
            FAULT_FREQ_ANOMALY, FAULT_TRANSIENT_SPIKE,
            FAULT_HARMONIC_DISTORTION, FAULT_BEARING_FAULT, FAULT_LOOSENESS
        };
        fault_type_t ft = faults[rand() % (sizeof(faults)/sizeof(faults[0]))];
        adc_sim_inject_fault(sim, ch, ft, ADC_SAMPLE_RATE / 10);  // 100ms故障
    }

    for (int s = 0; s < ADC_BLOCK_SAMPLES; s++) {
        int64_t global_sample = base_sample + s;
        for (int ch = 0; ch < ADC_CHANNELS; ch++) {
            int16_t val = generate_sample(
                sim->wave_type[ch],
                &sim->phase[ch],
                sim->frequency[ch],
                sim->amplitude[ch],
                ADC_SAMPLE_RATE,
                global_sample,
                sim->fault_channel, ch,
                sim->injected_fault
            );
            blk->data[ch][s] = val;
        }

        // 故障持续时间管理
        if (sim->fault_channel >= 0 && global_sample >=
            (int64_t)(sim->fault_start_sample + sim->fault_duration)) {
            sim->fault_channel = -1;
            sim->injected_fault = FAULT_NONE;
        }
    }
}

void adc_sim_inject_fault(adc_sim_t *sim, int channel, fault_type_t fault,
                          uint64_t duration_samples)
{
    sim->fault_channel = channel;
    sim->injected_fault = fault;
    sim->fault_start_sample = 0;  // 由generate_block中block_id计算
    sim->fault_duration = duration_samples;
}
