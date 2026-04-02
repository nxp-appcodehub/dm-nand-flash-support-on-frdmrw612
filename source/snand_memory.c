/*
 * Copyright (c) 2014-2015 Freescale Semiconductor, Inc.
 * Copyright 2016-2020,2026 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "snand_memory.h"
#include "fsl_device_registers.h"
#include "snand_flash.h"
#include <string.h>
#include "fsl_debug_console.h"
////////////////////////////////////////////////////////////////////////////////
// Definitions
////////////////////////////////////////////////////////////////////////////////

enum
{
    kFlashDefaultPattern = 0xFF,
};

enum
{
    kSPINANDStartAddress = 0,
};

enum
{
    kNandAddressType_ByteAddress = 0,
    kNandAddressType_BlockAddress = 1,
};

//! @brief SPI NAND memory feature inforamation
//!
//! An instance is maintained in this file, will is used to keep key information for write and flush
//! operatations.
typedef struct _spinand_mem_context
{
    bool isConfigured;
    uint32_t nandAddressType;
    uint32_t startBlockId;
    bool readwriteInProgress;
    uint32_t readBuffer[kSpiNandMemory_MaxPageSize / sizeof(uint32_t)];
    bool isReadBufferValid;
    uint32_t readBufferPageAddr;
    uint32_t writeBuffer[kSpiNandMemory_MaxPageSize / sizeof(uint32_t)];
    bool isWriteBufferValid;
    uint32_t writeBufferOffset;
    uint32_t writeBufferPageAddr;
    uint32_t skippedBlockCount;
    uint32_t instance;
} spinand_mem_context_t;

////////////////////////////////////////////////////////////////////////////////
// Prototypes
////////////////////////////////////////////////////////////////////////////////
 status_t spinand_mem_load_buffer(uint32_t pageAddr);

 status_t spinand_mem_flush_buffer(void);

 status_t spinand_mem_block_backup(uint32_t srcPageAddr, uint32_t destBlockAddr);

#if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
 bool is_erased_memory(uint32_t pageAddr, uint32_t pageCount);
#endif

 status_t spinand_memory_read(uint32_t pageAddr, uint32_t length, uint8_t *buffer);

 status_t spinand_memory_write_and_verify(uint32_t pageAddr, uint32_t length, uint8_t *buffer);

 status_t spinand_memory_erase_and_verify(uint32_t blockAddr);

 status_t spinand_memory_spi_init(flexspi_nand_config_t *config);

 status_t spinand_memory_read_page(uint32_t pageAddr, uint32_t length, uint8_t *buffer);

 status_t spinand_memory_program_page(uint32_t pageAddr, uint32_t length, uint8_t *buffer);

 status_t spinand_memory_erase_block(uint32_t blockAddr);

 bool is_spinand_configured(void);

 status_t spinand_mem_creat_empty_dbbt(void);

 bool is_bad_block_in_dbbt(uint32_t blockAddr);

 status_t bad_block_discovered(uint32_t blockAddr);

 status_t skip_bad_blocks(uint32_t *pageAddr);

 bool need_to_check_dbbt_before_read(uint32_t blockAddr);

 bool need_to_check_dbbt_before_write(uint32_t blockAddr);

 bool is_read_page_cached(uint32_t pageAddr);

 bool is_write_page_cached(uint32_t pageAddr);

 status_t nand_generate_fcb(spinand_fcb_t *fcb, serial_nand_config_option_t *option);

////////////////////////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////////////////////////

 spinand_fcb_t s_spinandFcb;

 spinand_dbbt_t s_spinandDbbt;

 spinand_mem_context_t s_spinandContext = {
    .isConfigured = false,
    .skippedBlockCount = 0,
    .isReadBufferValid = false,
    .isWriteBufferValid = false,
    .instance = 0,
};

////////////////////////////////////////////////////////////////////////////////
// Code
////////////////////////////////////////////////////////////////////////////////

// See qspi_memory.h for documentation on this function.
enum {
    LUT_IDX_PAGELOAD = 0,    // 0x13 command
    LUT_IDX_READCACHE,       // 0x03 command
    LUT_IDX_WRITEENABLE,
    LUT_IDX_PROGRAMLOAD,
    LUT_IDX_PROGRAMEXEC,
    LUT_IDX_ERASEBLOCK,
    LUT_IDX_READSTATUS,
    LUT_IDX_READID,
};


status_t BOARD_VerifyNandID(void) // Assuming this is called by BOARD_InitSNand
{
    status_t status;
    // We allocate 4 bytes to align with 32-bit transfer requirements and capture padding.
    uint8_t jedecId[4] = {0};

    // We use flexspi_xfer_t structure, assuming FLEXSPI_TransferBlocking or a similar SDK API is available.
    flexspi_transfer_t xfer = {0};
    xfer.deviceAddress = 0;
    xfer.port          = kFLEXSPI_PortB1;
    xfer.cmdType       = kFLEXSPI_Read; // Read operation
    xfer.seqIndex      = LUT_IDX_READID; // LUT entry for 0x9F command
    xfer.SeqNumber     = 1;

    // Crucial change: We read 4 bytes, even though the ID is 3 bytes long, to avoid buffer issues.
    xfer.data          = (uint32_t *)jedecId;
    xfer.dataSize      = 4;

    // Assuming FLEXSPI_TransferBlocking is available (or using xfer2 as in your driver)
    status = FLEXSPI_TransferBlocking(FLEXSPI, &xfer);

    if (status == kStatus_Success)
    {
        // 1. Check for expected Winbond ID (EF) at the start of the ID data (Byte 1)
        // Note: The buffer may contain 0x00, 0xEF, 0xAA, 0x21 due to the command structure.
        // We look for the 0xEF value in the buffer. Given your prior FF EF AA output,
        // the FF is the first byte (index 0).

        // We will shift the printout by one byte to skip the initial dummy/offset byte (FF).
        snand_printf("W25N01GV JEDEC ID: %02X %02X %02X\r\n", jedecId[1], jedecId[2], jedecId[3]);

        // Final verification check (Optional, but robust):
        if (jedecId[1] == 0xEF && jedecId[2] == 0xAA && jedecId[3] == 0x21) {
             // Success
        } else {
             // Handle ID Mismatch
        }
    }
    else
    {
    	snand_printf("SNAND JEDEC ID read failed: %d\r\n", status);
    }

    return status;
}
status_t BOARD_InitSNand(void)
{
    status_t status;
    flexspi_config_t config;

    CLOCK_EnableClock(kCLOCK_Flexspi);
    RESET_ClearPeripheralReset(kFLEXSPI_RST_SHIFT_RSTn);


    /* FlexSPI root clock: 320 MHz / 6 = ~53.3 MHz (LOWERED CLOCK FOR STABILITY) */
    BOARD_SetFlexspiClock(FLEXSPI, 5U, 3U);

    FLEXSPI_GetDefaultConfig(&config);
    config.rxSampleClock = kFLEXSPI_ReadSampleClkLoopbackInternally;
    config.ahbConfig.enableAHBPrefetch   = true;
    config.ahbConfig.enableAHBBufferable = true;
    config.ahbConfig.enableAHBCachable   = true;

    FLEXSPI_Init(FLEXSPI, &config);

    flexspi_device_config_t nandConfig = {

    		.flexspiRootClk    = 106000000,
			.isSck2Enabled     = false,
			.flashSize         = 0x8000,
			.CSIntervalUnit    = kFLEXSPI_CsIntervalUnit1SckCycle,
			.CSInterval        = 2,
			.CSHoldTime        = 3,
			.CSSetupTime       = 3,
			.dataValidTime     = 1,
			.columnspace       = 0,
			.enableWordAddress = false,
			.AWRSeqIndex       = 0,
			.AWRSeqNumber      = 1,
			.ARDSeqIndex       = 0,
			.ARDSeqNumber      = 1,
			.enableWriteMask   = false,
    };

    FLEXSPI_SetFlashConfig(FLEXSPI, &nandConfig, kFLEXSPI_PortB1);

    /* --- Complete W25N01GV LUT --- */
    enum
    {
        LUT_IDX_PAGELOAD = 0,
        LUT_IDX_READCACHE,
        LUT_IDX_WRITEENABLE,
        LUT_IDX_PROGRAMLOAD,
        LUT_IDX_PROGRAMEXEC,
        LUT_IDX_ERASEBLOCK,
        LUT_IDX_READSTATUS,
        LUT_IDX_READID,
    };

    const uint32_t w25n01gv_lut[64] = {
        /* 0: PAGE LOAD (0x13 + 3-byte address) */
        [4 * LUT_IDX_PAGELOAD] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x13,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 24),

        /* 1: READ CACHE (0x03) - Phase 1: CMD (0x03) + Column Address (16 bits) */
        [4 * LUT_IDX_READCACHE] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x03,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 16),

        /* 1: READ CACHE (0x03) - Phase 2: 8 Dummy Clocks + Read Data */
        [4 * LUT_IDX_READCACHE + 1] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_DUMMY_SDR, kFLEXSPI_1PAD, 8,
                            kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x80),

        /* 2: WRITE ENABLE (0x06) */
        [4 * LUT_IDX_WRITEENABLE] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x06,
                            kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),

        /* 3: PROGRAM LOAD (0x02 + 2-byte column) */
        [4 * LUT_IDX_PROGRAMLOAD] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x02,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 16),
        [4 * LUT_IDX_PROGRAMLOAD + 1] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_WRITE_SDR, kFLEXSPI_1PAD, 0x04,
                            kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),

        /* 4: PROGRAM EXEC (0x10 + 3-byte addr) */
        [4 * LUT_IDX_PROGRAMEXEC] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x10,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 24),

        /* 5: BLOCK ERASE (0xD8 + 3-byte addr) */
        [4 * LUT_IDX_ERASEBLOCK] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0xD8,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 24),

        /* 6: READ STATUS (0x0F + 1-byte reg addr + read 1 byte) */
        [4 * LUT_IDX_READSTATUS] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x0F,
                            kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_1PAD, 8),

        /* 7: READ ID (0x9F + 3-byte read) */
        [4 * LUT_IDX_READID] =
            FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR,  kFLEXSPI_1PAD, 0x9F,
                            kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x04),
    };
    FLEXSPI_UpdateLUT(FLEXSPI, 0, w25n01gv_lut, ARRAY_SIZE(w25n01gv_lut));
    FLEXSPI_SoftwareReset(FLEXSPI);

    BOARD_VerifyNandID();

    return kStatus_Success;
}
status_t spinand_mem_init(void)
{
    status_t status;

    // Load default config block from efuse.
    status = flexspi_nand_get_default_cfg_blk(&s_spinandFcb.config);
    if (status != kStatus_Success)
    {
        return status;
    }

    // Init SPI peripheral to enable fcb read.
    status = spinand_memory_spi_init(&s_spinandFcb.config);
    if (status != kStatus_Success)
    {
        return status;
    }

//    status = spinand_mem_load_dbbt(&s_spinandFcb);
//    if (status != kStatus_Success)
//    {
//        // Do not create a new DBBT during init.
//        // There is a risk, that the old DBBT is crashed.
//        // User needs to re-configure.
//        return kStatus_Fail;
//    }

    s_spinandContext.isConfigured = true;
    return kStatus_Success;
}

