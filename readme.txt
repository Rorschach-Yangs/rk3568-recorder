RK3568 工业级高速数据记录仪（边缘AI诊断终端）
=====================================================

项目背景
--------
针对工业现场（电力、轨道交通、智能制造）高频数据采集易丢包、原始数据传输
带宽占用高、故障诊断滞后等痛点，基于RK3568芯片设计一款集高速同步采集、大
容量固态存储、边缘AI实时诊断于一体的智能终端，替代传统采集设备，实现数据
"本地存、边缘算、精准传"。

硬件平台
--------
- 主控: RK3568（4核A55、1TOPS NPU、双千兆以太网、PCIe 2.1、SATA 3.0）
- 存储: eMMC（系统运行）+ M.2 SATA SSD（高速数据存储，支持TB级循环存储）
- 采集接口: 8通道同步AD采集（AD7606）、CAN FD、RS485/422、双千兆网口（PTP授时）
- 外设: HDMI实时显示、工业级看门狗、隔离电源

项目结构
--------
  config.h              硬件参数、数据结构、系统配置
  ringbuf.h / ringbuf.c  无锁环形缓冲区 (mmap模拟DMA零拷贝)
  adc_sim.h / adc_sim.c  8通道AD7606模拟器 (200kSPS波形生成+故障注入)
  storage.h / storage.c  SATA SSD高速写入 (O_DIRECT + 循环覆盖)
  ai_detect.h / ai_detect.c  边缘AI诊断 (模拟RKNN-Toolkit2推理)
  ptp.h / ptp.c          PTP精准授时 (微秒级时间戳)
  main.c                 主程序: 4线程采集-存储-分析流水线
  Makefile               支持 x86本地编译 + ARM64交叉编译
  rk3568-recorder        编译产物
  rk3568-recorder.service  systemd服务文件 (7x24自愈运行)

