// Copyright 2013-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "esp_types.h"
#include "rom/ets_sys.h"
#include "esp_psram.h"
#include "soc/io_mux_reg.h"
#include "soc/dport_reg.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_attr.h"
#include "rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "string.h"
#include "rom/spi_flash.h"
#include "esp_err.h"
#include "rom/cache.h"
#include "driver/gpio.h"

//Commands for PSRAM chip
#define PSRAM_READ              0x03
#define PSRAM_FAST_READ         0x0B
#define PSRAM_FAST_READ_QUAD    0xEB
#define PSRAM_WRITE             0x02
#define PSRAM_QUAD_WRITE        0x38
#define PSRAM_ENTER_QMODE       0x35
#define PSRAM_EXIT_QMODE        0xF5
#define PSRAM_RESET_EN          0x66
#define PSRAM_RESET             0x99
#define PSRAM_SET_BURST_LEN     0xC0
#define PSRAM_DEVICE_ID         0x9F

#define PSRAM_MFG_ID_M          0xff
#define PSRAM_MFG_ID_S             8
#define PSRAM_MFG_ID_ESPRESSIF  0x5d

#define PSRAM_CLK_IO      17
#define PSRAM_CS_IO       16

typedef enum {
    PSRAM_SPI_1  = 0x1,
    PSRAM_SPI_2,
    PSRAM_SPI_3,
    PSRAM_SPI_MAX ,
} psram_spi_num_t;

static psram_cache_mode_t s_psram_mode = PSRAM_CACHE_MAX;

//For now, we only use F40M + S40M, and we don't have to go through gpio matrix
#define DISABLE_GPIO_MATRIX_FOR_40M   1
static int extra_dummy = 0;

typedef enum {
    PSRAM_CMD_QPI,
    PSRAM_CMD_SPI,
} psram_cmd_mode_t;

typedef struct {
    uint16_t cmd;                /*!< Command value */
    uint16_t cmdBitLen;          /*!< Command byte length*/
    uint32_t *addr;              /*!< Point to address value*/
    uint16_t addrBitLen;         /*!< Address byte length*/
    uint32_t *txData;            /*!< Point to send data buffer*/
    uint16_t txDataBitLen;       /*!< Send data byte length.*/
    uint32_t *rxData;            /*!< Point to recevie data buffer*/
    uint16_t rxDataBitLen;       /*!< Recevie Data byte length.*/
    uint32_t dummyBitLen;
} psram_cmd_t;

static void IRAM_ATTR psram_cache_init(psram_cache_mode_t psram_cache_mode, psram_vaddr_mode_t vaddrmode);

static void psram_clear_spi_fifo(psram_spi_num_t spi_num)
{
    int i;
    for (i = 0; i < 16; i++) {
        DPORT_WRITE_PERI_REG(SPI_W0_REG(spi_num)+i*4, 0);
    }
}

//set basic SPI write mode
static void psram_set_basic_write_mode(psram_spi_num_t spi_num)
{
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_QIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_DIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_QUAD);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_DUAL);
}
//set QPI write mode
static void psram_set_qio_write_mode(psram_spi_num_t spi_num)
{
    DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_QIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_DIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_QUAD);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_FWRITE_DUAL);
}
//set QPI read mode
static void psram_set_qio_read_mode(psram_spi_num_t spi_num)
{
    DPORT_SET_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_QIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_QUAD);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_DUAL);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_DIO);
}
//set SPI read mode
static void psram_set_basic_read_mode(psram_spi_num_t spi_num)
{
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_QIO);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_QUAD);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_DUAL);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_FREAD_DIO);
}