status_t nand_generate_fcb(spinand_fcb_t *fcb, serial_nand_config_option_t *option)
{
    status_t status = kStatus_InvalidArgument;

    do
    {
        if ((fcb == NULL) || (option == NULL))
        {
            break;
        }

        memset(fcb, 0, sizeof(spinand_fcb_t));

        status = flexspi_nand_get_config(EXAMPLE_MIXSPI, &fcb->config, option);
        if (status != kStatus_Success)
        {
            break;
        }

        s_spinandContext.nandAddressType = kNandAddressType_ByteAddress;

        status = kStatus_Success;

    } while (0);

    return status;
}

status_t spinand_mem_config(uint32_t *config)
{
    status_t status = kStatus_InvalidArgument;

    bool isNandConfig = false;
    
    serial_nand_config_option_t *option = (serial_nand_config_option_t *)config;

    do
    {
        status = nand_generate_fcb(&s_spinandFcb, option);
        if (status != kStatus_Success)
        {
            break;
        }

        // First, mark SPI NAND as not configured.
        s_spinandContext.isConfigured = false;
        isNandConfig = true;

        if (isNandConfig)
        {
            status = spinand_memory_spi_init(&s_spinandFcb.config);
            if (status != kStatus_Success)
            {
                break;
            }

            spinand_mem_creat_empty_dbbt();
            // All configuration steps are success. SPI NAND can be accessable.
            s_spinandContext.isConfigured = true;
        }
    } while (0);

    return status;
}

