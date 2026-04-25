/**
 * @file    i2c_task.c
 * @brief   I2C loopback test task for the UUT (Unit Under Test).
 *
 * Implements a FreeRTOS task that performs a two-phase I2C loopback test:
 *
 *   Phase 1 — Write (Master TX → Slave RX):
 *     I2C4 Master transmits pattern via DMA → I2C2 Slave receives via DMA.
 *
 *   Phase 2 — Read (Master RX ← Slave TX):
 *     I2C2 Slave transmits the received data back via IT →
 *     I2C4 Master receives via IT.
 *     (No DMA available for these directions — IT mode used instead.)
 *
 *   Validation: compare Master TX buffer against Master RX buffer.
 *
 * Hardware:  STM32F756ZG
 * Wiring:    I2C4 SDA  <-->  I2C2 SDA  (with pull-up resistors)
 *            I2C4 SCL  <-->  I2C2 SCL  (with pull-up resistors)
 *
 * DMA:       I2C4 TX: DMA1 Stream5  (Phase 1 Master transmit)
 *            I2C2 RX: DMA1 Stream3  (Phase 1 Slave receive)
 *            I2C4 RX: IT mode       (Phase 2 Master receive)
 *            I2C2 TX: IT mode       (Phase 2 Slave transmit)
 *
 * Slave address: 0x52 (7-bit), must match I2C2 Own Address in CubeMX.
 *
 * @note   I2C Slave must be armed BEFORE the Master starts — the Master
 *         immediately sends a START + address and the Slave must be
 *         listening or it will NACK.
 *
 * @note   All lwIP API calls are delegated to the lwIP core task via
 *         tcpip_callback() to avoid thread-safety violations.
 */

#include "pc_test_uut.h"
#include "uut_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External HAL peripheral handles (defined by CubeMX in i2c.c / crc.c)
 * -------------------------------------------------------------------------*/
extern I2C_HandleTypeDef hi2c4; /* Master */
extern I2C_HandleTypeDef hi2c2; /* Slave  */
extern CRC_HandleTypeDef hcrc;

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern TaskHandle_t      I2CTaskHandle;  /* Used by ISR to notify this task directly */
extern SemaphoreHandle_t i2cSemHandle;  /* Binary semaphore: Dispatcher -> I2CTask  */
extern test_command_t    g_i2c_cmd;     /* Command written by Dispatcher before Give */
extern uint16_t          g_pc_port;     /* Source port of last UDP packet from PC    */

/* ---------------------------------------------------------------------------
 * I2C slave address
 * SLAVE_ADDR_7BIT must match I2C2 Own Address 1 configured in CubeMX.
 * HAL I2C functions expect the address left-shifted by 1 (8-bit format).
 * -------------------------------------------------------------------------*/
#define SLAVE_ADDR_7BIT  0x52
#define SLAVE_ADDR_8BIT  (SLAVE_ADDR_7BIT << 1)

/* ---------------------------------------------------------------------------
 * Task notification bit masks
 *
 * I2CTask uses xTaskNotifyWait with two bits per phase:
 *   Bit 0: Master operation complete (TX done in Phase 1, RX done in Phase 2)
 *   Bit 1: Slave  operation complete (RX done in Phase 1, TX done in Phase 2)
 * -------------------------------------------------------------------------*/
#define I2C_NOTIFY_MASTER_DONE  (1UL << 0)
#define I2C_NOTIFY_SLAVE_DONE   (1UL << 1)
#define I2C_NOTIFY_ALL          (I2C_NOTIFY_MASTER_DONE | I2C_NOTIFY_SLAVE_DONE)

/* ---------------------------------------------------------------------------
 * Private DMA / transfer buffers
 * aligned(4): required by STM32 DMA controller (word-aligned transfers)
 * static:     file-local scope — not exposed to other modules
 * -------------------------------------------------------------------------*/
static uint8_t i2c_tx_buf[MAX_PATTERN_LEN]    __attribute__((aligned(4))); /* Master TX         */
static uint8_t i2c_slave_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* Slave RX, then TX */
static uint8_t i2c_rx_buf[MAX_PATTERN_LEN]    __attribute__((aligned(4))); /* Master RX result  */

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static void    send_result_callback(void *arg);
static uint8_t validate_loopback(const test_command_t *cmd);
static uint8_t run_phase1_write(uint8_t pattern_len);
static uint8_t run_phase2_read(uint8_t pattern_len);

/* ---------------------------------------------------------------------------
 * @brief  lwIP core callback — sends UDP result packet to PC.
 *
 * Executed inside the lwIP core task context (safe to call all lwIP APIs).
 * Frees the udp_send_req_t allocated by StartI2CTask before returning.
 *
 * @param  arg  Pointer to a heap-allocated udp_send_req_t.
 * @retval None
 * -------------------------------------------------------------------------*/
static void send_result_callback(void *arg)
{
    udp_send_req_t *req = (udp_send_req_t *)arg;
    struct udp_pcb *pcb = udp_new();

    if (pcb != NULL) {
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(req->res), PBUF_RAM);
        if (p != NULL) {
            memcpy(p->payload, &req->res, sizeof(req->res));
            udp_sendto(pcb, p, &req->addr, req->port);
            pbuf_free(p);
        }
        udp_remove(pcb);
    }

    vPortFree(req); /* Always free — even if send failed */
}

