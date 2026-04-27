/**
 * @file    i2c_task.c
 * @brief   I2C loopback test task for the UUT (Unit Under Test).
 *
 * Performs a two-phase I2C loopback test:
 *   Phase 1 — Write (I2C4 Master TX → I2C2 Slave RX, both DMA).
 *   Phase 2 — Read  (I2C4 Master RX ← I2C2 Slave TX, both IT).
 *   Validation: Master TX buffer vs Master RX buffer.
 *
 * Hardware:  STM32F756ZG
 * Wiring:    I2C4 SDA <--> I2C2 SDA  (with pull-up resistors)
 *            I2C4 SCL <--> I2C2 SCL  (with pull-up resistors)
 *
 * DMA:       I2C4 TX: DMA1 Stream5  (Phase 1 Master transmit)
 *            I2C2 RX: DMA1 Stream3  (Phase 1 Slave receive)
 *            I2C4 RX: IT mode       (Phase 2 Master receive — no DMA available)
 *            I2C2 TX: IT mode       (Phase 2 Slave transmit — no DMA available)
 *
 * Slave address: 0x52 (7-bit) — must match I2C2 Own Address in CubeMX.
 */

#include "uut_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External HAL peripheral handles (defined by CubeMX in i2c.c)
 * -------------------------------------------------------------------------*/
extern I2C_HandleTypeDef hi2c4; /* Master */
extern I2C_HandleTypeDef hi2c2; /* Slave  */

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t i2cSemHandle;
extern TaskHandle_t      I2CTaskHandle;
extern test_command_t    g_i2c_cmd;
extern uint16_t          g_pc_port;

/* ---------------------------------------------------------------------------
 * I2C slave address
 * SLAVE_ADDR_7BIT must match I2C2 Own Address 1 in CubeMX.
 * HAL expects the address left-shifted by 1 (8-bit format).
 * -------------------------------------------------------------------------*/
#define SLAVE_ADDR_7BIT  0x52
#define SLAVE_ADDR_8BIT  (SLAVE_ADDR_7BIT << 1)

/* ---------------------------------------------------------------------------
 * Task notification bit masks (shared across both phases)
 *   Bit 0: Master operation complete
 *   Bit 1: Slave  operation complete
 * -------------------------------------------------------------------------*/
#define I2C_NOTIFY_MASTER_DONE  (1UL << 0)
#define I2C_NOTIFY_SLAVE_DONE   (1UL << 1)
#define I2C_NOTIFY_ALL          (I2C_NOTIFY_MASTER_DONE | I2C_NOTIFY_SLAVE_DONE)

/* ---------------------------------------------------------------------------
 * Private transfer buffers
 * aligned(4): required for CRC peripheral access
 * static:     file-local scope
 * -------------------------------------------------------------------------*/
static uint8_t i2c_tx_buf[MAX_PATTERN_LEN]    __attribute__((aligned(4))); /* Master TX         */
static uint8_t i2c_slave_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* Slave RX, then TX */
static uint8_t i2c_rx_buf[MAX_PATTERN_LEN]    __attribute__((aligned(4))); /* Master RX result  */

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static uint8_t run_phase1_write(uint8_t pattern_len);
static uint8_t run_phase2_read(uint8_t pattern_len);
static uint8_t validate_loopback(const test_command_t *cmd);

/* ---------------------------------------------------------------------------
 * @brief  Phase 1 — I2C4 Master Write → I2C2 Slave Receive (DMA).
 * -------------------------------------------------------------------------*/
