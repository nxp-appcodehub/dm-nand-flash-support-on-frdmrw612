/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#include "FreeRTOS.h"
#include "task.h"
#include "app.h"
#include "fsl_debug_console.h"
#include "snand.h"
#include "snand_memory.h"

/* DHARA includes */
#include "dhara_map.h"
#include "dhara_port.h"
#include "lfs.h"
#include "lfs_dhara_port.h"
/*******************************************************************************
 * Globals
 ******************************************************************************/


static void snand_configuration_task(void *pvParameters);

lfs_t lfs;
extern struct lfs_config lfs_dhara_cfg;
volatile int lfs_formatting = 0;

extern int dhara_init_and_resume(void);
/*******************************************************************************
 * DHARA init + resume
 ******************************************************************************/

/*******************************************************************************
 * Main / task
 ******************************************************************************/
int main(void)
{
    BOARD_InitHardware();

    if (xTaskCreate(snand_configuration_task,
                    "snand",
                    2048,
                    NULL,
                    configMAX_PRIORITIES - 1,
                    NULL) != pdPASS)
    {
        PRINTF("Task creation failed\r\n");
        while (1);
    }

    vTaskStartScheduler();
    while (1);
}

static void snand_configuration_task(void *pvParameters)
{
    status_t status;

    status = BOARD_InitSNand();
    if (status != kStatus_Success)
    {
        PRINTF("BOARD_InitSNand failed!\r\n");
        vTaskSuspend(NULL);
    }
    serial_nand_config_option_t nandOpt =
    {
        .option0.U = 0xc1010021,
        .option1.U = 0x000000ef,
    };

    status = spinand_mem_config((uint32_t *)&nandOpt);
    if (status != kStatus_Success)
    {
        PRINTF("spinand_mem_config failed!\r\n");
        vTaskSuspend(NULL);
    }

    PRINTF("\r\nSNAND: Target FRDM-RW612\r\n");
    PRINTF("-------------------------------------\r\n");

    if (dhara_init_and_resume() != 0)
    {
        PRINTF("[DHARA] init failed\r\n");
        vTaskSuspend(NULL);
    }

    PRINTF("-------------------------------------\r\n");
    PRINTF("DHARA bring-up successful\r\n");


    /* Finalize config */
	lfs_dhara_configure();

	/* Attempt to mount first */
	int ret = lfs_mount(&lfs, &lfs_dhara_cfg);

	/* If mount fails, it's likely a blank chip; format it then */
	if (ret < 0) {
		PRINTF("[LFS] Mount failed (%d). Formatting...\r\n", ret);
		ret = lfs_format(&lfs, &lfs_dhara_cfg);
		if (ret < 0) {
			PRINTF("[LFS] Format failed (%d)\r\n", ret);
			vTaskSuspend(NULL);
		}
		ret = lfs_mount(&lfs, &lfs_dhara_cfg);
		if (ret < 0) {
			PRINTF("[LFS] Second mount failed (%d)\r\n", ret);
			vTaskSuspend(NULL);
		}
	}

        PRINTF("[LFS] mount OK\r\n");

        /* Run the Stress Test */
        lfs_stress_test(&lfs);
        lfs_persistence_test(&lfs);
        lfs_performance_test(&lfs);
}
