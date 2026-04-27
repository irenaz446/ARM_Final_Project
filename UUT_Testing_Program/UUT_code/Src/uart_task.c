/**
 * @file    uart_task.c
 * @brief   UART loopback test task for the UUT (Unit Under Test).
 *
 * Implements a FreeRTOS task that performs a full-duplex UART loopback test:
 *   1. Receives a test command from the Dispatcher via a binary semaphore.
 *   2. Transmits a pattern from UART4 to UART5 using DMA.
 *   3. Bounces the received data back from UART5 to UART4 using DMA.
 *   4. Validates the received data (memcmp for short patterns, CRC for long).
 *   5. Sends the result back to the PC via lwIP (thread-safe, tcpip_callback).
 *
 * Hardware:  STM32F756ZG
 * Wiring:    UART4 TX --> UART5 RX
 *            UART5 TX --> UART4 RX
 *
 * @note   All lwIP API calls are delegated to the lwIP core task via
 *         uut_send_result() — see uut_task.c.
 */

#include "uut_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External HAL peripheral handles (defined by CubeMX in usart.c / crc.c)
 * -------------------------------------------------------------------------*/
extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart5;

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t uartSemHandle;
extern TaskHandle_t      UARTTaskHandle;
extern test_command_t    g_uart_cmd;
extern uint16_t          g_pc_port;

/* ---------------------------------------------------------------------------
 * Private DMA buffers
 * aligned(4): required by STM32 DMA controller (word-aligned transfers)
 * static:     file-local scope — not exposed to other modules
 * -------------------------------------------------------------------------*/
static uint8_t u4_rx_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* UART4 RX */
static uint8_t u5_rx_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* UART5 RX */

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static uint8_t validate_loopback(const test_command_t *cmd);

/* ---------------------------------------------------------------------------
 * @brief  Validates the loopback buffer against what UART5 received.
 *
 * Uses uut_validate_crc (hardware CRC32) for patterns longer than 100 bytes.
 * Uses memcmp for shorter patterns. Both buffers are 4-byte aligned.
 *
 * @param  cmd  Pointer to the test command containing pattern length.
 * @retval TEST_SUCCESS if data matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        return uut_validate_crc((uint32_t *)u5_rx_buf,
                                (uint32_t *)u4_rx_buf,
                                cmd->pattern_len);
    }

    if (memcmp(u5_rx_buf, u4_rx_buf, cmd->pattern_len) != 0) {
        printf("ERROR: UART data mismatch on loopback\r\n");
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  UART loopback test task entry point.
 *
 * Blocks on uartSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations loopback cycles and sends a pass/fail result to the PC.
 *
 * Priority: osPriorityNormal2 (highest among application tasks).
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartUARTTask(void const *argument)
{
    ip_addr_t pc_addr;
    uint8_t   test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(uartSemHandle, portMAX_DELAY);

        /* Take a local copy — Dispatcher may overwrite g_uart_cmd immediately */
        test_command_t cmd = g_uart_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nUART: Task woken. Starting %d iteration(s), pattern_len=%d\r\n",
               cmd.iterations, cmd.pattern_len);

        for (int i = 0; i < cmd.iterations; i++) {

            /* ----------------------------------------------------------
             * Leg 1: UART4 TX --> UART5 RX
             * Arm UART5 receiver first, then fire UART4 transmitter.
             * ulTaskNotifyTake blocks until RxCpltCallback fires (500ms).
             * --------------------------------------------------------*/
            HAL_UART_Receive_DMA(&huart5, u5_rx_buf, cmd.pattern_len);
            HAL_UART_Transmit_DMA(&huart4, cmd.pattern, cmd.pattern_len);

            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
                printf("ERROR: UART5 RX timeout on iteration %d\r\n", i);
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: Iteration %d — UART5 received. Bouncing back...\r\n", i);

            /* ----------------------------------------------------------
             * Leg 2: UART5 TX --> UART4 RX
             * u5_rx_buf holds the data from Leg 1 — transmit it back.
             * ulTaskNotifyTake blocks until RxCpltCallback fires (500ms).
             * --------------------------------------------------------*/
            HAL_UART_Receive_DMA(&huart4, u4_rx_buf, cmd.pattern_len);
            HAL_UART_Transmit_DMA(&huart5, u5_rx_buf, cmd.pattern_len);

            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
                printf("ERROR: UART4 RX timeout on iteration %d\r\n", i);
                test_success = TEST_FAILURE;
                break;
            }

            if (validate_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: Iteration %d — PASS\r\n", i);
        }

        printf("UART: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        uut_send_result(cmd.test_id, test_success, &pc_addr, g_pc_port);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL UART RX complete callback — called from DMA ISR context.
 *
 * Both UART4 and UART5 notify the same task sequentially via
 * ulTaskNotifyTake, so no instance check is needed here.
 *
 * @param  huart  Unused — both UARTs notify the same task.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t x = pdFALSE;

    (void)huart;
    vTaskNotifyGiveFromISR(UARTTaskHandle, &x);
    portYIELD_FROM_ISR(x);
}

/* ---------------------------------------------------------------------------
 * @brief  HAL UART error callback — called from DMA/UART ISR context.
 *
 * @param  huart  UART handle that encountered an error.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4) {
        printf("ERROR: UART4 error, code=0x%08lX\r\n", huart->ErrorCode);
    }
    if (huart->Instance == UART5) {
        printf("ERROR: UART5 error, code=0x%08lX\r\n", huart->ErrorCode);
    }
}
