#ifndef PTP_H
#define PTP_H

#include "config.h"

int  ptp_init(void);
void ptp_close(void);
uint64_t ptp_get_timestamp_ns(void);   // 获取PTP纳秒时间戳
void ptp_sleep_until_ns(uint64_t target_ns);

#endif // PTP_H
