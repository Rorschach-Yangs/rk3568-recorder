#include "ai_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 模拟RKNN-Toolkit2推理上下文
struct ai_ctx {
    uint64_t total_analyzed;
    uint64_t faults_found;
    double   history_rms[ADC_CHANNELS][100];  // 各通道RMS历史 (滑动窗口)
    int      history_idx;
    int      history_count;
};

static const char* fault_names[] = {
    "NORMAL",
    "OVERVOLTAGE",
    "UNDERVOLTAGE",
    "FREQ_ANOMALY",
    "HARMONIC_DISTORTION",
    "TRANSIENT_SPIKE",
    "PHASE_IMBALANCE",
    "BEARING_FAULT",
    "LOOSENESS"
};

ai_ctx_t* ai_detect_init(void)
{
    ai_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    printf("[AI_DETECT] RKNN-Toolkit2 simulation initialized\n");
    printf("[AI_DETECT] Model: anomaly_detector_v3.rknn (1 TOPS NPU)\n");
    printf("[AI_DETECT] Input: 8ch × 1000 samples, Output: fault type + severity\n");
    return ctx;
}

void ai_detect_destroy(ai_ctx_t *ctx)
{
    if (!ctx) return;
    printf("[AI_DETECT] Analyzed %lu blocks, found %lu faults\n",
           (unsigned long)ctx->total_analyzed,
           (unsigned long)ctx->faults_found);
    free(ctx);
}

// 计算RMS值
static float compute_rms(const int16_t *samples, int count)
{
    double sum_sq = 0.0;
    for (int i = 0; i < count; i++) {
        double v = (double)samples[i] / 32767.0 * ADC_RANGE_MAX;
        sum_sq += v * v;
    }
    return (float)sqrt(sum_sq / count);
}

// 计算峰值
static float compute_peak(const int16_t *samples, int count)
{
    float peak = 0.0;
    for (int i = 0; i < count; i++) {
        float v = fabsf((float)samples[i] / 32767.0f * (float)ADC_RANGE_MAX);
        if (v > peak) peak = v;
    }
    return peak;
}

// 简单FFT能量分布分析 (模拟NPU推理)
static float compute_thd_estimate(const int16_t *samples, int count)
{
    // 简化THD估算: 计算信号偏离理想正弦的程度
    double mean = 0.0;
    for (int i = 0; i < count; i++) mean += samples[i];
    mean /= count;

    double ss_total = 0.0, ss_residual = 0.0;
    for (int i = 0; i < count; i++) {
        double v = samples[i];
        ss_total += (v - mean) * (v - mean);
        // 理想正弦拟合
        double ideal = 16000.0 * sin(2.0 * M_PI * 50.0 * i / ADC_SAMPLE_RATE);
        ss_residual += (v - mean - ideal) * (v - mean - ideal);
    }
    if (ss_total < 1.0) return 0.0;
    return (float)(sqrt(ss_residual / ss_total));
}