// See qspi_memory.h for documentation on this function.
status_t spinand_mem_read(uint32_t address, uint32_t length, uint8_t *restrict buffer)
{
    status_t status = kStatus_InvalidArgument;

    do
    {
        if (buffer == NULL)
        {
            break;
        }

        // SPI NAND should be configured before access.
        if (!is_spinand_configured())
        {
            status = kStatusMemoryNotConfigured;
            break;
        }

        uint32_t pageSize = s_spinandFcb.config.pageDataSize;
        uint32_t columnAddr;
        uint32_t pageAddr;

        if (s_spinandContext.nandAddressType == kNandAddressType_BlockAddress)
        {
            // If the Write transfer is not started yet, log the address as block address (index)
            if (!s_spinandContext.readwriteInProgress)
            {
                // Ensure the block address is a valid block address
                if (address >= s_spinandFcb.config.blocksPerDevice)
                {
                    break;
                }
                s_spinandContext.readwriteInProgress = true;
                s_spinandContext.startBlockId = address;
                columnAddr = 0;
                pageAddr = address * s_spinandFcb.config.pagesPerBlock;
            }
            // Otherwise, need to convert the address to actual physical address
            else
            {
                // Actual physical address caluclation formula: blockId * page size * pages per block + address -
                // blockId
                uint32_t actualAddress = s_spinandContext.startBlockId * s_spinandFcb.config.pagesPerBlock *
                                             s_spinandFcb.config.pageDataSize +
                                         (address - s_spinandContext.startBlockId);
                columnAddr = actualAddress % pageSize;
                pageAddr = actualAddress / pageSize;
            }
        }
        else // Address is actual NAND address
        {
            columnAddr = address % pageSize;
            pageAddr = address / pageSize;
        }

        // Skip the skipped blocks during a read operation.
        // No need to change the columnAddr.
        skip_bad_blocks(&pageAddr);

        uint32_t readLength;

        while (length)
        {
            // Check if current page to read is already read to readbuffer.
            // If no, need to read the whole page to buffer.
            if (!is_read_page_cached(pageAddr))
            {
                // Check if the page to read and cached page is in the same block.
                // If no, need to check if the block is a bad block.
                uint32_t blockAddr = pageAddr / s_spinandFcb.config.pagesPerBlock;
                if (need_to_check_dbbt_before_read(blockAddr))
                {
                    // Due to skipping bad blocks, blockAddr might cross the end boundary.
                    // Need to check the range.
                    if (blockAddr >= s_spinandFcb.config.blocksPerDevice)
                    {
                        status = kStatusMemoryRangeInvalid;
                        break;
                    }
                    // Check if reading a bad block.
                    if (is_bad_block_in_dbbt(blockAddr))
                    {
                        // If yes, skip the bad block and read the next block.
                        s_spinandContext.skippedBlockCount++;
                        pageAddr += s_spinandFcb.config.pagesPerBlock;
                        continue;
                    }
                }

                // Good block and not cached, then read the page to buffer.
                status = spinand_mem_load_buffer(pageAddr);
                if (status != kStatus_Success)
                {
                    break;
                }
            }
            // If it is a read accoss the page, divide it into two steps.
            if (columnAddr + length <= pageSize)
            {
                readLength = length;
            }
            else
            {
                readLength = pageSize - columnAddr;
            }
            uint8_t *p_readBuffer_8 = (uint8_t *)s_spinandContext.readBuffer;
            memcpy(buffer, &p_readBuffer_8[columnAddr], readLength);
            length -= readLength;
            buffer += readLength;
            columnAddr += readLength;
            if (columnAddr >= pageSize)
            {
                columnAddr -= pageSize;
                pageAddr++;
            }
            // Mark current loop is successfully executed.
            status = kStatus_Success;
        }

        // Terminate current transfer if errors happen during transfer
        if (status != kStatus_Success)
        {
            s_spinandContext.readwriteInProgress = false;
        }

    } while (0);

    return status;
}

