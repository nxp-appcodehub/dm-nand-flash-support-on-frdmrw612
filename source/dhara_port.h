/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#ifndef DHARA_PORT_H
#define DHARA_PORT_H

#include <stdint.h>
#include "dhara_nand.h"
#include "fsl_debug_console.h"

/* -------------------------------------------------------------------------- */
/* W25N01GV geometry                                                          */
/* -------------------------------------------------------------------------- */


#define NAND_PAGE_SIZE          2048
#define NAND_PAGES_PER_BLOCK    64
#define NAND_BLOCK_SIZE         (NAND_PAGE_SIZE * NAND_PAGES_PER_BLOCK)

#define NAND_TOTAL_BLOCKS       1024
#define NAND_RESERVED_BLOCKS    24U   /* margin for factory bad blocks + wear */

#define DHARA_USABLE_BLOCKS     (NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS)

/*
 * Export the NAND descriptor instance used by DHARA.
 * This describes geometry only (classic DHARA API).
 */
extern struct dhara_nand rw612_dhara_nand;

int dhara_self_test(void);
int dhara_init_and_resume(void);

#endif /* DHARA_PORT_H */