//start sending and wait for finishing
static IRAM_ATTR void psram_cmd_start(psram_spi_num_t spi_num, psram_cmd_mode_t cmd_mode)
{
    //get cs1
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS1_DIS_M);
    DPORT_SET_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS0_DIS_M);

    uint32_t wr_mode_backup = (DPORT_READ_PERI_REG(SPI_USER_REG(spi_num)) >> SPI_FWRITE_DUAL_S) & 0xf;
    uint32_t rd_mode_backup = DPORT_READ_PERI_REG(SPI_CTRL_REG(spi_num)) & (SPI_FREAD_DIO_M | SPI_FREAD_DUAL_M | SPI_FREAD_QUAD_M | SPI_FREAD_QIO_M);
    if (cmd_mode == PSRAM_CMD_SPI) {
        psram_set_basic_write_mode(spi_num);
        psram_set_basic_read_mode(spi_num);
    } else if (cmd_mode == PSRAM_CMD_QPI) {
        psram_set_qio_write_mode(spi_num);
        psram_set_qio_read_mode(spi_num);
    }

    //WAIT SPI0 IDLE
    //READ THREE TIMES TO MAKE SURE?
    while( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    while( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    while( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    DPORT_SET_PERI_REG_MASK( DPORT_HOST_INF_SEL_REG, 1<<14);

    // Start send data
    DPORT_SET_PERI_REG_MASK(SPI_CMD_REG(spi_num), SPI_USR);
    while ((DPORT_READ_PERI_REG(SPI_CMD_REG(spi_num)) & SPI_USR));

    DPORT_CLEAR_PERI_REG_MASK(DPORT_HOST_INF_SEL_REG, 1 << 14);

    //recover spi mode
    DPORT_SET_PERI_REG_BITS(SPI_USER_REG(spi_num), 0xf, wr_mode_backup, SPI_FWRITE_DUAL_S);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), (SPI_FREAD_DIO_M|SPI_FREAD_DUAL_M|SPI_FREAD_QUAD_M|SPI_FREAD_QIO_M));
    DPORT_SET_PERI_REG_MASK(SPI_CTRL_REG(spi_num), rd_mode_backup);

    //return cs to cs0
    DPORT_SET_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS1_DIS_M);
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1),SPI_CS0_DIS_M);
}

//start sending cmd/addr and receving data
static void IRAM_ATTR psram_recv_start(psram_spi_num_t spi_num, uint32_t* pRxData, uint16_t rxByteLen,
        psram_cmd_mode_t cmd_mode)
{
    //get cs1
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS1_DIS_M);
    DPORT_SET_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS0_DIS_M);

    uint32_t cmd_mode_backup = (DPORT_READ_PERI_REG(SPI_USER_REG(spi_num)) >> SPI_FWRITE_DUAL_S) & 0xf;
    uint32_t rd_mode_backup = DPORT_READ_PERI_REG(SPI_CTRL_REG(spi_num)) & (SPI_FREAD_DIO_M | SPI_FREAD_DUAL_M | SPI_FREAD_QUAD_M | SPI_FREAD_QIO_M);
    if (cmd_mode == PSRAM_CMD_SPI) {
        psram_set_basic_write_mode(spi_num);
        psram_set_basic_read_mode(spi_num);
    } else if (cmd_mode == PSRAM_CMD_QPI) {
        psram_set_qio_write_mode(spi_num);
        psram_set_qio_read_mode(spi_num);
    }

    //WAIT SPI0 IDLE
    //READ THREE TIMES TO MAKE SURE?
    while ( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    while ( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    while ( DPORT_READ_PERI_REG(SPI_EXT2_REG(0)) != 0);
    DPORT_SET_PERI_REG_MASK(DPORT_HOST_INF_SEL_REG, 1 << 14);

    // Start send data
    DPORT_SET_PERI_REG_MASK(SPI_CMD_REG(spi_num), SPI_USR);
    while ((DPORT_READ_PERI_REG(SPI_CMD_REG(spi_num)) & SPI_USR));
    DPORT_CLEAR_PERI_REG_MASK(DPORT_HOST_INF_SEL_REG, 1 << 14);

    //recover spi mode
    DPORT_SET_PERI_REG_BITS(SPI_USER_REG(spi_num), SPI_FWRITE_DUAL_M, cmd_mode_backup, SPI_FWRITE_DUAL_S);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), (SPI_FREAD_DIO_M|SPI_FREAD_DUAL_M|SPI_FREAD_QUAD_M|SPI_FREAD_QIO_M));
    DPORT_SET_PERI_REG_MASK(SPI_CTRL_REG(spi_num), rd_mode_backup);

    //return cs to cs0
    DPORT_SET_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS1_DIS_M);
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(PSRAM_SPI_1), SPI_CS0_DIS_M);

    int idx = 0;
    // Read data out
    do {
        *pRxData++ = DPORT_READ_PERI_REG(SPI_W0_REG(spi_num) + (idx << 2));
    } while (++idx < ((rxByteLen / 4) + ((rxByteLen % 4) ? 1 : 0)));
}