status_t spinand_mem_write(uint32_t address, uint32_t length, const uint8_t *buffer)
{
    status_t status = kStatus_InvalidArgument;

    do
    {
        if (buffer == NULL)
        {
            break;
        }

        // SPI NAND should be configured before access.
        if (!is_spinand_configured())
        {
            status = kStatusMemoryNotConfigured;
            break;
        }

        uint32_t pageSize = s_spinandFcb.config.pageDataSize;
        uint32_t columnAddr;
        uint32_t pageAddr;
         uint32_t expectedNextActualAddr;
        if (s_spinandContext.nandAddressType == kNandAddressType_BlockAddress)
        {
            // If the Write transfer is not started yet, log the address as block address (index)
            if (!s_spinandContext.readwriteInProgress)
            {
                // Ensure the block address is a valid block address
                if (address >= s_spinandFcb.config.blocksPerDevice)
                {
                    break;
                }
                s_spinandContext.isWriteBufferValid = false;
                s_spinandContext.readwriteInProgress = true;
                s_spinandContext.startBlockId = address;
                columnAddr = 0;
                pageAddr = address * s_spinandFcb.config.pagesPerBlock;
                expectedNextActualAddr = pageAddr * pageSize + length;
            }
            // Otherwise, need to convert the address to actual physical address
            else
            {
                // Actual physical address caluclation formula: blockId * page size * pages per block + address -
                // blockId
                uint32_t actualAddress = s_spinandContext.startBlockId * s_spinandFcb.config.pagesPerBlock *
                                             s_spinandFcb.config.pageDataSize +
                                         (address - s_spinandContext.startBlockId);
                columnAddr = actualAddress % pageSize;
                pageAddr = actualAddress / pageSize;

                // The address is continuous in a transfer, so once the address is not continuous,
                // Flush data in buffer into SPI NAND and then re-start a new transfer
                if (actualAddress != expectedNextActualAddr)
                {
                    if (s_spinandContext.isWriteBufferValid)
                    {
                        status = spinand_mem_flush_buffer();
                        if (status != kStatus_Success)
                        {
                            // Terminate transfer if error occurs.
                            s_spinandContext.readwriteInProgress = false;
                            break;
                        }
                    }
                    s_spinandContext.readwriteInProgress = true;
                    s_spinandContext.startBlockId = address;
                    columnAddr = 0;
                    pageAddr = s_spinandContext.startBlockId * s_spinandFcb.config.pagesPerBlock;
                    actualAddress = pageAddr * pageSize;
                }
                expectedNextActualAddr = actualAddress + length;
            }
        }
        else // Address is actual NAND address
        {
            columnAddr = address % pageSize;
            pageAddr = address / pageSize;
        }

        // Skip the skipped blocks during a read operation.
        // No need to change the columnAddr.
        skip_bad_blocks(&pageAddr);

        uint32_t writeLength;

        while (length)
        {
            // Check if current page to write is already cached to writebuffer.
            // If no, need to init the writebuffer.
            if (!is_write_page_cached(pageAddr))
            {
                uint32_t blockAddr = pageAddr / s_spinandFcb.config.pagesPerBlock;
                // Check if the page to write and cached page is in the same block.
                // If no, need to check if the block is a bad block.
                if (need_to_check_dbbt_before_write(blockAddr))
                {
                    // Due to skipping bad blocks, blockAddr might cross the end boundary.
                    // Need to check the range.

                    if (blockAddr >= s_spinandFcb.config.blocksPerDevice)
                    {
                        status = kStatusMemoryRangeInvalid;
                        break;
                    }
                    // Check if writting a bad block.
                    if (is_bad_block_in_dbbt(blockAddr))
                    {
                        // If yes, skip the bad block and write to the next block.
                        s_spinandContext.skippedBlockCount++;
                        pageAddr += s_spinandFcb.config.pagesPerBlock;
                        continue;
                    }
                }
                // There is data already cached in the buffer, flush it to SPI NAND.
                if (s_spinandContext.isWriteBufferValid)
                {
                    status = spinand_mem_flush_buffer();
                    if (status != kStatus_Success)
                    {
                        break;
                    }
                }
                // Start a new page write. The address must page size aligned.
                if (columnAddr != 0)
                {
                    status = kStatus_FlexSPINAND_WriteAlignmentError;
                    break;
                }
                s_spinandContext.writeBufferOffset = columnAddr;
                s_spinandContext.writeBufferPageAddr = pageAddr;
                s_spinandContext.isWriteBufferValid = true;
            }

            // If the address is not continuous, start a new page write.
            if (s_spinandContext.writeBufferOffset != columnAddr)
            {
                status = spinand_mem_flush_buffer();
                if (status != kStatus_Success)
                {
                    break;
                }
                continue;
            }

            if (columnAddr + length <= pageSize)
            {
                writeLength = length;
            }
            else
            {
                writeLength = pageSize - columnAddr;
            }
            uint8_t *p_writeBuffer_8 = (uint8_t *)s_spinandContext.writeBuffer;
            memcpy(&p_writeBuffer_8[columnAddr], buffer, writeLength);
            s_spinandContext.writeBufferOffset += writeLength;
            length -= writeLength;
            buffer += writeLength;
            columnAddr += writeLength;
            if (columnAddr >= pageSize)
            {
                columnAddr -= pageSize;
                pageAddr++;
            }
            // Mark current loop is successfully executed.
            status = kStatus_Success;
        }

        // Terminate current transfer if errors happen during transfer
        if (status != kStatus_Success)
        {
            s_spinandContext.readwriteInProgress = false;
        }
    } while (0);

    return status;
}

