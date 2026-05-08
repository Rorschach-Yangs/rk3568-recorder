#ifndef AI_DETECT_H
#define AI_DETECT_H

#include "config.h"

typedef struct ai_ctx ai_ctx_t;

ai_ctx_t* ai_detect_init(void);
void ai_detect_destroy(ai_ctx_t *ctx);

// 分析一个数据块,识别故障 (模拟RKNN-Toolkit2推理)
// 返回0=正常, >0=发现故障
int ai_detect_analyze(ai_ctx_t *ctx, const adc_data_block_t *blk,
                      ai_diagnosis_t *result);

// 获取诊断统计
void ai_detect_stats(ai_ctx_t *ctx, uint64_t *total_analyzed,
                     uint64_t *faults_found);

#endif // AI_DETECT_H
