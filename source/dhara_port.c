/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#include "dhara_port.h"
#include "dhara_map.h"
#include "snand_memory.h"
#include <string.h>

/* Ensure these are NOT static in your snand_memory.c */

struct dhara_nand rw612_dhara_nand = {
    .log2_page_size = 11, // 2048
    .log2_ppb = 6,       // 64
    .num_blocks = 1024,
};
struct dhara_map dhara;

/* MUST be page-sized AND aligned */
__attribute__((aligned(32)))
static uint8_t dhara_page_buf[NAND_PAGE_SIZE];
static uint8_t w[NAND_PAGE_SIZE];
static uint8_t r[NAND_PAGE_SIZE];


int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t b) {
    return is_bad_block_in_dbbt(b) ? 1 : 0;
}

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t b) {
    bad_block_discovered(b);
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b, dhara_error_t *err) {
    if (spinand_memory_erase_and_verify(b) != kStatus_Success) {
        if (err) *err = DHARA_E_BAD_BLOCK;
        return -1;
    }
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data, dhara_error_t *err) {
    // p is absolute page index
    if (spinand_memory_write_and_verify(p, NAND_PAGE_SIZE, (uint8_t*)data) != kStatus_Success) {
        if (err) *err = DHARA_E_BAD_BLOCK;
        return -1;
    }
    return 0;
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p, size_t off, size_t len, uint8_t *data, dhara_error_t *err) {
    static uint8_t pg_buf[NAND_PAGE_SIZE];
    if (spinand_memory_read_page(p, NAND_PAGE_SIZE, pg_buf) != kStatus_Success) {
        if (err) *err = DHARA_E_ECC;
        return -1;
    }
    memcpy(data, pg_buf + off, len);
    return 0;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t p) {
    static uint8_t buf[NAND_PAGE_SIZE];
    if (spinand_memory_read_page(p, NAND_PAGE_SIZE, buf) != kStatus_Success) return 0;
    for (int i = 0; i < NAND_PAGE_SIZE; i++) if (buf[i] != 0xFF) return 0;
    return 1;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst, dhara_error_t *err) {
    static uint8_t buf[NAND_PAGE_SIZE];
    if (spinand_memory_read_page(src, NAND_PAGE_SIZE, buf) != kStatus_Success) return -1;
    return dhara_nand_prog(n, dst, buf, err);
}

int dhara_init_and_resume(void)
{
    dhara_error_t err;
    int ret;

    dhara_map_init(&dhara,
                   &rw612_dhara_nand,
                   dhara_page_buf,
                   NAND_PAGE_SIZE);

    ret = dhara_map_resume(&dhara, &err);
    if (ret < 0)
    {
        PRINTF("[DHARA] resume failed, clearing map (err=%d)\r\n", err);
        dhara_map_clear(&dhara);
    }

    PRINTF("[DHARA] init/resume OK\r\n");
    return 0;
}

/*******************************************************************************
 * DHARA self-test
 ******************************************************************************/
int dhara_self_test(void)
{
    dhara_error_t err;

    memset(w, 0x5A, sizeof(w));
    memset(r, 0x00, sizeof(r));

    PRINTF("[DHARA][TEST] write sector 0\r\n");
    if (dhara_map_write(&dhara, 0, w, &err) < 0)
    {
        PRINTF("[DHARA][TEST] write failed (err=%d)\r\n", err);
        return -1;
    }

    PRINTF("[DHARA][TEST] read sector 0\r\n");
    if (dhara_map_read(&dhara, 0, r, &err) < 0)
    {
        PRINTF("[DHARA][TEST] read failed (err=%d)\r\n", err);
        return -1;
    }

    if (memcmp(w, r, sizeof(w)) != 0)
    {
        PRINTF("[DHARA][TEST] data mismatch\r\n");
        return -1;
    }

    PRINTF("[DHARA][TEST] PASS\r\n");
    return 0;
}