status_t spinand_mem_flush(void)
{
    status_t status = kStatus_Success;
    // If there still is data in the buffer, then flush them to SPI NAND.
    if (s_spinandContext.isWriteBufferValid)
    {
        status = spinand_mem_flush_buffer();
    }
    return status;
}

// See qspi_memory.h for documentation on this function.
status_t spinand_mem_finalize(void)
{
    status_t status = kStatus_Success;

    // Mark buffer to invalid.
    s_spinandContext.isWriteBufferValid = false;
    s_spinandContext.isReadBufferValid = false;
    // A read / write operation is finished. Clear the skipped block count.
    s_spinandContext.skippedBlockCount = 0;

    s_spinandContext.readwriteInProgress = false;

    return status;
}

// See qspi_memory.h for documentation on this function.
status_t spinand_mem_erase(uint32_t address, uint32_t length)
{
    status_t status = kStatus_InvalidArgument;

    do
    {
        // SPI NAND should be configured before access.
        if (!is_spinand_configured())
        {
            status = kStatusMemoryNotConfigured;
            break;
        }
        // length = 0 means no erase operation will be executed. Just return success.
        if (length == 0)
        {
            status = kStatus_Success;
            break;
        }
        // else means 1 block at lest to be erased.

        uint32_t totalBlocks = s_spinandFcb.config.blocksPerDevice;
        uint32_t startBlockAddr;
        uint32_t blockCount;
        // The address[30:0] is block id if address[31] is 1
        if (s_spinandContext.nandAddressType == kNandAddressType_BlockAddress)
        {
            startBlockAddr = address;
            blockCount = length;
        }
        else // Address is actual NAND address
        {
            startBlockAddr = address / s_spinandFcb.config.pageDataSize / s_spinandFcb.config.pagesPerBlock;
            // Don't get block count from length. Address to address + length might across block boundary.
            blockCount = (address + length - 1) / s_spinandFcb.config.pageDataSize / s_spinandFcb.config.pagesPerBlock -
                         startBlockAddr + 1;
        }

        // Due to bad block is skipped,
        // then also need to check if the block to erase is not cross the memory end.
        while (blockCount && (startBlockAddr < totalBlocks))
        {
            if (!is_bad_block_in_dbbt(startBlockAddr))
            {
                status = spinand_memory_erase_and_verify(startBlockAddr);
                if (status != kStatus_Success)
                {
                    bad_block_discovered(startBlockAddr);
                }
                else
                {
                    // Don't count in the bad blocks.
                    blockCount--;
                }
            }
            startBlockAddr++;
        }
    } while (0);

    return status;
}
#define SNAND_TOTAL_BLOCKS   1024