/* ---------------------------------------------------------------------------
 * @brief  Validates Master RX buffer against Master TX buffer.
 *
 * Uses hardware CRC32 for patterns longer than 100 bytes to reduce CPU load.
 * Uses memcmp for shorter patterns. Both buffers are 4-byte aligned,
 * satisfying the STM32 CRC peripheral requirement.
 *
 * @param  cmd  Pointer to the test command containing pattern length.
 * @retval TEST_SUCCESS if data matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        uint32_t words  = cmd->pattern_len / 4;
        uint32_t c_sent = HAL_CRC_Calculate(&hcrc, (uint32_t *)i2c_tx_buf,  words);
        uint32_t c_recv = HAL_CRC_Calculate(&hcrc, (uint32_t *)i2c_rx_buf,  words);

        if (c_sent != c_recv) {
            printf("ERROR: I2C CRC mismatch: sent=%lu recv=%lu\r\n", c_sent, c_recv);
            return TEST_FAILURE;
        }
    } else {
        if (memcmp(i2c_tx_buf, i2c_rx_buf, cmd->pattern_len) != 0) {
            printf("ERROR: I2C data mismatch on loopback\r\n");
            return TEST_FAILURE;
        }
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Phase 1 — Master Write → Slave Receive (both via DMA).
 *
 * Arms I2C2 Slave RX DMA first, then starts I2C4 Master TX DMA.
 * Blocks on xTaskNotifyWait until both complete (1000 ms timeout).
 *
 * @param  pattern_len  Number of bytes to transfer.
 * @retval TEST_SUCCESS on completion, TEST_FAILURE on timeout or error.
 * -------------------------------------------------------------------------*/
static uint8_t run_phase1_write(uint8_t pattern_len)
{
    uint32_t notif = 0;

    /* Clear stale notification bits before arming */
    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Arm I2C2 Slave RX DMA first — Slave must be listening before
     * Master asserts START. On I2C, a missing Slave causes immediate NACK. */
    HAL_StatusTypeDef ret = HAL_I2C_Slave_Receive_DMA(&hi2c2, i2c_slave_buf, pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C2 Slave RX DMA start failed: %d\r\n", ret);
        return TEST_FAILURE;
    }

    __DSB();
    __ISB();

    /* Start I2C4 Master TX DMA — generates START + address + data */
    ret = HAL_I2C_Master_Transmit_DMA(&hi2c4, SLAVE_ADDR_8BIT, i2c_tx_buf, pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C4 Master TX DMA start failed: %d\r\n", ret);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    /* Wait for both Master TX done and Slave RX done */
    BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif, pdMS_TO_TICKS(1000));

    if ((notified == pdFALSE) ||
        ((notif & I2C_NOTIFY_ALL) != I2C_NOTIFY_ALL)) {
        printf("ERROR: I2C Phase1 timeout (bits=0x%08lX)\r\n", notif);
        HAL_I2C_Master_Abort_IT(&hi2c4, SLAVE_ADDR_8BIT);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Phase 2 — Master Receive ← Slave Transmit (both via IT).
 *
 * Arms I2C2 Slave TX IT first, then starts I2C4 Master RX IT.
 * Blocks on xTaskNotifyWait until both complete (1000 ms timeout).
 * IT mode is used because DMA is not available for these directions.
 *
 * @param  pattern_len  Number of bytes to transfer.
 * @retval TEST_SUCCESS on completion, TEST_FAILURE on timeout or error.
 * -------------------------------------------------------------------------*/
static uint8_t run_phase2_read(uint8_t pattern_len)
{
    uint32_t notif = 0;

    /* Clear stale notification bits before arming */
    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Arm I2C2 Slave TX IT — Slave sends back what it received in Phase 1 */
    HAL_StatusTypeDef ret = HAL_I2C_Slave_Transmit_IT(&hi2c2, i2c_slave_buf, pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C2 Slave TX IT start failed: %d\r\n", ret);
        return TEST_FAILURE;
    }

    __DSB();
    __ISB();

    /* Start I2C4 Master RX IT — reads data clocked out by Slave */
    ret = HAL_I2C_Master_Receive_IT(&hi2c4, SLAVE_ADDR_8BIT, i2c_rx_buf, pattern_len);
    if (ret != HAL_OK) {
        printf("ERROR: I2C4 Master RX IT start failed: %d\r\n", ret);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    /* Wait for both Master RX done and Slave TX done */
    BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif, pdMS_TO_TICKS(1000));

    if ((notified == pdFALSE) ||
        ((notif & I2C_NOTIFY_ALL) != I2C_NOTIFY_ALL)) {
        printf("ERROR: I2C Phase2 timeout (bits=0x%08lX)\r\n", notif);
        HAL_I2C_Master_Abort_IT(&hi2c4, SLAVE_ADDR_8BIT);
        HAL_I2C_DeInit(&hi2c2);
        HAL_I2C_Init(&hi2c2);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  I2C loopback test task entry point.
 *
 * Blocks on i2cSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations loopback cycles and sends a pass/fail result to the PC.
 *
 * Priority: osPriorityNormal2 (highest among application tasks).
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartI2CTask(void *argument)
{
    test_result_t  res;
    ip_addr_t      pc_addr;
    uint8_t        test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(i2cSemHandle, portMAX_DELAY);

        /* Take a local copy — Dispatcher may overwrite g_i2c_cmd immediately */
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

            /* ---- Validate: i2c_tx_buf (sent) vs i2c_rx_buf (received) ---- */
            if (validate_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: I2C iteration %d — PASS\r\n", i);
        }

        printf("I2C: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        /* ------------------------------------------------------------------
         * Send result to PC via lwIP core task (thread-safe).
         * ----------------------------------------------------------------*/
        res.test_id = cmd.test_id;
        res.result  = test_success;

        udp_send_req_t *req = pvPortMalloc(sizeof(udp_send_req_t));
        if (req != NULL) {
            req->res  = res;
            req->addr = pc_addr;
            req->port = g_pc_port;
            tcpip_callback(send_result_callback, req);
        } else {
            printf("ERROR: I2C: Failed to allocate UDP send request\r\n");
        }
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
