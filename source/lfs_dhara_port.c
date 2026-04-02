/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "lfs.h"
#include "dhara_map.h"
#include "dhara_port.h"
#include "fsl_debug_console.h"
#include <string.h>
#include "lfs_dhara_port.h"
#include "fsl_common.h" 
#include "FreeRTOS.h"
#include "task.h"
#define LFS_BLOCK_SIZE    2048  // Treated as 1 Dhara Sector

extern struct dhara_map dhara;

/* Static buffers for LittleFS to avoid stack allocation */
static uint8_t lfs_read_buf[NAND_PAGE_SIZE];
static uint8_t lfs_prog_buf[NAND_PAGE_SIZE];
static uint8_t lfs_lookahead_buf[128];

static int lfs_dhara_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    dhara_error_t err;
    // off will be 0 because block_size == read_size
    if (dhara_map_read(&dhara, (uint32_t)block, buffer, &err) < 0) return LFS_ERR_IO;
    return 0;
}

static int lfs_dhara_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    dhara_error_t err;
    if (dhara_map_write(&dhara, (uint32_t)block, buffer, &err) < 0) return LFS_ERR_IO;
    return 0;
}

static int lfs_dhara_erase(const struct lfs_config *c, lfs_block_t block)
{
    /* FTL handles erases internally via Garbage Collection.
       Returning 0 lets LFS know the logical block is 'cleared'. */
    return 0;
}

static int lfs_dhara_sync(const struct lfs_config *c)
{
    dhara_error_t err;
    // Commit Dhara journal to physical flash
    return (dhara_map_sync(&dhara, &err) < 0) ? LFS_ERR_IO : 0;
}

struct lfs_config lfs_dhara_cfg = {
    .read = lfs_dhara_read,
    .prog = lfs_dhara_prog,
    .erase = lfs_dhara_erase,
    .sync = lfs_dhara_sync,
    .read_size = NAND_PAGE_SIZE,
    .prog_size = NAND_PAGE_SIZE,
    .block_size = LFS_BLOCK_SIZE,
    .cache_size = NAND_PAGE_SIZE,
    .lookahead_size = sizeof(lfs_lookahead_buf),
    .read_buffer = lfs_read_buf,
    .prog_buffer = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead_buf,
    .block_cycles = 500,
};

void lfs_dhara_configure(void)
{
    lfs_dhara_cfg.block_count = dhara_map_capacity(&dhara);
    PRINTF("[LFS] Ready: %u blocks (%u KB usable)\r\n",
            lfs_dhara_cfg.block_count, (lfs_dhara_cfg.block_count * 2048) / 1024);
}


void lfs_stress_test(lfs_t *lfs_ptr)
{
    lfs_file_t file;
    char filename[16];
    char write_buf[64];
    char read_buf[64];
    int err;
    const int num_files = 50;

    PRINTF("\r\n--- Starting LFS Stress Test ---\r\n");

    // 1. Create and Write files
    for (int i = 0; i < num_files; i++) {
        sprintf(filename, "test_%d.txt", i);
        sprintf(write_buf, "Hello! This is data for file number %d", i);

        err = lfs_file_open(lfs_ptr, &file, filename, LFS_O_WRONLY | LFS_O_CREAT);
        if (err < 0) {
            PRINTF("Error opening %s for write: %d\r\n", filename, err);
            return;
        }

        lfs_file_write(lfs_ptr, &file, write_buf, strlen(write_buf));
        lfs_file_close(lfs_ptr, &file);

        if (i % 10 == 0) PRINTF("Written %d files...\r\n", i);
    }

    // 2. Read and Verify
    PRINTF("Verifying data...\r\n");
    for (int i = 0; i < num_files; i++) {
        sprintf(filename, "test_%d.txt", i);
        sprintf(write_buf, "Hello! This is data for file number %d", i);

        lfs_file_open(lfs_ptr, &file, filename, LFS_O_RDONLY);
        memset(read_buf, 0, sizeof(read_buf));
        lfs_file_read(lfs_ptr, &file, read_buf, sizeof(read_buf));
        lfs_file_close(lfs_ptr, &file);

        if (strcmp(read_buf, write_buf) != 0) {
            PRINTF("DATA MISMATCH in file %s!\r\n", filename);
            PRINTF("Expected: %s\r\n", write_buf);
            PRINTF("Got:      %s\r\n", read_buf);
            return;
        }
    }
    PRINTF("Verification successful!\r\n");

    // 3. Clean up (Forces Garbage Collection eventually)
    PRINTF("Deleting files to trigger Dhara recovery...\r\n");
    for (int i = 0; i < num_files; i++) {
        sprintf(filename, "test_%d.txt", i);
        lfs_remove(lfs_ptr, filename);
    }

    // 4. Final Sync
    lfs_dhara_sync(&lfs_dhara_cfg);
    PRINTF("--- Stress Test Passed! ---\r\n\r\n");
}