void snand_full_chip_erase(void)
{
    PRINTF("[SNAND] Full chip erase start\r\n");

    for (uint32_t blk = 0; blk < SNAND_TOTAL_BLOCKS; blk++)
    {
        status_t st = spinand_mem_erase(blk, 1);
        if (st != kStatus_Success)
        {
            PRINTF("[SNAND] Erase failed at block %u\r\n", blk);
        }
    }

    spinand_mem_flush();

    PRINTF("[SNAND] Full chip erase done\r\n");
}


// See memory.h for documentation on this function.
status_t spinand_mem_erase_all(void)
{
    // SPI NAND should be configured before access.
    if (!is_spinand_configured())
    {
        return kStatusMemoryNotConfigured;
    }

    uint32_t startBlockAddr = 0;
    uint32_t totalBlocks = s_spinandFcb.config.blocksPerDevice;
    // In case SPI NAND is over 4G, do not call spinand_mem_erase() here.
    while (startBlockAddr < totalBlocks)
    {
        if (!is_bad_block_in_dbbt(startBlockAddr))
        {
            status_t status = spinand_memory_erase_and_verify(startBlockAddr);
            if (status != kStatus_Success)
            {
                return status;
            }
        }
        startBlockAddr++;
    }

    return kStatus_Success;
}

 status_t spinand_mem_load_buffer(uint32_t pageAddr)
{
    status_t status;

    s_spinandContext.isReadBufferValid = false; // Mark read buffer invalid.

    // Read the page to read buffer.
    status =
        spinand_memory_read(pageAddr, s_spinandFcb.config.pageDataSize, (uint8_t *)&s_spinandContext.readBuffer[0]);
    if (status == kStatus_Success)
    {
        s_spinandContext.isReadBufferValid = true;
        s_spinandContext.readBufferPageAddr = pageAddr;
    }

    return status;
}

 status_t spinand_mem_flush_buffer(void)
{
    status_t status;

    // Terminate current transfer if the write buffer size is less than page size when the flush API is called
    if (s_spinandContext.writeBufferOffset != s_spinandFcb.config.pageDataSize)
    {
        s_spinandContext.readwriteInProgress = false;
    }

    s_spinandContext.isWriteBufferValid = false;

    uint32_t srcPageAddr = s_spinandContext.writeBufferPageAddr;

#if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
    if (!is_erased_memory(srcPageAddr, 1))
    {
        return kStatusMemoryCumulativeWrite;
    }
#endif // #if SNAND_FLASH_CHECK_CUMULATIVE_WRITE

    // Flush the data in the write buffer to SPI NAND
    status =
        spinand_memory_write_and_verify(srcPageAddr, s_spinandContext.writeBufferOffset, (uint8_t*)s_spinandContext.writeBuffer);
    if (status == kStatus_Success)
    {
        // Write success, return.
        return status;
    }
    // else write failed, and need to move data to the next good block.

    uint32_t srcBlockAddr = srcPageAddr / s_spinandFcb.config.pagesPerBlock;

    bad_block_discovered(srcBlockAddr);
    s_spinandContext.skippedBlockCount++;

    uint32_t totalBlocks = s_spinandFcb.config.blocksPerDevice;
    uint32_t destBlockAddr = srcBlockAddr + 1; // First destination block is next block.
    // Should not cross the end boundary.
    while (destBlockAddr < totalBlocks)
    {
        // Check if destination block is a good block.
        if (!is_bad_block_in_dbbt(destBlockAddr))
        {
            // If a good block, try to backup the datas to next block.
            status = spinand_mem_block_backup(srcPageAddr, destBlockAddr);
#if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
            // Return if success or the next good block is not erased.
            if ((status == kStatus_Success) || (status == kStatusMemoryCumulativeWrite))
#else
            if (status == kStatus_Success)
#endif // #if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
            {
                return status;
            }
            // Backup failed means destination block is also a bad block.
            bad_block_discovered(destBlockAddr);
        }

        // Move to next block.
        destBlockAddr++;
        s_spinandContext.skippedBlockCount++;
    }
    // No erased good block left.
    return kStatusMemoryRangeInvalid;
}

 status_t spinand_mem_block_backup(uint32_t srcPageAddr, uint32_t destBlockAddr)
{
    status_t status = kStatus_Success;

    uint32_t startPageAddr = srcPageAddr - (srcPageAddr % s_spinandFcb.config.pagesPerBlock); // First page to backup.
    uint32_t endPageAddr = srcPageAddr; // The last page to backup. The last page is the page needs to flush.
    uint32_t destPageAddr =
        destBlockAddr * s_spinandFcb.config.pagesPerBlock; // Destination page to store the first page.

#if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
    // Firstly, need to check if the destination is erased.
    if (!is_erased_memory(destPageAddr, endPageAddr - startPageAddr + 1))
    {
        return kStatusMemoryCumulativeWrite;
    }
#endif // #if SNAND_FLASH_CHECK_CUMULATIVE_WRITE

    // Move all pages in the block to the good block, except the last page.
    while (startPageAddr < endPageAddr)
    {
        // Read the page needs to backup.
        status = spinand_memory_read(startPageAddr, s_spinandFcb.config.pageDataSize,
                                     (uint8_t *)&s_spinandContext.readBuffer[0]);
        if (status != kStatus_Success)
        {
            // Read failed, skip to next block to execute the backup progress.
            return status;
        }
        // Write the read page to the destination memory.
        status = spinand_memory_write_and_verify(destPageAddr, s_spinandFcb.config.pageDataSize,
                                                 (uint8_t *)&s_spinandContext.readBuffer[0]);
        if (status != kStatus_Success)
        {
            // Write failed, then skip to next block to execute the backup progress.
            return status;
        }
        // Move the source and destination pointer.
        startPageAddr++;
        destPageAddr++;
    }
    // Flush the last page. The data is contained in the write buffer.
    return spinand_memory_write_and_verify(destPageAddr, s_spinandFcb.config.pageDataSize,
                                           (uint8_t*)s_spinandContext.writeBuffer);
}

