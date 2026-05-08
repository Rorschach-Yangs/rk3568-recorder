#include "storage.h"
#include "ptp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

struct storage_ctx {
    char     data_dir[256];
    char     current_file[512];
    int      fd;                       // 当前写入文件句柄
    uint64_t current_file_size;       // 当前文件已写字节数
    uint64_t current_file_index;      // 当前文件序号
    uint64_t total_files;
    uint64_t total_bytes_written;
    uint64_t write_start_ns;
    uint64_t bytes_since_sample;
    uint64_t speed_sample_time_ns;
    double   current_speed_mbps;
};

storage_ctx_t* storage_init(const char *data_dir)
{
    storage_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    strncpy(ctx->data_dir, data_dir, sizeof(ctx->data_dir) - 1);

    // 创建数据目录
    mkdir(data_dir, 0755);

    // 扫描已有文件,恢复文件序号
    DIR *d = opendir(data_dir);
    if (d) {
        struct dirent *ent;
        uint64_t max_idx = 0;
        ctx->total_files = 0;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, STORAGE_FILE_PREFIX)) {
                ctx->total_files++;
                // 解析序号
                const char *p = ent->d_name + strlen(STORAGE_FILE_PREFIX);
                uint64_t idx = strtoull(p, NULL, 10);
                if (idx > max_idx) max_idx = idx;
            }
        }
        closedir(d);
        ctx->current_file_index = max_idx + 1;
    }

    // 打开第一个文件
    snprintf(ctx->current_file, sizeof(ctx->current_file),
             "%s/%s%06lu.bin", ctx->data_dir,
             STORAGE_FILE_PREFIX, ctx->current_file_index);

    // 使用O_DIRECT绕过页缓存(模拟DMA→SATA直写路径,实际产品用)
    // 如果O_DIRECT失败则回退到普通写
    ctx->fd = open(ctx->current_file,
                   O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT,
                   0644);
    if (ctx->fd < 0) {
        // O_DIRECT需要对齐,失败时回退
        ctx->fd = open(ctx->current_file,
                       O_WRONLY | O_CREAT | O_TRUNC | O_SYNC,
                       0644);
    }
    if (ctx->fd < 0) {
        perror("open storage file");
        free(ctx);
        return NULL;
    }

    ctx->current_file_size = 0;
    ctx->total_bytes_written = 0;
    ctx->write_start_ns = 0;
    ctx->bytes_since_sample = 0;
    ctx->current_speed_mbps = 0.0;
    ctx->speed_sample_time_ns = 0;

    printf("[STORAGE] Data directory: %s\n", ctx->data_dir);
    printf("[STORAGE] Current file: %s\n", ctx->current_file);
    printf("[STORAGE] SATA SSD write engine ready (target: %d MB/s)\n",
           STORAGE_TARGET_SPEED);
    return ctx;
}

void storage_destroy(storage_ctx_t *ctx)
{
    if (!ctx) return;
    storage_flush(ctx);
    if (ctx->fd >= 0) close(ctx->fd);
    printf("[STORAGE] Shutdown: %lu files, %.2f GB written\n",
           (unsigned long)ctx->total_files,
           (double)ctx->total_bytes_written / (1024.0 * 1024.0 * 1024.0));
    free(ctx);
}

int storage_write_block(storage_ctx_t *ctx, const adc_data_block_t *blk)
{
    if (!ctx || ctx->fd < 0) return -1;

    size_t block_size = sizeof(adc_data_block_t);
    ssize_t written = write(ctx->fd, blk, block_size);

    if (written < 0) {
        perror("write block");
        return -1;
    }

    ctx->current_file_size += written;
    ctx->total_bytes_written += written;
    ctx->bytes_since_sample += written;

    // 更新写入速度统计
    uint64_t now = ptp_get_timestamp_ns();
    if (ctx->speed_sample_time_ns == 0) {
        ctx->speed_sample_time_ns = now;
        ctx->bytes_since_sample = 0;
    } else {
        uint64_t elapsed_ns = now - ctx->speed_sample_time_ns;
        if (elapsed_ns > 500000000ULL) {  // 每500ms更新速度
            double elapsed_s = (double)elapsed_ns / 1e9;
            ctx->current_speed_mbps =
                (double)ctx->bytes_since_sample / (1024.0 * 1024.0) / elapsed_s;
            ctx->speed_sample_time_ns = now;
            ctx->bytes_since_sample = 0;
        }
    }

    // 文件达到上限时轮转
    if (ctx->current_file_size >= STORAGE_FILE_MAX_SIZE) {
        close(ctx->fd);
        ctx->current_file_index++;
        ctx->total_files++;
        ctx->current_file_size = 0;

        snprintf(ctx->current_file, sizeof(ctx->current_file),
                 "%s/%s%06lu.bin", ctx->data_dir,
                 STORAGE_FILE_PREFIX, ctx->current_file_index);

        ctx->fd = open(ctx->current_file,
                       O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        if (ctx->fd < 0) {
            ctx->fd = open(ctx->current_file,
                           O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
        }

        printf("[STORAGE] Rotated to: %s\n", ctx->current_file);
    }

    return 0;
}

int storage_check_and_rotate(storage_ctx_t *ctx)
{
    // 检查存储目录总大小,超出限制时删除最旧文件
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "du -sb %s 2>/dev/null | cut -f1", ctx->data_dir);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    uint64_t total_size = 0;
    if (fscanf(fp, "%lu", &total_size) != 1) total_size = 0;
    pclose(fp);

    uint64_t max_bytes = (uint64_t)STORAGE_MAX_SIZE_GB * 1024 * 1024 * 1024;

    if (total_size > max_bytes) {
        printf("[STORAGE] Space limit reached (%.1f GB), starting circular cleanup...\n",
               (double)total_size / (1024.0*1024.0*1024.0));

        // 获取最旧的文件并删除
        snprintf(cmd, sizeof(cmd),
                 "ls -t %s/%s*.bin 2>/dev/null | tail -n 10",
                 ctx->data_dir, STORAGE_FILE_PREFIX);

        fp = popen(cmd, "r");
        if (fp) {
            char fname[512];
            while (fgets(fname, sizeof(fname), fp)) {
                fname[strcspn(fname, "\n")] = 0;
                if (strcmp(fname, ctx->current_file) != 0) {
                    unlink(fname);
                    printf("[STORAGE] Deleted old file: %s\n", fname);
                }
            }
            pclose(fp);
        }
    }
    return 0;
}

void storage_get_stats(storage_ctx_t *ctx, uint64_t *total_files,
                       uint64_t *total_bytes, double *speed_mbps)
{
    if (total_files) *total_files = ctx->total_files;
    if (total_bytes) *total_bytes = ctx->total_bytes_written;
    if (speed_mbps) *speed_mbps = ctx->current_speed_mbps;
}

int storage_flush(storage_ctx_t *ctx)
{
    if (ctx && ctx->fd >= 0) {
        // fdatasync确保数据和元数据落盘(掉电保护)
        return fdatasync(ctx->fd);
    }
    return -1;
}