//setup spi command/addr/data/dummy in user mode
static int psram_cmd_config(psram_spi_num_t spi_num, psram_cmd_t* pInData)
{
    while (DPORT_READ_PERI_REG(SPI_CMD_REG(spi_num)) & SPI_USR);
    // Set command by user.
    if (pInData->cmdBitLen != 0) {
        // Max command length 16 bits.
        DPORT_SET_PERI_REG_BITS(SPI_USER2_REG(spi_num), SPI_USR_COMMAND_BITLEN, pInData->cmdBitLen - 1,
                SPI_USR_COMMAND_BITLEN_S);
        // Enable command
        DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_COMMAND);
        // Load command,bit15-0 is cmd value.
        DPORT_SET_PERI_REG_BITS(SPI_USER2_REG(spi_num), SPI_USR_COMMAND_VALUE, pInData->cmd, SPI_USR_COMMAND_VALUE_S);
    } else {
        DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_COMMAND);
        DPORT_SET_PERI_REG_BITS(SPI_USER2_REG(spi_num), SPI_USR_COMMAND_BITLEN, 0, SPI_USR_COMMAND_BITLEN_S);
    }
    // Set Address by user.
    if (pInData->addrBitLen != 0) {
        DPORT_SET_PERI_REG_BITS(SPI_USER1_REG(spi_num), SPI_USR_ADDR_BITLEN, (pInData->addrBitLen - 1), SPI_USR_ADDR_BITLEN_S);
        // Enable address
        DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_ADDR);
        // Set address
        //DPORT_SET_PERI_REG_BITS(SPI_ADDR_REG(spi_num), SPI_USR_ADDR_VALUE, *pInData->addr, SPI_USR_ADDR_VALUE_S);
        DPORT_WRITE_PERI_REG(SPI_ADDR_REG(spi_num), *pInData->addr);
    } else {
        DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_ADDR);
        DPORT_SET_PERI_REG_BITS(SPI_USER1_REG(spi_num), SPI_USR_ADDR_BITLEN, 0, SPI_USR_ADDR_BITLEN_S);
    }
    // Set data by user.
    uint32_t* p_tx_val = pInData->txData;
    if (pInData->txDataBitLen != 0) {
        // Enable MOSI
        DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_MOSI);
        // Load send buffer
        int len = (pInData->txDataBitLen + 31) / 32;
        if (p_tx_val != NULL) {
            memcpy((void*)SPI_W0_REG(spi_num), p_tx_val, len * 4);
        }
        // Set data send buffer length.Max data length 64 bytes.
        DPORT_SET_PERI_REG_BITS(SPI_MOSI_DLEN_REG(spi_num), SPI_USR_MOSI_DBITLEN, (pInData->txDataBitLen - 1),
                SPI_USR_MOSI_DBITLEN_S);
    } else {
        DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_MOSI);
        DPORT_SET_PERI_REG_BITS(SPI_MOSI_DLEN_REG(spi_num), SPI_USR_MOSI_DBITLEN, 0, SPI_USR_MOSI_DBITLEN_S);
    }
    // Set rx data by user.
    if (pInData->rxDataBitLen != 0) {
        // Enable MOSI
        DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_MISO);
        // Set data send buffer length.Max data length 64 bytes.
        DPORT_SET_PERI_REG_BITS(SPI_MISO_DLEN_REG(spi_num), SPI_USR_MISO_DBITLEN, (pInData->rxDataBitLen - 1),
                SPI_USR_MISO_DBITLEN_S);
    } else {
        DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_USR_MISO);
        DPORT_SET_PERI_REG_BITS(SPI_MISO_DLEN_REG(spi_num), SPI_USR_MISO_DBITLEN, 0, SPI_USR_MISO_DBITLEN_S);
    }
    if (pInData->dummyBitLen != 0) {
        DPORT_SET_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_USR_DUMMY); // dummy en
        DPORT_SET_PERI_REG_BITS(SPI_USER1_REG(PSRAM_SPI_1), SPI_USR_DUMMY_CYCLELEN_V, pInData->dummyBitLen - 1,
                SPI_USR_DUMMY_CYCLELEN_S);  //DUMMY
    } else {
        DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_USR_DUMMY); // dummy en
        DPORT_SET_PERI_REG_BITS(SPI_USER1_REG(PSRAM_SPI_1), SPI_USR_DUMMY_CYCLELEN_V, 0, SPI_USR_DUMMY_CYCLELEN_S);  //DUMMY
    }
    return 0;
}

