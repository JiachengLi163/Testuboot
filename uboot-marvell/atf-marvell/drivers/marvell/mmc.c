/*
 * Copyright (C) 2016-2018 Marvell International Ltd.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 * https://spdx.org/licenses
 */

/* CGX driver for OcteonTX (CN8xxx and CN9xxx) */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <platform_def.h>
#include <octeontx_common.h>
#include <io_storage.h>
#include <assert.h>
#include <io_driver.h>
#include <debug.h>
#include <plat_board_cfg.h>
#include <octeontx_security.h>

#include <io_mmc.h>

#define SHARED_MEM_BASE (TZDRAM_BASE + TZDRAM_SIZE)

#define ROUND_UP(val, align)	(((val) + (align) - 1) / (align) * (align))

#define REF_FREQ	50000 // number of reference cycles in a millisecond
#define INIT_MAX_RETRY	20
#define SECTOR_SIZE	512
#define MMC_SECTOR_SIZE	512

#define CMD_NO_DATA 	0
#define CMD_READ_DBUF	1
#define CMD_WRITE_DBUF	2
#define RESP_NONE	0
#define RESP_R1		1
#define RESP_R2		2
#define RESP_R3		3
#define RESP_R4		4
#define RESP_R5		5

#define BUS_1_BIT	0
#define BUS_4_BIT	1
#define BUS_8_BIT	2

#define MMC_CMD_GO_IDLE_STATE		0
#define MMC_CMD_SEND_OP_COND		1
#define MMC_CMD_ALL_SEND_CID		2
#define MMC_CMD_SET_RELATIVE_ADDR	3
#define MMC_CMD_SET_DSR			4
#define MMC_CMD_SWITCH			6
#define MMC_CMD_SELECT_CARD		7
#define MMC_CMD_SEND_EXT_CSD		8
#define MMC_CMD_SEND_CSD		9
#define MMC_CMD_SEND_CID		10
#define MMC_CMD_STOP_TRANSMISSION	12
#define MMC_CMD_SEND_STATUS		13
#define MMC_CMD_SET_BLOCKLEN		16
#define MMC_CMD_READ_SINGLE_BLOCK	17
#define MMC_CMD_READ_MULTIPLE_BLOCK	18
#define MMC_CMD_WRITE_SINGLE_BLOCK	24
#define MMC_CMD_WRITE_MULTIPLE_BLOCK	25
#define MMC_CMD_ERASE_GROUP_START	35
#define MMC_CMD_ERASE_GROUP_END		36
#define MMC_CMD_ERASE			38
#define MMC_CMD_APP_CMD			55
#define MMC_CMD_SPI_READ_OCR		58
#define MMC_CMD_SPI_CRC_ON_OFF		59
#define MMC_CMD_RES_MAN			62

#define SD_CMD_SEND_RELATIVE_ADDR	3
#define SD_CMD_SWITCH_FUNC		6
#define SD_CMD_SEND_IF_COND		8

#define SD_CMD_APP_SET_BUS_WIDTH	6
#define SD_CMD_ERASE_WR_BLK_START	32
#define SD_CMD_ERASE_WR_BLK_END		33
#define SD_CMD_APP_SEND_OP_COND		41
#define SD_CMD_APP_SEND_SCR		51

typedef struct {
	/* Use the 'in_use' flag as any value for base and file_pos could be
	 * valid.
	 */
	int		in_use;
	unsigned	cs;
	size_t		file_pos;
	size_t		offset_address;
} file_state_t;

static file_state_t mmc_current_file = { 0 };

typedef struct mio_emm_driver {
	int bus_id;
	int rca;
	int sector_size;
	int bus_width;
	int sector_mode;
} mio_emm_driver_t;

mio_emm_driver_t mmc_drv = { 0 };

/* Wait for delay reference cycles */
void wait(uint64_t delay)
{
	volatile uint64_t count = CSR_READ(CAVM_RST_REF_CNTR);
	uint64_t start = count;
	uint64_t end = start + delay;

	do {
		count = CSR_READ(CAVM_RST_REF_CNTR);
	} while (count < end);
}