架构设计
--------
  ┌──────────────┐    ┌──────────────────┐    ┌──────────────┐
  │ Acquisition  │───>│  Ring Buffer     │───>│  SATA SSD    │
  │ Thread       │    │  (512 blocks     │    │  Storage     │
  │ CPU2 FIFO80  │    │   8MB mmap'd)    │    │  CPU3 FIFO70 │
  │ 200kSPS×8ch  │    └──────────────────┘    │  O_DIRECT    │
  └──────────────┘              │             └──────────────┘
                                ▼
                      ┌──────────────────┐
                      │  Edge AI Engine  │
                      │  100ms interval  │
                      │  RKNN simulation │
                      └──────────────────┘

关键模块说明
------------

[环形缓冲区] ringbuf.c
  - 512个数据块 × 16KB/块 = 8MB 总容量
  - mmap 匿名映射（优先HugeTLB），模拟DMA物理内存池
  - __atomic 无锁读写，采集线程写、存储线程读，无锁竞争
  - 缓冲区满时自动丢块并记录 overrun 计数

[ADC模拟器] adc_sim.c
  - CH0-2: 三相50Hz交流电压（相位差120°，含3/5次谐波）
  - CH3:   60Hz备用通道
  - CH4-5: 振动加速度传感器（轴承特征频率，用于故障诊断）
  - CH6:   温度传感器（三角波模拟）
  - CH7:   电流传感器（锯齿波模拟）
  - 自动故障注入：每约500块随机注入1次故障（持续100ms）
  - 支持7种故障类型: 过压、欠压、频率异常、谐波畸变、瞬态尖峰、
    相位不平衡、轴承故障、松动故障
  - 每块 8通道 × 1000采样 × 2字节 = 16KB

[SSD存储] storage.c
  - O_DIRECT 打开文件，绕过Linux页缓存（模拟SATA DMA直写路径）
  - O_DIRECT 不可用时回退到 O_SYNC
  - 256MB 文件轮转，自动递增序号
  - du 命令检测存储目录总大小，超出1TB自动删除最旧文件（循环覆盖）
  - fdatasync 每次flush确保掉电不丢数据
  - 实时统计写入速度（每500ms更新）

[边缘AI诊断] ai_detect.c
  - 每100ms分析一次最新数据块（满足 <100ms 响应时间指标）
  - 特征提取: RMS值、峰值、THD估计
  - 100点滑动窗口建立各通道RMS基线（均值+标准差）
  - Z-score 异常检测:
    - Z > 3.0 且RMS > 1.4倍基线 → 过压
    - Z < -3.0 且RMS < 0.5倍基线 → 欠压
    - 峰值 > 4倍RMS + 8倍标准差 → 瞬态尖峰
  - 振动通道专项: 高THD + 高峰值/ RMS比 → 轴承故障
  - 三相不平衡: 最大RMS与最小RMS差 > 30%
  - 仅输出诊断摘要（故障类型、通道、严重程度、时间戳）
  - 相比上传原始波形，带宽占用降低90%

[PTP授时] ptp.c
  - clock_gettime(CLOCK_REALTIME) 获取纳秒时间戳
  - 实际产品中由 ptp4l + phc2sys 将硬件PTP时钟同步到系统时钟
  - clock_nanosleep 实现高精度定时采集（微秒级）

[主程序] main.c
  - 4线程架构:
    - 采集线程: CPU2绑定, SCHED_FIFO 80, 5ms间隔生成数据块
    - 存储线程: CPU3绑定, SCHED_FIFO 70, 阻塞读取环形缓冲区写入SSD
    - AI线程:   无CPU绑定（可调度到NPU）, SCHED_FIFO 60, 100ms周期诊断
    - 状态线程: 每秒刷新实时运行状态
  - SIGINT/SIGTERM 优雅退出，确保数据落盘
  - 退出时打印完整统计汇总

编译与部署
----------

[1] x86本地编译（开发测试用）
    make

[2] ARM64交叉编译（RK3568部署用）
    make ARCH=arm64
    需要 aarch64-linux-gnu-gcc 工具链
    编译选项自动添加: -march=armv8-a -mtune=cortex-a55

[3] 部署到RK3568设备
    # 拷贝程序
    scp rk3568-recorder root@<rk3568-ip>:/opt/rk3568-recorder/
    # 创建数据目录
    ssh root@<rk3568-ip> 'mkdir -p /mnt/ssd/data'
    # 手动运行
    ssh root@<rk3568-ip> '/opt/rk3568-recorder/rk3568-recorder /mnt/ssd/data'
    # 或指定其他数据目录
    ssh root@<rk3568-ip> '/opt/rk3568-recorder/rk3568-recorder /data/records'

[4] systemd服务（7×24小时自愈运行）
    # 拷贝服务文件
    scp rk3568-recorder.service root@<rk3568-ip>:/etc/systemd/system/
    # 启用并启动
    ssh root@<rk3568-ip> 'systemctl daemon-reload'
    ssh root@<rk3568-ip> 'systemctl enable --now rk3568-recorder'
    # 查看状态
    ssh root@<rk3568-ip> 'systemctl status rk3568-recorder'
    # 查看日志
    ssh root@<rk3568-ip> 'journalctl -u rk3568-recorder -f'

    systemd服务特性:
    - 60秒硬件看门狗（WatchdogSec=60）
    - 异常退出后60秒自动重启
    - CPU调度策略: SCHED_FIFO, 优先级80
    - 内存限制: 512MB
    - 文件系统写保护: 仅/mnt/ssd/data可写

[5] 一键打包部署
    make deploy
    # 生成 rk3568-recorder-deploy.tar.gz，包含程序和systemd服务文件

运行效果
--------
程序启动后显示实时状态面板:

┌─────────────────────────────────────────────────────────────┐
│  RK3568 High-Speed Data Recorder — Edge AI Terminal        │
│  8-ch AD7606 @200kSPS | SATA SSD | PTP | AI Diagnosis       │
├────────────┬────────────┬────────────┬────────────┬─────────┤
│ ACQ(kB/s)  │ WR(kB/s)   │ Overruns   │ Dropped    │ Faults  │
├────────────┼────────────┼────────────┼────────────┼─────────┤
│       3125 │       3200 │          0 │          0 │       3 │
└────────────┴────────────┴────────────┴────────────┴─────────┘

检测到故障时实时打印诊断结果:

╔══════════════════════════════════════════════════════════╗
║  [AI DIAGNOSIS] Fault Detected!                          ║
║  [CH0] OVERVOLTAGE | severity=0.45 | RMS=7.123V          ║
║  Timestamp: 1234567890.123456                            ║
║  Bandwidth saved: ~90% (only diagnosis uploaded)         ║
╚══════════════════════════════════════════════════════════╝

退出时打印最终统计:
═══════════════════════════════════════════════════
  Final Statistics
  Blocks Acquired:  1001
  Blocks Written:   1000
  Blocks Dropped:   0
  Total Data:       0.02 GB
  Faults Detected:  3
  Buffer Overruns:  0
═══════════════════════════════════════════════════

项目关键指标
------------
  ADC采集:        8通道 × 200kSPS × 16bit = 3.2 MB/s
  存储带宽:        目标 450 MB/s (SATA 3.0)
  AI响应时间:      每100ms诊断一次，故障检出 <100ms
  存储容量:        支持 1TB 循环覆盖
  循环缓冲:        8MB (512块 × 16KB)，可抗瞬时I/O抖动
  CPU隔离:        采集线程绑核心2，存储线程绑核心3
  掉电保护:        fdatasync 确保数据落盘
  时间同步:        PTP微秒级精度

从模拟到真实硬件
----------------
本程序目前以软件模拟ADC采集和AI推理，迁移到真实RK3568硬件时：
  1. ADC: 替换 adc_sim 为 AD7606 SPI 驱动（/dev/spi0.0）
  2. DMA:  使用 RK3568 DMAC 实现 SPI RX → 环形缓冲区直传
  3. AI:   链接 librknnrt.so，用 rknn_run() 替换模拟推理
  4. PTP:  配置 ptp4l + phc2sys，使能 /dev/ptp0
  5. SSD:  O_DIRECT 路径已保留，挂载 ext4/xfs 即可用
  6. GPIO: 使能工业看门狗（/dev/watchdog），配置复位策略