//exit QPI mode(set back to SPI mode)
static void psram_disable_qio_mode(psram_spi_num_t spi_num)
{
    psram_cmd_t ps_cmd;
    uint32_t cmd_exit_qpi;
    switch (s_psram_mode) {
        case PSRAM_CACHE_F80M_S80M:
            cmd_exit_qpi = PSRAM_EXIT_QMODE;
            ps_cmd.txDataBitLen = 8;
            break;
        case PSRAM_CACHE_F80M_S40M:
        case PSRAM_CACHE_F40M_S40M:
        default:
            cmd_exit_qpi = PSRAM_EXIT_QMODE << 8;
            ps_cmd.txDataBitLen = 16;
            break;
    }
    ps_cmd.txData = &cmd_exit_qpi;
    ps_cmd.cmd = 0;
    ps_cmd.cmdBitLen = 0;
    ps_cmd.addr = 0;
    ps_cmd.addrBitLen = 0;
    ps_cmd.rxData = NULL;
    ps_cmd.rxDataBitLen = 0;
    ps_cmd.dummyBitLen = 0;
    psram_cmd_config(spi_num, &ps_cmd);
    psram_cmd_start(spi_num, PSRAM_CMD_QPI);
}

//read psram id
static void psram_read_id(uint32_t* dev_id)
{
    psram_spi_num_t spi_num = PSRAM_SPI_1;
    psram_disable_qio_mode(spi_num);
    uint32_t addr = (PSRAM_DEVICE_ID << 24) | 0;
    uint32_t dummy_bits = 0;
    psram_cmd_t ps_cmd;
    switch (s_psram_mode) {
        case PSRAM_CACHE_F80M_S80M:
            dummy_bits = 0 + extra_dummy;
            ps_cmd.cmdBitLen = 0;
            break;
        case PSRAM_CACHE_F80M_S40M:
        case PSRAM_CACHE_F40M_S40M:
        default:
            dummy_bits = 0 + extra_dummy;
            ps_cmd.cmdBitLen = 2;   //this two bits is used to delay 2 clock cycle
            break;
    }
    ps_cmd.cmd = 0;
    ps_cmd.addr = &addr;
    ps_cmd.addrBitLen = 4 * 8;
    ps_cmd.txDataBitLen = 0;
    ps_cmd.txData = NULL;
    ps_cmd.rxDataBitLen = 4 * 8;
    ps_cmd.rxData = dev_id;
    ps_cmd.dummyBitLen = dummy_bits;
    psram_cmd_config(spi_num, &ps_cmd);
    psram_clear_spi_fifo(spi_num);
    psram_recv_start(spi_num, ps_cmd.rxData, ps_cmd.rxDataBitLen / 8, PSRAM_CMD_SPI);
}

//enter QPI mode
static esp_err_t IRAM_ATTR psram_enable_qio_mode(psram_spi_num_t spi_num)
{
    psram_cmd_t ps_cmd;
    uint32_t addr = (PSRAM_ENTER_QMODE << 24) | 0;
    switch (s_psram_mode) {
        case PSRAM_CACHE_F80M_S80M:
            ps_cmd.cmdBitLen = 0;
            break;
        case PSRAM_CACHE_F80M_S40M:
        case PSRAM_CACHE_F40M_S40M:
        default:
            ps_cmd.cmdBitLen = 2;
            break;
    }
    ps_cmd.cmd = 0;
    ps_cmd.addr = &addr;
    ps_cmd.addrBitLen = 8;
    ps_cmd.txData = NULL;
    ps_cmd.txDataBitLen = 0;
    ps_cmd.rxData = NULL;
    ps_cmd.rxDataBitLen = 0;
    ps_cmd.dummyBitLen = 0;
    psram_cmd_config(spi_num, &ps_cmd);
    psram_cmd_start(spi_num, PSRAM_CMD_SPI);
    return ESP_OK;
}