union cavm_mio_emm_rsp_sts mio_emm_cmd(uint32_t cmd_idx, uint32_t ctype_xor, uint32_t rtype_xor, uint32_t arg)
{
	union cavm_mio_emm_cmd cmd;
	volatile union cavm_mio_emm_rsp_sts emm_rsp_sts;

	cmd.u = 0;
	cmd.s.cmd_val = 1;
	cmd.s.ctype_xor = ctype_xor;
	cmd.s.rtype_xor = rtype_xor;
	cmd.s.arg = arg;
	cmd.s.cmd_idx = cmd_idx;
	CSR_WRITE(CAVM_MIO_EMM_CMD, cmd.u);

	do {
		emm_rsp_sts.u = CSR_READ(CAVM_MIO_EMM_RSP_STS);
	} while (emm_rsp_sts.s.cmd_val);


	return emm_rsp_sts;
}

int sdmmc_rw_data(int write, unsigned int addr, int size, uintptr_t buf, int buf_size)
{
	int blk_cnt, blks, offset, bytes;
	int round_size;
	uintptr_t tmp_buf = TZDRAM_BASE + 512;

	union cavm_mio_emm_dma mio_emm_dma;
	union cavm_mio_emm_dma_cfg emm_dma_cfg;
	union cavm_mio_emm_rsp_sts emm_rsp_sts;

	/* DMA engine address must be 64-bit aligned */
	if (size == 0 || ((uintptr_t)buf & 0x7)) {
		printf("Invalid size %d or unaligned addr 0x%lx\n",
				size, buf);
		return -1;
	}

	round_size = ROUND_UP(size, mmc_drv.sector_size);
	if (buf_size < size) {
		printf("buf_size %d too small, need %d (aligned %d)\n",
				buf_size, size, round_size);
	}

	octeontx_configure_mmc_security(1); /* secure */

	blk_cnt = round_size / mmc_drv.sector_size;
	offset  = addr % mmc_drv.sector_size;

	if ((size % mmc_drv.sector_size) + offset > 512)
		blk_cnt++;

	while (blk_cnt > 0) {
		blks = (blk_cnt > 8) ? 8 : blk_cnt;

		emm_dma_cfg.u = 0;
		mio_emm_dma.u = 0;

		emm_dma_cfg.s.en = 1;
		emm_dma_cfg.s.rw = write ? 1 : 0;
		emm_dma_cfg.s.endian = 1;
		emm_dma_cfg.s.size = (blks * mmc_drv.sector_size >> 3) - 1;

		mio_emm_dma.s.dma_val = 1;
		mio_emm_dma.s.bus_id = mmc_drv.bus_id;
		mio_emm_dma.s.sector = 1;
		mio_emm_dma.s.card_addr = addr / mmc_drv.sector_size;
		mio_emm_dma.s.block_cnt = blks;

		CSR_WRITE(CAVM_MIO_EMM_DMA_ADR, tmp_buf);
		CSR_WRITE(CAVM_MIO_EMM_DMA_CFG, emm_dma_cfg.u);
		CSR_WRITE(CAVM_MIO_EMM_DMA, mio_emm_dma.u);

		do {
			emm_rsp_sts.u = CSR_READ(CAVM_MIO_EMM_RSP_STS);
		} while (emm_rsp_sts.s.dma_val);

		/* DMA error */
		if (emm_rsp_sts.s.dma_pend) {
			ERROR("sdmmc: DMA error\n");

			mio_emm_dma.u = CSR_READ(CAVM_MIO_EMM_DMA);
			mio_emm_dma.s.dma_val = 1;
			mio_emm_dma.s.dat_null = 1;
			CSR_WRITE(CAVM_MIO_EMM_DMA, mio_emm_dma.u);
			do {
				emm_rsp_sts.u = CSR_READ(CAVM_MIO_EMM_RSP_STS);
			} while (emm_rsp_sts.s.dma_val);

			return -1;
		}

		do {
			emm_dma_cfg.u = CSR_READ(CAVM_MIO_EMM_DMA_CFG);
		} while (emm_dma_cfg.s.en);

		blk_cnt -= blks;
		addr += mmc_drv.sector_size * blks;

		inv_dcache_range(tmp_buf, blks * mmc_drv.sector_size);

		__asm__ volatile ("dsb sy");

		bytes = blk_cnt ? (mmc_drv.sector_size * blks - offset) : size;

		memcpy((void *)buf, (void *)(tmp_buf + offset), bytes);

		buf += bytes;

		offset = 0; // make skip 0 as we don't need it next loop
	}

	return buf_size;
}

