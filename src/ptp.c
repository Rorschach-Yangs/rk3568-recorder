#include "ptp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static clockid_t g_ptp_clock = CLOCK_REALTIME;

int ptp_init(void)
{
    // 尝试打开PTP硬件时钟
#ifdef __arm__
    // 在RK3568上,PTP时钟设备为 /dev/ptp0 或 /dev/ptp1
    // 这里使用clock_gettime配合phc2sys同步后的CLOCK_REALTIME
    struct timespec res;
    if (clock_getres(CLOCK_REALTIME, &res) == 0) {
        printf("[PTP] CLOCK_REALTIME resolution: %ld ns\n", res.tv_nsec);
    }
    // 实际产品中:
    // 1. ptp4l 同步PTP硬件时钟
    // 2. phc2sys 将硬件时钟同步到系统时钟
    // 3. 应用层使用 CLOCK_REALTIME 获取微秒级精度时间
    g_ptp_clock = CLOCK_REALTIME;
#else
    g_ptp_clock = CLOCK_REALTIME;
#endif
    printf("[PTP] Initialized (simulated, real HW uses /dev/ptp0+phc2sys)\n");
    return 0;
}

void ptp_close(void)
{
    printf("[PTP] Closed\n");
}

uint64_t ptp_get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(g_ptp_clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void ptp_sleep_until_ns(uint64_t target_ns)
{
    uint64_t now = ptp_get_timestamp_ns();
    if (target_ns <= now) return;

    uint64_t delta = target_ns - now;
    struct timespec ts = {
        .tv_sec  = delta / 1000000000ULL,
        .tv_nsec = delta % 1000000000ULL
    };
    clock_nanosleep(g_ptp_clock, 0, &ts, NULL);
}
