/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef LFS_DHARA_PORT_H_
#define LFS_DHARA_PORT_H_

#define WRITE_CHUNK_SIZE      2048          // 1 NAND Page


static int lfs_dhara_sync(const struct lfs_config *c);
static int lfs_dhara_erase(const struct lfs_config *c, lfs_block_t block);
static int lfs_dhara_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);
static int lfs_dhara_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);

void lfs_persistence_test(lfs_t *lfs_ptr);
void lfs_stress_test(lfs_t *lfs_ptr);
void lfs_dhara_configure(void);
void lfs_performance_test(lfs_t *lfs_ptr) ;

#endif /* LFS_DHARA_PORT_H_ */