int emmc_config()
{
#if 0
	uint64_t val;
	int clock;
	int sec_cap = 0;
	volatile union cavm_rst_boot rst_boot;
	volatile union cavm_gpio_bit_cfgx gpio_bit_cfgx;
	volatile union cavm_mio_emm_switch emm_switch, slow_switch;
	volatile union cavm_mio_emm_rsp_sts emm_rsp_sts;
	volatile unsigned long emm_rsp_lo;
#endif

	mmc_drv.bus_id = mmc_current_file.cs;
	mmc_drv.sector_size = MMC_SECTOR_SIZE;
	/* Set eMMC bus id bit according to the used CS */
	CSR_WRITE(CAVM_MIO_EMM_CFG, 1<<mmc_current_file.cs);

#if 0
	rst_boot.u = CSR_READ(mmc_current_file.node, CAVM_RST_BOOT);

	/* Clear GPIO_BIT_CFG8[TX_OE] to disable power / enable reset */
	//gpio_bit_cfgx.u = 0;
	//gpio_bit_cfgx.s.tx_oe = 0;
	//CSR_WRITE(mmc_current_file.node,regs.gpio_base, GPIO_BIT_CFGX(8), gpio_bit_cfgx.u);
	wait(200 * REF_FREQ);
	val = (CSR_READ(mmc_current_file.node, CAVM_GPIO_RX_DAT) >> 8) & 0x1;

	if (val)
		CSR_WRITE(mmc_current_file.node, CAVM_GPIO_TX_CLR, 1 << 8);
	else
		CSR_WRITE(mmc_current_file.node, CAVM_GPIO_TX_SET, 1 << 8);

	gpio_bit_cfgx.u = 0;
	gpio_bit_cfgx.s.tx_oe = 1;
	CSR_WRITE(mmc_current_file.node, CAVM_GPIO_BIT_CFGX(8), gpio_bit_cfgx.u);

	wait(2 * REF_FREQ);

	/* Set bit 1 to use eMMC bus 0 */
	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_CFG, 0x1);

	/* EMMC:(70)  Clear mio_emm_sts_mask.STS_MSK[31:8,6:0], Set Switch_ERR(<7>)
	 * ignore errors except <7>
	 */
	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_STS_MASK, 0x80);

	/* 400 KHz */
	clock = (125 * rst_boot.s.pnr_mul + 1) >> 1;
	emm_switch.u = 0;
	emm_switch.s.power_class = 10;
	emm_switch.s.clk_hi = emm_switch.s.clk_lo = clock;
	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_SWITCH, emm_switch.u);

	clock = (5 * rst_boot.s.pnr_mul + 3) >> 2;
	emm_switch.s.clk_hi = emm_switch.s.clk_lo = clock;
	emm_switch.s.power_class = 10;
	emm_switch.s.bus_width = BUS_8_BIT;
	wait(1.2 * REF_FREQ);

	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_WDOG, 0x400);

	// DO while loop
	clock = (125 * rst_boot.s.pnr_mul + 1) >> 1;
	slow_switch.u = 0;
	slow_switch.s.power_class = 10;
	//slow_switch.s.clk_hi = 0;
	//slow_switch.s.clk_lo = clock;
	slow_switch.s.clk_hi = slow_switch.s.clk_lo = clock;
	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_SWITCH, slow_switch.u);

	mio_emm_cmd(MMC_CMD_GO_IDLE_STATE, 0, 0, 0);
	emm_rsp_sts = mio_emm_cmd(SD_CMD_SEND_IF_COND, 1, 2, 0x1aa);
	sec_cap = emm_rsp_sts.s.rsp_timeout ? 0 : 1;
	(void)sec_cap;

	mio_emm_cmd(MMC_CMD_APP_CMD, 0, 0, 0);
	emm_rsp_sts = mio_emm_cmd(SD_CMD_APP_SEND_OP_COND, 0, 3, 0x40ff8000);

	/* Command 41 SD_CMD_APP_SEND_OP_COND timesout - eMMC  */
	if (emm_rsp_sts.s.rsp_timeout) {
		INFO("%s %d: mmc_drv\n", __func__, __LINE__);
		return -1;
	}

	emm_rsp_lo = CSR_READ(mmc_current_file.node, CAVM_MIO_EMM_RSP_LO);
	/* SD card */
	if ((emm_rsp_lo & (1UL << 39)) == 0) {
		INFO("%s %d: error emm_rsp_lo = %lx\n", __func__, __LINE__, emm_rsp_lo);
		/* TODO: retry */
		return -1;
	}

	mmc_drv.sector_mode = emm_rsp_lo & (0x3UL << 37);
	mmc_drv.bus_width = BUS_4_BIT;
	emm_rsp_sts = mio_emm_cmd(MMC_CMD_ALL_SEND_CID, 0, 0, 0x0);
	emm_rsp_sts = mio_emm_cmd(SD_CMD_SEND_RELATIVE_ADDR, 0, 0, 0x0);
	mmc_drv.rca = (emm_rsp_lo >> 24) & 0xffff;
	emm_rsp_sts = mio_emm_cmd(MMC_CMD_SELECT_CARD, 0, 0, mmc_drv.rca << 16);

	CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_RCA, mmc_drv.rca);

	/* Switch to new bus width etc.
	 * TODO: read the CSD register and config card properly
	 */
	mio_emm_cmd(MMC_CMD_APP_CMD, 0, 0, (mmc_drv.rca<<16));
	emm_rsp_sts = mio_emm_cmd(MMC_CMD_SWITCH, 0, 0, mmc_drv.bus_width << 1);
	emm_rsp_lo = CSR_READ(mmc_current_file.node, CAVM_MIO_EMM_RSP_LO);

	if (!emm_rsp_sts.s.rsp_timeout) {
		if (!(emm_rsp_lo >> (8 + 5) & 1)			// APP_CMD
				&& (emm_rsp_lo >> (8 + 31) & 1)		// OUT_OF_RANGE
				&& (emm_rsp_lo >> (8 + 19) & 1)){ 	// ERROR

			emm_rsp_sts = mio_emm_cmd(MMC_CMD_SEND_STATUS, 0, 0, mmc_drv.rca << 16);
			emm_rsp_lo = CSR_READ(mmc_current_file.node, CAVM_MIO_EMM_RSP_LO);
			if (!emm_rsp_sts.s.rsp_timeout) {

				if ((emm_rsp_lo >> (8 + 31) & 1)		// OUT_OF_RANGE
						&& (emm_rsp_lo >> (8 + 19) & 1)) {	// ERROR

					/* 20 MHz */
					clock = (5 * rst_boot.s.pnr_mul + 3) >> 2;
					emm_switch.u = 0;
					emm_switch.s.bus_width = mmc_drv.bus_width;
					emm_switch.s.clk_hi = clock;
					emm_switch.s.clk_lo = clock;
					emm_switch.s.power_class = 10;
					emm_switch.s.switch_exe = 1;
					CSR_WRITE(mmc_current_file.node, CAVM_MIO_EMM_SWITCH, emm_switch.u);
					INFO("%s %d: emm_rsp_sts\n", __func__, __LINE__);

					do {
						emm_rsp_sts.u = CSR_READ(mmc_current_file.node, CAVM_MIO_EMM_RSP_STS);
					} while (emm_rsp_sts.s.switch_val);
				}
			}
		}
	}

	INFO("%s %d: done\n", __func__, __LINE__);
