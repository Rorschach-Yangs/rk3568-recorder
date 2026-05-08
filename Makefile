# RK3568 高速数据记录仪 - Build System
# 支持: 本地编译(x86测试) + ARM交叉编译(RK3568部署)

PROG   := rk3568-recorder
SRCDIR := src
INCDIR := inc
BLDDIR := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(SRCS:$(SRCDIR)/%.c=$(BLDDIR)/%.o)

# ----- 工具链选择 -----
# make              → 本地编译 (x86_64, 用于开发测试)
# make ARCH=arm64   → ARM64交叉编译 (RK3568部署)

ifeq ($(ARCH),arm64)
  CROSS  ?= aarch64-linux-gnu-
  CC     := $(CROSS)gcc
  STRIP  := $(CROSS)strip
  CFLAGS += -march=armv8-a -mtune=cortex-a55
  LDFLAGS += -Wl,-gc-sections
else
  CC     ?= gcc
  STRIP  ?= strip
endif

# ----- 编译选项 -----
CFLAGS += -std=gnu11 -Wall -Wextra -O2 -g
CFLAGS += -D_GNU_SOURCE
CFLAGS += -ffast-math -funroll-loops
CFLAGS += -pthread
CFLAGS += -I$(INCDIR)

LDFLAGS += -lm -lrt -pthread
CFLAGS += -DDATA_DIR_DEFAULT='"/mnt/ssd/data"'

all: $(BLDDIR) $(PROG)

$(BLDDIR):
	@mkdir -p $(BLDDIR)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "Build complete: $(PROG)"
	@file $(PROG)

$(BLDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/config.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 安装到RK3568目标设备
install: $(PROG)
	@echo "Copy to RK3568 device:"
	@echo "  scp $(PROG) root@<rk3568-ip>:/opt/rk3568-recorder/"
	@echo "  ssh root@<rk3568-ip> 'mkdir -p /mnt/ssd/data'"
	@echo "  ssh root@<rk3568-ip> '/opt/rk3568-recorder/$(PROG)'"

# 打包部署
deploy: $(PROG)
	mkdir -p deploy/opt/rk3568-recorder
	mkdir -p deploy/etc/systemd/system
	cp $(PROG) deploy/opt/rk3568-recorder/
	cp rk3568-recorder.service deploy/etc/systemd/system/
	tar czf rk3568-recorder-deploy.tar.gz -C deploy .
	@echo "Deploy package: rk3568-recorder-deploy.tar.gz"

clean:
	rm -rf $(BLDDIR) $(PROG) deploy *.tar.gz

.PHONY: all install deploy clean