//spi param init for psram
void IRAM_ATTR psram_spi_init(psram_spi_num_t spi_num, psram_cache_mode_t mode)
{
    uint8_t i, k;
    DPORT_CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(spi_num), SPI_TRANS_DONE << 5);
    DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_CS_SETUP);
    // SPI_CPOL & SPI_CPHA
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(spi_num), SPI_CK_IDLE_EDGE);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_CK_OUT_EDGE);
    // SPI bit order
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_WR_BIT_ORDER);
    DPORT_CLEAR_PERI_REG_MASK(SPI_CTRL_REG(spi_num), SPI_RD_BIT_ORDER);
    // SPI bit order
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_DOUTDIN);
    // May be not must to do.
    DPORT_WRITE_PERI_REG(SPI_USER1_REG(spi_num), 0);
    // SPI mode type
    DPORT_CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(spi_num), SPI_SLAVE_MODE);
    switch (mode) {
        case PSRAM_CACHE_F80M_S80M:
            DPORT_WRITE_PERI_REG(SPI_CLOCK_REG(spi_num), SPI_CLK_EQU_SYSCLK); // 80Mhz speed
            break;
        case PSRAM_CACHE_F80M_S40M:
        case PSRAM_CACHE_F40M_S40M:
        default:
            i = (2 / 40) ? (2 / 40) : 1;
            k = 2 / i;
            DPORT_CLEAR_PERI_REG_MASK(SPI_CLOCK_REG(spi_num), SPI_CLK_EQU_SYSCLK);
            DPORT_WRITE_PERI_REG(SPI_CLOCK_REG(spi_num),
                    (((i - 1) & SPI_CLKDIV_PRE) << SPI_CLKDIV_PRE_S) |
                    (((k - 1) & SPI_CLKCNT_N) << SPI_CLKCNT_N_S) |
                    ((((k + 1) / 2 - 1) & SPI_CLKCNT_H) << SPI_CLKCNT_H_S) |
                    (((k - 1) & SPI_CLKCNT_L) << SPI_CLKCNT_L_S)); //clear bit 31,set SPI clock div
            break;
    }
    // Enable MOSI
    DPORT_SET_PERI_REG_MASK(SPI_USER_REG(spi_num), SPI_CS_SETUP | SPI_CS_HOLD | SPI_USR_MOSI);
    memset((void*)SPI_W0_REG(spi_num), 0, 16 * 4);
}

//psram gpio init , different working frequency we have different solutions
esp_err_t IRAM_ATTR psram_enable(psram_cache_mode_t mode, psram_vaddr_mode_t vaddrmode)   //psram init
{
    DPORT_WRITE_PERI_REG(GPIO_ENABLE_W1TC_REG, BIT(PSRAM_CLK_IO) | BIT(PSRAM_CS_IO));   //DISABLE OUPUT FOR IO16/17
    assert(mode == PSRAM_CACHE_F40M_S40M && "we don't support any other mode for now.");
    s_psram_mode = mode;

    DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI_CLK_EN);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI_RST);
    DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI_CLK_EN_1);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI_RST_1);
    DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI_CLK_EN_2);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI_RST_2);

    DPORT_WRITE_PERI_REG(SPI_EXT3_REG(0), 0x1);
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_USR_PREP_HOLD_M);

    switch (mode) {
        case PSRAM_CACHE_F80M_S80M:
            psram_spi_init(PSRAM_SPI_1, mode);
            extra_dummy = 2;
            DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_CS_HOLD);
            gpio_matrix_out(PSRAM_CS_IO, SPICS1_OUT_IDX, 0, 0);
            gpio_matrix_out(PSRAM_CLK_IO, VSPICLK_OUT_IDX, 0, 0);
            //use spi3 clock,but use spi1 data/cs wires
            DPORT_WRITE_PERI_REG(SPI_ADDR_REG(PSRAM_SPI_3), 32 << 24);
            DPORT_WRITE_PERI_REG(SPI_CLOCK_REG(PSRAM_SPI_3), SPI_CLK_EQU_SYSCLK_M);   //SET 80M AND CLEAR OTHERS
            DPORT_SET_PERI_REG_MASK(SPI_CMD_REG(PSRAM_SPI_3), SPI_FLASH_READ_M);
            uint32_t spi_status;
            while (1) {
                spi_status = DPORT_READ_PERI_REG(SPI_EXT2_REG(PSRAM_SPI_3));
                if (spi_status != 0 && spi_status != 1) {
                    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, BIT(PSRAM_CS_IO));   //DPORT_SPI_CLK_EN
                    break;
                }
            }
            break;
        case PSRAM_CACHE_F80M_S40M:
        case PSRAM_CACHE_F40M_S40M:
        default:
#if DISABLE_GPIO_MATRIX_FOR_40M
            extra_dummy = 0;
#else
            extra_dummy = 1;
#endif
            psram_spi_init(PSRAM_SPI_1, mode);
            DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_CS_HOLD);
            gpio_matrix_out(PSRAM_CS_IO, SPICS1_OUT_IDX, 0, 0);
            gpio_matrix_in(6, SIG_IN_FUNC224_IDX, 0);
            gpio_matrix_out(20, SIG_IN_FUNC224_IDX, 0, 0);
            gpio_matrix_in(20, SIG_IN_FUNC225_IDX, 0);
            gpio_matrix_out(PSRAM_CLK_IO, SIG_IN_FUNC225_IDX, 0, 0);
            break;
    }
    DPORT_CLEAR_PERI_REG_MASK(SPI_USER_REG(PSRAM_SPI_1), SPI_CS_SETUP_M);

#if (!DISABLE_GPIO_MATRIX_FOR_40M)
    psram_gpio_config(mode);
#endif
    DPORT_WRITE_PERI_REG(GPIO_ENABLE_W1TS_REG, BIT(PSRAM_CS_IO)| BIT(PSRAM_CLK_IO));
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PSRAM_CS_IO], 2);
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PSRAM_CLK_IO], 2);
    uint32_t id;
    psram_read_id(&id);
    if (((id >> PSRAM_MFG_ID_S) & PSRAM_MFG_ID_M) != PSRAM_MFG_ID_ESPRESSIF) {
        return ESP_FAIL;
    }
    psram_enable_qio_mode(PSRAM_SPI_1);
    psram_cache_init(mode, vaddrmode);
    return ESP_OK;
}