#endif
	return 0;
}

/* Identify the device type as emmc */
static io_type_t device_type_emmc(void)
{
	return IO_TYPE_EMMC;
}

static int emmc_block_open(io_dev_info_t *dev_info, const uintptr_t spec,
		io_entity_t *entity)
{
	int result = -ENOMEM;
	const io_block_spec_t *block_spec = (io_block_spec_t *)spec;

	/* Since we need to track open state for seek() we only allow one open
	 * spec at a time. When we have dynamic memory we can malloc and set
	 * entity->info.
	 */

	if (mmc_current_file.in_use == 0) {
		assert(block_spec != NULL);
		assert(entity != NULL);

		mmc_current_file.in_use = 1;
		// FIXME mmc_current_file.cs = block_spec->offset;
		/* File cursor offset for seek and incremental reads etc. */
		mmc_current_file.file_pos = 0;
		mmc_current_file.offset_address = block_spec->offset;
		mmc_current_file.cs = plat_octeontx_bcfg->bcfg.boot_dev.cs;

		entity->info = (uintptr_t)&mmc_current_file;

		return emmc_config();
	} else {
		WARN("An emmc device is already active. Close first.\n");
	}

	return result;
}

static int emmc_block_seek(io_entity_t *entity, int mode,
		ssize_t offset)
{
	int result = -ENOENT;

	/* We only support IO_SEEK_SET and CUR for the moment. */
	if (mode == IO_SEEK_SET) {
		assert(entity != NULL);

		/* TODO: can we do some basic limit checks on seek? */
		((file_state_t *)entity->info)->file_pos = offset;
		result = 0;
	} else if (mode == IO_SEEK_CUR) {
		assert(entity != NULL);

		/* TODO: can we do some basic limit checks on seek? */
		((file_state_t *)entity->info)->file_pos += offset;
		result = 0;
	}

	return result;
}