void lfs_persistence_test(lfs_t *lfs_ptr)
{
    lfs_file_t file;
    uint32_t boot_count = 0;

    // 1. Read the current count
    int err = lfs_file_open(lfs_ptr, &file, "boot_stat.dat", LFS_O_RDWR | LFS_O_CREAT);
    if (err >= 0) {
        lfs_file_read(lfs_ptr, &file, &boot_count, sizeof(boot_count));

        // 2. Increment and overwrite
        boot_count++;
        lfs_file_rewind(lfs_ptr, &file);
        lfs_file_write(lfs_ptr, &file, &boot_count, sizeof(boot_count));
        lfs_file_close(lfs_ptr, &file);

        PRINTF("\r\n>>>> BOOT NUMBER: %u <<<<\r\n", boot_count);
    } else {
        PRINTF("Persistence file error: %d\r\n", err);
    }
    /* --- RESET LOGIC START ---
           Uncomment the line below and run once to reset the count to zero.
           After running it once, comment it back out.
        */
//         lfs_remove(lfs_ptr, "boot_stat.dat");
        /* --- RESET LOGIC END --- */    // 3. Sync Dhara to ensure the journal is saved before you hit RESET
    lfs_dhara_sync(&lfs_dhara_cfg);

}

void lfs_performance_test(lfs_t *lfs_ptr) {
    lfs_file_t file;
    static uint8_t data_buf[2048];

    // Test size: 512 KB
    const uint32_t total_bytes = 512 * 1024;
    const uint32_t total_kb = 512;

    TickType_t start_tick, end_tick;
    uint32_t time_ms, speed_kbps;

    PRINTF("\r\n--- NAND Performance Benchmark (%u KB) ---\r\n", total_kb);

    // Fill buffer with pattern
    memset(data_buf, 0x55, sizeof(data_buf));

    /* --- 1. WRITE TEST --- */
    start_tick = xTaskGetTickCount();

    int err = lfs_file_open(lfs_ptr, &file, "perf.bin", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) { PRINTF("Open failed: %d\r\n", err); return; }

    for (int i = 0; i < (total_bytes / 2048); i++) {
        lfs_file_write(lfs_ptr, &file, data_buf, 2048);
    }
    lfs_file_close(lfs_ptr, &file);
    lfs_dhara_sync(&lfs_dhara_cfg);

    end_tick = xTaskGetTickCount();

    time_ms = (end_tick - start_tick) * portTICK_PERIOD_MS;
    if (time_ms == 0) time_ms = 1; // Prevent divide by zero
    speed_kbps = (total_kb * 1000) / time_ms;

    PRINTF("WRITE: %u KB in %u ms (%u KB/s)\r\n", total_kb, time_ms, speed_kbps);

    /* --- 2. READ TEST --- */
    start_tick = xTaskGetTickCount();

    lfs_file_open(lfs_ptr, &file, "perf.bin", LFS_O_RDONLY);
    for (int i = 0; i < (total_bytes / 2048); i++) {
        lfs_file_read(lfs_ptr, &file, data_buf, 2048);
    }
    lfs_file_close(lfs_ptr, &file);

    end_tick = xTaskGetTickCount();

    time_ms = (end_tick - start_tick) * portTICK_PERIOD_MS;
    if (time_ms == 0) time_ms = 1;
    speed_kbps = (total_kb * 1000) / time_ms;

    PRINTF("READ : %u KB in %u ms (%u KB/s)\r\n", total_kb, time_ms, speed_kbps);

    /* --- 3. DELETE TEST --- */
    start_tick = xTaskGetTickCount();

    lfs_remove(lfs_ptr, "perf.bin");
    lfs_dhara_sync(&lfs_dhara_cfg);

    end_tick = xTaskGetTickCount();
    time_ms = (end_tick - start_tick) * portTICK_PERIOD_MS;

    PRINTF("DELETE: Completed in %u ms\r\n", time_ms);
}