static uint8_t run_phase1_write(uint8_t pattern_len)
{
    uint32_t notif = 0;

    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Arm Slave first — Master START is immediate after Master call */
    HAL_StatusTypeDef ret = HAL_I2C_Slave_Receive_DMA(&hi2c2, i2c_slave_buf,
                                                        pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C2 Slave RX DMA start failed: %d\r\n", ret);
        return TEST_FAILURE;
    }

    __DSB();
    __ISB();

    ret = HAL_I2C_Master_Transmit_DMA(&hi2c4, SLAVE_ADDR_8BIT, i2c_tx_buf,
                                       pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C4 Master TX DMA start failed: %d\r\n", ret);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif,
                                          pdMS_TO_TICKS(1000));

    if ((notified == pdFALSE) || ((notif & I2C_NOTIFY_ALL) != I2C_NOTIFY_ALL)) {
        printf("ERROR: I2C Phase1 timeout (bits=0x%08lX)\r\n", notif);
        HAL_I2C_Master_Abort_IT(&hi2c4, SLAVE_ADDR_8BIT);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Phase 2 — I2C4 Master Receive ← I2C2 Slave Transmit (IT).
 *
 * IT mode used because DMA is not configured for these directions.
 * -------------------------------------------------------------------------*/
static uint8_t run_phase2_read(uint8_t pattern_len)
{
    uint32_t notif = 0;

    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Arm Slave TX first */
    HAL_StatusTypeDef ret = HAL_I2C_Slave_Transmit_IT(&hi2c2, i2c_slave_buf,
                                                        pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C2 Slave TX IT start failed: %d\r\n", ret);
        return TEST_FAILURE;
    }

    __DSB();
    __ISB();

    ret = HAL_I2C_Master_Receive_IT(&hi2c4, SLAVE_ADDR_8BIT, i2c_rx_buf,
                                     pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C4 Master RX IT start failed: %d\r\n", ret);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif,
                                          pdMS_TO_TICKS(1000));

    if ((notified == pdFALSE) || ((notif & I2C_NOTIFY_ALL) != I2C_NOTIFY_ALL)) {
        printf("ERROR: I2C Phase2 timeout (bits=0x%08lX)\r\n", notif);
        HAL_I2C_Master_Abort_IT(&hi2c4, SLAVE_ADDR_8BIT);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Validates Master RX buffer against Master TX buffer.
 * -------------------------------------------------------------------------*/
static uint8_t validate_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        return uut_validate_crc((uint32_t *)i2c_tx_buf,
                                (uint32_t *)i2c_rx_buf,
                                cmd->pattern_len);
    }

    if (memcmp(i2c_tx_buf, i2c_rx_buf, cmd->pattern_len) != 0) {
        printf("ERROR: I2C data mismatch on loopback\r\n");
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  I2C loopback test task entry point.
 * @param  argument  Unused.
 * @retval None (infinite loop)
 * -------------------------------------------------------------------------*/
void StartI2CTask(void *argument)
{
    ip_addr_t pc_addr;
    uint8_t   test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        xSemaphoreTake(i2cSemHandle, portMAX_DELAY);

        test_command_t cmd = g_i2c_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nI2C: Task woken. Starting %d iteration(s), pattern_len=%d\r\n",
               cmd.iterations, cmd.pattern_len);

        for (int i = 0; i < cmd.iterations; i++) {

            memset(i2c_slave_buf, 0, cmd.pattern_len);
            memset(i2c_rx_buf,    0, cmd.pattern_len);
            memcpy(i2c_tx_buf, cmd.pattern, cmd.pattern_len);

            /* ---- Phase 1: I2C4 Master Write → I2C2 Slave Receive (DMA) ---- */
            if (run_phase1_write(cmd.pattern_len) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: I2C iteration %d — Phase1 done. Starting Phase2...\r\n", i);

            /* ---- Phase 2: I2C4 Master Read ← I2C2 Slave Transmit (IT) ---- */
            if (run_phase2_read(cmd.pattern_len) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            if (validate_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: I2C iteration %d — PASS\r\n", i);
        }

        printf("I2C: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        uut_send_result(cmd.test_id, test_success, &pc_addr, g_pc_port);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL I2C Master TX complete callback (Phase 1, DMA).
 *
 * Called when I2C4 Master DMA transmission completes.
 * Sets I2C_NOTIFY_MASTER_DONE in I2CTask's notification value.
 *
 * @param  hi2c  I2C handle that completed transmission.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    BaseType_t x = pdFALSE;
    if (hi2c->Instance == I2C4) {
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL I2C Slave RX complete callback (Phase 1, DMA).
 *
 * Called when I2C2 Slave DMA reception completes.
 * Sets I2C_NOTIFY_SLAVE_DONE in I2CTask's notification value.
 *
 * @param  hi2c  I2C handle that completed reception.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    BaseType_t x = pdFALSE;
    if (hi2c->Instance == I2C2) {
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL I2C Master RX complete callback (Phase 2, IT).
 *
 * Called when I2C4 Master IT reception completes.
 * Sets I2C_NOTIFY_MASTER_DONE in I2CTask's notification value.
 *
 * @param  hi2c  I2C handle that completed reception.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    BaseType_t x = pdFALSE;
    if (hi2c->Instance == I2C4) {
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL I2C Slave TX complete callback (Phase 2, IT).
 *
 * Called when I2C2 Slave IT transmission completes.
 * Sets I2C_NOTIFY_SLAVE_DONE in I2CTask's notification value.
 *
 * @param  hi2c  I2C handle that completed transmission.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    BaseType_t x = pdFALSE;
    if (hi2c->Instance == I2C2) {
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL I2C error callback — called from I2C/DMA ISR context.
 *
 * Notifies I2CTask immediately with the relevant bit so it unblocks and
 * reports failure rather than waiting the full 1000 ms timeout.
 * ErrorCode bit definitions: stm32f7xx_hal_i2c.h (HAL_I2C_ERROR_*).
 *
 * @param  hi2c  I2C handle that encountered an error.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    BaseType_t x = pdFALSE;
    if (hi2c->Instance == I2C4) {
        printf("ERROR: I2C4 error, code=0x%08lX\r\n", hi2c->ErrorCode);
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
    if (hi2c->Instance == I2C2) {
        printf("ERROR: I2C2 error, code=0x%08lX\r\n", hi2c->ErrorCode);
        xTaskNotifyFromISR(I2CTaskHandle, I2C_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}