/*
 * This func should only come for reading NS image size
 */
static int emmc_block_size(io_entity_t *entity, size_t *length)
{
	file_state_t *fp;
	uint64_t offset = 0x480000;
	int ret;
	unsigned char *buffer = (unsigned char*)SHARED_MEM_BASE;

	assert(entity != NULL);
	fp = (file_state_t *)entity->info;

	ret = sdmmc_rw_data(0, offset + fp->file_pos, 1024, (uintptr_t) buffer, 1024);
	if (ret < 0)
		return ret;

	*length = *(size_t *)(buffer + (2 * 0x100));

	return 0;
}

static int emmc_block_read(io_entity_t *entity, uintptr_t buffer,
		size_t length, size_t *length_read)
{
	file_state_t *fp;
	ssize_t ret;
	unsigned int addr;

	assert(entity != NULL);
	assert(buffer != (uintptr_t)NULL);
	assert(length_read != NULL);

	fp = (file_state_t *)entity->info;

	addr = fp->offset_address + fp->file_pos;

	ret = sdmmc_rw_data(0,  addr, length, buffer, length);
	if (ret < 0)
		return ret;

	*length_read = ret;
	fp->file_pos += ret;

	return 0;
}

static int emmc_block_close(io_entity_t *entity)
{
	assert(entity != NULL);

	entity->info = 0;

	/* This would be a mem free() if we had malloc.*/
	memset((void *)&mmc_current_file, 0, sizeof(mmc_current_file));

	return 0;
}

static int emmc_dev_close(io_dev_info_t *dev_info)
{
	/* NOP */
	/* TODO: Consider tracking open files and cleaning them up here */
	return 0;
}



static const io_dev_funcs_t emmc_dev_funcs = {
	.type = device_type_emmc,
	.open = emmc_block_open,
	.seek = emmc_block_seek,
	.size = emmc_block_size,
	.read = emmc_block_read,
	.write = NULL,
	.close = emmc_block_close,
	.dev_init = NULL,
	.dev_close = emmc_dev_close,
};


/* No state associated with this device so structure can be const */
static const io_dev_info_t emmc_dev_info = {
	.funcs = &emmc_dev_funcs,
	.info = (uintptr_t)NULL
};

/* Open a connection to the emmc device */
static int emmc_dev_open(const uintptr_t dev_spec __attribute__((unused)),
		io_dev_info_t **dev_info)
{
	assert(dev_info != NULL);
	*dev_info = (io_dev_info_t *)&emmc_dev_info; /* cast away const */

	return 0;
}


static const io_dev_connector_t emmc_dev_connector = {
	.dev_open = emmc_dev_open
};

/* Exported functions */

/* Register the emmc driver with the IO abstraction */
int register_io_dev_emmc(const io_dev_connector_t **dev_con)
{
	int result;
	assert(dev_con != NULL);

	result = io_register_device(&emmc_dev_info);
	if (result == 0)
		*dev_con = &emmc_dev_connector;

	return result;
}