//register initialization for sram cache params and r/w commands
static void IRAM_ATTR psram_cache_init(psram_cache_mode_t psram_cache_mode, psram_vaddr_mode_t vaddrmode)
{
    DPORT_CLEAR_PERI_REG_MASK(SPI_CLOCK_REG(0), SPI_CLK_EQU_SYSCLK_M);
    DPORT_SET_PERI_REG_BITS(SPI_CLOCK_REG(0), SPI_CLKDIV_PRE_V, 0, SPI_CLKDIV_PRE_S);
    DPORT_SET_PERI_REG_BITS(SPI_CLOCK_REG(0), SPI_CLKCNT_N, 1, SPI_CLKCNT_N_S);
    DPORT_SET_PERI_REG_BITS(SPI_CLOCK_REG(0), SPI_CLKCNT_H, 0, SPI_CLKCNT_H_S);
    DPORT_SET_PERI_REG_BITS(SPI_CLOCK_REG(0), SPI_CLKCNT_L, 1, SPI_CLKCNT_L_S);

    switch (psram_cache_mode) {
        case PSRAM_CACHE_F80M_S80M:
            DPORT_CLEAR_PERI_REG_MASK(SPI_DATE_REG(0), BIT(31));   //flash 1 div clk,80+40;
            DPORT_CLEAR_PERI_REG_MASK(SPI_DATE_REG(0), BIT(30)); //pre clk div , ONLY IF SPI/SRAM@ DIFFERENT SPEED,JUST FOR SPI0. FLASH DIV 2+SRAM DIV4
            DPORT_WRITE_PERI_REG(SPI_CLOCK_REG(0), SPI_CLK_EQU_SYSCLK_M);   //SET 1DIV CLOCK AND RESET OTHER PARAMS
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_USR_RD_SRAM_DUMMY_M);   //enable cache read dummy
            DPORT_SET_PERI_REG_BITS(SPI_CACHE_SCTRL_REG(0), SPI_SRAM_DUMMY_CYCLELEN_V, 3 + extra_dummy,
                    SPI_SRAM_DUMMY_CYCLELEN_S); //dummy, psram cache :  40m--+1dummy,80m--+2dummy
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_CACHE_SRAM_USR_RCMD_M); //enable user mode for cache read command
            break;
        case PSRAM_CACHE_F80M_S40M:
            DPORT_SET_PERI_REG_MASK(SPI_DATE_REG(0), BIT(31)); //flash 1 div clk
            DPORT_CLEAR_PERI_REG_MASK(SPI_DATE_REG(0), BIT(30)); //pre clk div , ONLY IF SPI/SRAM@ DIFFERENT SPEED,JUST FOR SPI0.
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_USR_RD_SRAM_DUMMY_M); //enable cache read dummy
            DPORT_SET_PERI_REG_BITS(SPI_CACHE_SCTRL_REG(0), SPI_SRAM_DUMMY_CYCLELEN_V, 3 + extra_dummy,
                    SPI_SRAM_DUMMY_CYCLELEN_S); //dummy, psram cache :  40m--+1dummy,80m--+2dummy
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_CACHE_SRAM_USR_RCMD_M); //enable user mode for cache read command
            break;
        case PSRAM_CACHE_F40M_S40M:
        default:
            DPORT_CLEAR_PERI_REG_MASK(SPI_DATE_REG(0), BIT(31)); //flash 1 div clk
            DPORT_CLEAR_PERI_REG_MASK(SPI_DATE_REG(0), BIT(30)); //pre clk div
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_USR_RD_SRAM_DUMMY_M); //enable cache read dummy
            DPORT_SET_PERI_REG_BITS(SPI_CACHE_SCTRL_REG(0), SPI_SRAM_DUMMY_CYCLELEN_V, 3 + extra_dummy,
                    SPI_SRAM_DUMMY_CYCLELEN_S); //dummy, psram cache :  40m--+1dummy,80m--+2dummy
            DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_CACHE_SRAM_USR_RCMD_M); //enable user mode for cache read command
            break;
    }
    DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_CACHE_SRAM_USR_WCMD_M);     // cache write command enable
    DPORT_SET_PERI_REG_BITS(SPI_CACHE_SCTRL_REG(0), SPI_SRAM_ADDR_BITLEN_V, 23, SPI_SRAM_ADDR_BITLEN_S); //write address for cache command.
    DPORT_SET_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_USR_SRAM_QIO_M);     //enable qio mode for cache command
    DPORT_CLEAR_PERI_REG_MASK(SPI_CACHE_SCTRL_REG(0), SPI_USR_SRAM_DIO_M);     //disable dio mode for cache command

    //config sram cache r/w command
    switch (psram_cache_mode) {
        case PSRAM_CACHE_F80M_S80M: //in this mode , no delay is needed
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DWR_CMD_REG(0), SPI_CACHE_SRAM_USR_WR_CMD_BITLEN, 7,
                    SPI_CACHE_SRAM_USR_WR_CMD_BITLEN_S);
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DWR_CMD_REG(0), SPI_CACHE_SRAM_USR_WR_CMD_VALUE, 0x38,
                    SPI_CACHE_SRAM_USR_WR_CMD_VALUE_S); //0x38
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DRD_CMD_REG(0), SPI_CACHE_SRAM_USR_RD_CMD_BITLEN_V, 7,
                    SPI_CACHE_SRAM_USR_RD_CMD_BITLEN_S);
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DRD_CMD_REG(0), SPI_CACHE_SRAM_USR_RD_CMD_VALUE_V, 0x0b,
                    SPI_CACHE_SRAM_USR_RD_CMD_VALUE_S); //0x0b
            break;
        case PSRAM_CACHE_F80M_S40M: //is sram is @40M, need 2 cycles of delay
        case PSRAM_CACHE_F40M_S40M:
        default:
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DRD_CMD_REG(0), SPI_CACHE_SRAM_USR_RD_CMD_BITLEN_V, 15,
                    SPI_CACHE_SRAM_USR_RD_CMD_BITLEN_S); //read command length, 2 bytes(1byte for delay),sending in qio mode in cache
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DRD_CMD_REG(0), SPI_CACHE_SRAM_USR_RD_CMD_VALUE_V, 0x0b00,
                    SPI_CACHE_SRAM_USR_RD_CMD_VALUE_S); //0x0b, read command value,(0x00 for delay,0x0b for cmd)
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DWR_CMD_REG(0), SPI_CACHE_SRAM_USR_WR_CMD_BITLEN, 15,
                    SPI_CACHE_SRAM_USR_WR_CMD_BITLEN_S); //write command length,2 bytes(1byte for delay,send in qio mode in cache)
            DPORT_SET_PERI_REG_BITS(SPI_SRAM_DWR_CMD_REG(0), SPI_CACHE_SRAM_USR_WR_CMD_VALUE, 0x3800,
                    SPI_CACHE_SRAM_USR_WR_CMD_VALUE_S); //0x38, write command value,(0x00 for delay)
            break;
    }

    DPORT_CLEAR_PERI_REG_MASK(DPORT_PRO_CACHE_CTRL_REG, DPORT_PRO_DRAM_HL|DPORT_PRO_DRAM_SPLIT);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_APP_CACHE_CTRL_REG, DPORT_APP_DRAM_HL|DPORT_APP_DRAM_SPLIT);
    if (vaddrmode == PSRAM_VADDR_MODE_LOWHIGH) {
        DPORT_SET_PERI_REG_MASK(DPORT_PRO_CACHE_CTRL_REG, DPORT_PRO_DRAM_SPLIT);
        DPORT_SET_PERI_REG_MASK(DPORT_APP_CACHE_CTRL_REG, DPORT_APP_DRAM_SPLIT);
    } else if (vaddrmode == PSRAM_VADDR_MODE_EVENODD) {
        DPORT_SET_PERI_REG_MASK(DPORT_PRO_CACHE_CTRL_REG, DPORT_PRO_DRAM_HL);
        DPORT_SET_PERI_REG_MASK(DPORT_APP_CACHE_CTRL_REG, DPORT_APP_DRAM_HL);
    }

    DPORT_CLEAR_PERI_REG_MASK(DPORT_PRO_CACHE_CTRL1_REG, DPORT_PRO_CACHE_MASK_DRAM1|DPORT_PRO_CACHE_MASK_OPSDRAM); //use Dram1 to visit ext sram.
    //cache page mode : 1 -->16k  4 -->2k  0-->32k,(accord with the settings in cache_sram_mmu_set)
    DPORT_SET_PERI_REG_BITS(DPORT_PRO_CACHE_CTRL1_REG, DPORT_PRO_CMMU_SRAM_PAGE_MODE, 0, DPORT_PRO_CMMU_SRAM_PAGE_MODE_S);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_APP_CACHE_CTRL1_REG, DPORT_APP_CACHE_MASK_DRAM1|DPORT_APP_CACHE_MASK_OPSDRAM); //use Dram1 to visit ext sram.
    //cache page mode : 1 -->16k  4 -->2k  0-->32k,(accord with the settings in cache_sram_mmu_set)
    DPORT_SET_PERI_REG_BITS(DPORT_APP_CACHE_CTRL1_REG, DPORT_APP_CMMU_SRAM_PAGE_MODE, 0, DPORT_APP_CMMU_SRAM_PAGE_MODE_S);
    DPORT_CLEAR_PERI_REG_MASK(SPI_PIN_REG(0), SPI_CS1_DIS_M); //ENABLE SPI0 CS1 TO PSRAM(CS0--FLASH; CS1--SRAM)
}