int ai_detect_analyze(ai_ctx_t *ctx, const adc_data_block_t *blk,
                      ai_diagnosis_t *result)
{
    ctx->total_analyzed++;

    memset(result, 0, sizeof(*result));
    result->timestamp_ns = blk->ptp_timestamp_ns;
    result->fault_type = FAULT_NONE;

    // 各通道特征提取
    float rms[ADC_CHANNELS];
    float peak[ADC_CHANNELS];
    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        rms[ch] = compute_rms(blk->data[ch], ADC_BLOCK_SAMPLES);
        peak[ch] = compute_peak(blk->data[ch], ADC_BLOCK_SAMPLES);
        result->rms_value[ch] = rms[ch];
        result->peak_value[ch] = peak[ch];
    }

    // 更新滑动窗口历史
    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        ctx->history_rms[ch][ctx->history_idx] = rms[ch];
    }
    ctx->history_idx = (ctx->history_idx + 1) % 100;
    if (ctx->history_count < 100) ctx->history_count++;

    // 计算历史统计 (用于异常判定基线)
    float mean_rms[ADC_CHANNELS] = {0};
    float std_rms[ADC_CHANNELS] = {0};
    if (ctx->history_count >= 10) {
        for (int ch = 0; ch < ADC_CHANNELS; ch++) {
            double sum = 0.0;
            for (int i = 0; i < ctx->history_count; i++) sum += ctx->history_rms[ch][i];
            mean_rms[ch] = (float)(sum / ctx->history_count);

            double sum_sq = 0.0;
            for (int i = 0; i < ctx->history_count; i++) {
                double d = ctx->history_rms[ch][i] - mean_rms[ch];
                sum_sq += d * d;
            }
            std_rms[ch] = (float)sqrt(sum_sq / ctx->history_count);
        }
    }

    // 故障检测逻辑 (模拟RKNN模型推理)
    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        if (ctx->history_count < 10) break;  // 需要足够的历史数据

        float z_score = (std_rms[ch] > 0.001f) ?
                        (rms[ch] - mean_rms[ch]) / std_rms[ch] : 0.0f;

        // 过压检测: RMS超出基线3倍标准差
        if (z_score > AI_ANOMALY_THRESHOLD && rms[ch] > mean_rms[ch] * 1.4f) {
            result->fault_type = FAULT_OVERVOLTAGE;
            result->channel = ch;
            result->severity = fminf(1.0f, (z_score - AI_ANOMALY_THRESHOLD) / 6.0f);
        }
        // 欠压检测
        else if (z_score < -AI_ANOMALY_THRESHOLD && rms[ch] < mean_rms[ch] * 0.5f) {
            result->fault_type = FAULT_UNDERVOLTAGE;
            result->channel = ch;
            result->severity = (float)fmin(1.0, fabs(z_score + AI_ANOMALY_THRESHOLD) / 6.0);
        }
        // 瞬态尖峰检测
        else if (peak[ch] > mean_rms[ch] * 4.0f + 8.0f * std_rms[ch]) {
            result->fault_type = FAULT_TRANSIENT_SPIKE;
            result->channel = ch;
            result->severity = (float)fmin(1.0, peak[ch] / (ADC_RANGE_MAX * 0.8));
        }
    }

    // 振动通道(4-5)专项分析: 轴承故障检测
    for (int ch = 4; ch <= 5; ch++) {
        if (ctx->history_count < 10) break;
        float thd = compute_thd_estimate(blk->data[ch], ADC_BLOCK_SAMPLES);

        if (thd > 0.4f && result->fault_type == FAULT_NONE) {
            // 高THD + 周期冲击 → 轴承故障
            float peak = result->peak_value[ch];
            float rms_val = result->rms_value[ch];
            if (peak / (rms_val + 0.001f) > 5.0f) {
                result->fault_type = FAULT_BEARING_FAULT;
                result->channel = ch;
                result->severity = fminf(1.0f, thd);
            } else {
                result->fault_type = FAULT_HARMONIC_DISTORTION;
                result->channel = ch;
                result->severity = fminf(1.0f, thd);
            }
        }
        result->thd = thd;
    }

    // 三相不平衡检测 (CH0, CH1, CH2)
    if (result->fault_type == FAULT_NONE && ctx->history_count >= 10) {
        float max_rms = fmaxf(fmaxf(rms[0], rms[1]), rms[2]);
        float min_rms = fminf(fminf(rms[0], rms[1]), rms[2]);
        if (max_rms > 0.1f && (max_rms - min_rms) / max_rms > 0.3f) {
            result->fault_type = FAULT_PHASE_IMBALANCE;
            result->channel = (rms[0] == min_rms) ? 0 : ((rms[1] == min_rms) ? 1 : 2);
            result->severity = (max_rms - min_rms) / max_rms;
        }
    }

    // 松动故障检测(振动通道的次谐波能量)
    if (result->fault_type == FAULT_NONE) {
        for (int ch = 4; ch <= 5; ch++) {
            float rms_val = result->rms_value[ch];
            float base_rms = (ctx->history_count >= 10) ? mean_rms[ch] : rms_val;
            if (rms_val > base_rms * 1.8f && rms_val < base_rms * 3.0f) {
                result->fault_type = FAULT_LOOSENESS;
                result->channel = ch;
                result->severity = fminf(1.0f, (rms_val - base_rms) / base_rms);
                break;
            }
        }
    }

    // 生成故障描述
    if (result->fault_type != FAULT_NONE) {
        ctx->faults_found++;
        snprintf(result->description, sizeof(result->description),
                 "[CH%d] %s | severity=%.2f | RMS=%.3fV | Peak=%.3fV | "
                 "timestamp=%lu.%06lu",
                 result->channel,
                 fault_names[result->fault_type],
                 result->severity,
                 result->rms_value[result->channel],
                 result->peak_value[result->channel],
                 (unsigned long)(result->timestamp_ns / 1000000000ULL),
                 (unsigned long)((result->timestamp_ns / 1000ULL) % 1000000ULL));

        __atomic_fetch_add(&g_stats.faults_detected, 1, __ATOMIC_RELAXED);
    }

    return (result->fault_type != FAULT_NONE) ? 1 : 0;
}

void ai_detect_stats(ai_ctx_t *ctx, uint64_t *total_analyzed,
                     uint64_t *faults_found)
{
    if (total_analyzed) *total_analyzed = ctx->total_analyzed;
    if (faults_found) *faults_found = ctx->faults_found;
}