#if SNAND_FLASH_CHECK_CUMULATIVE_WRITE
 bool is_erased_memory(uint32_t pageAddr, uint32_t pageCount)
{
    status_t status;
    uint32_t pageSize = s_spinandFcb.config.pageDataSize;
    uint32_t offset, *buffer;
    while (pageCount)
    {
        // Read the page firstly.
        status = spinand_memory_read(pageAddr++, pageSize, (uint8_t *)&s_spinandContext.readBuffer[0]);
        if (status != kStatus_Success)
        {
            // If read failed, return false.
            return false;
        }

        offset = 0;
        buffer = (uint32_t *)s_spinandContext.readBuffer;
        while (offset < pageSize)
        {
            // Check if all 0xFFs
            if (*buffer != 0xffffffff)
            {
                return false;
            }
            buffer++;
            offset += 4;
        }
        pageCount--;
    }
    return true;
}
#endif // #if SNAND_FLASH_CHECK_CUMULATIVE_WRITE

 status_t spinand_memory_read(uint32_t pageAddr, uint32_t length, uint8_t *buffer)
{
    assert(buffer);

    status_t status;
    uint32_t size;

    while (length)
    {
        size = MIN(s_spinandFcb.config.pageDataSize, length);
        status = spinand_memory_read_page(pageAddr, size, buffer);
        if (status != kStatus_Success)
        {
            return status;
        }
        buffer += size;
        length -= size;
        pageAddr++;
    }

    return status;
}

 status_t spinand_memory_write_and_verify(uint32_t pageAddr, uint32_t length, uint8_t *buffer)
{
    assert(buffer);

    status_t status;
    uint32_t size;

    while (length)
    {
        size = MIN(s_spinandFcb.config.pageDataSize, length);
        status = spinand_memory_program_page(pageAddr, size, buffer);
        if (status != kStatus_Success)
        {
            return status;
        }

        status = spinand_memory_read_page(pageAddr, size, (uint8_t *)&s_spinandContext.readBuffer[0]);
        if (status != kStatus_Success)
        {
            return status;
        }

        if (memcmp(buffer, s_spinandContext.readBuffer, size))
        {
            return kStatus_Fail;
        }

        buffer += size;
        length -= size;
        pageAddr++;
    }

    return status;
}

status_t spinand_memory_erase_and_verify(uint32_t blockAddr)
{
#if SPI_NAND_ERASE_VERIFY
    status_t status;

    uint32_t pageAddr = blockAddr * s_spinandFcb.config.pagesPerBlock;

    // Erase the block.
    status = spinand_memory_erase_block(blockAddr);
    if (status != kStatus_Success)
    {
        return status;
    }

    // Check if the memory is erased ( All 0xFFs).
    if (is_erased_memory(pageAddr, s_spinandFcb.config.pagesPerBlock))
    {
        return kStatus_Success;
    }
    else
    {
        // If not all 0xFFs, means erase operation is failed.
        return kStatus_FlexSPINAND_EraseBlockFail;
    }
#else
    return spinand_memory_erase_block(blockAddr);
#endif // defined(SPI_NAND_ERASE_VERIFY)
}

status_t spinand_memory_spi_init(flexspi_nand_config_t *config)
{
    return flexspi_nand_init(EXAMPLE_MIXSPI, config);
}

 status_t spinand_memory_read_page(uint32_t pageAddr, uint32_t length, uint8_t *buffer)
{
    return flexspi_nand_read_page(EXAMPLE_MIXSPI, &s_spinandFcb.config, pageAddr, (uint32_t *)buffer, length);
}

 status_t spinand_memory_program_page(uint32_t pageAddr, uint32_t length, uint8_t *buffer)
{
    return flexspi_nand_program_page(EXAMPLE_MIXSPI, &s_spinandFcb.config, pageAddr, (uint32_t *)buffer, length);
}

 status_t spinand_memory_erase_block(uint32_t blockAddr)
{
    return flexspi_nand_erase_block(EXAMPLE_MIXSPI, &s_spinandFcb.config, blockAddr);
}

 bool is_spinand_configured(void)
{
    return s_spinandContext.isConfigured;
}

 status_t spinand_mem_creat_empty_dbbt(void)
{
    // Create empty dbbt;
    // Fill DBBT to all 0xFFs.
    memset(&s_spinandDbbt, kFlashDefaultPattern, sizeof(spinand_dbbt_t));

    s_spinandDbbt.badBlockNumber = 0;

    return kStatus_Success;
}

 bool is_bad_block_in_dbbt(uint32_t blockAddr)
{
    uint32_t i;
    // Traversal. No sort when adding items.
    for (i = 0; i < s_spinandDbbt.badBlockNumber; i++)
    {
        if (s_spinandDbbt.badBlockTable[i] == blockAddr)
        {
            return true;
        }
    }
    return false;
}

 status_t bad_block_discovered(uint32_t blockAddr)
{
    if (s_spinandDbbt.badBlockNumber < kSpiNandMemory_MaxDBBTSize)
    {
        s_spinandDbbt.badBlockTable[s_spinandDbbt.badBlockNumber++] = blockAddr;
        return kStatus_Success;
    }
    else
    {
        return kStatus_Fail;
    }
}

 status_t skip_bad_blocks(uint32_t *addr)
{
    *addr += s_spinandContext.skippedBlockCount * s_spinandFcb.config.pagesPerBlock;
    return kStatus_Success;
}

 bool need_to_check_dbbt_before_read(uint32_t blockAddr)
{
    return (!s_spinandContext.isReadBufferValid) ||
           (blockAddr != (s_spinandContext.readBufferPageAddr / s_spinandFcb.config.pagesPerBlock));
}

 bool need_to_check_dbbt_before_write(uint32_t blockAddr)
{
    return (!s_spinandContext.isWriteBufferValid) ||
           (blockAddr != (s_spinandContext.writeBufferPageAddr / s_spinandFcb.config.pagesPerBlock));
}

 bool is_read_page_cached(uint32_t pageAddr)
{
    return (s_spinandContext.readBufferPageAddr == pageAddr) && (s_spinandContext.isReadBufferValid);
}

 bool is_write_page_cached(uint32_t pageAddr)
{
    return (s_spinandContext.writeBufferPageAddr == pageAddr) && (s_spinandContext.isWriteBufferValid);
}


////////////////////////////////////////////////////////////////////////////////
// EOF
////////////////////////////////////////////////////////////////////////////////
