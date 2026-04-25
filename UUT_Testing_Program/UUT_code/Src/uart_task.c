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
 *         tcpip_callback() to avoid thread-safety violations.
 */

#include "pc_test_uut.h"
#include "uut_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External HAL peripheral handles (defined by CubeMX in usart.c / crc.c)
 * -------------------------------------------------------------------------*/
extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart5;
extern CRC_HandleTypeDef  hcrc;

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t uartSemHandle;  /* Binary semaphore: Dispatcher -> UARTTask  */
extern TaskHandle_t      UARTTaskHandle; /* Used by ISR to notify this task directly  */
extern test_command_t    g_uart_cmd;     /* Command written by Dispatcher before Give  */
extern uint16_t          g_pc_port;     /* Source port of last UDP packet from PC     */

/* ---------------------------------------------------------------------------
 * Private DMA buffers
 * -------------------------------------------------------------------------*/
static uint8_t u4_rx_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* UART4 RX (From UART5 TX to UART4 RX) */
static uint8_t u5_rx_buf[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* UART5 RX (From UART4 TX to UART5 RX) */

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static void    send_result_callback(void *arg);
static uint8_t validate_loopback(const test_command_t *cmd);

/* ---------------------------------------------------------------------------
 * @brief  lwIP core callback — sends UDP result packet to PC.
 *
 * Executed inside the lwIP core task context (safe to call all lwIP APIs).
 * Frees the udp_send_req_t allocated by StartUARTTask before returning.
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

    vPortFree(req);
}

/* ---------------------------------------------------------------------------
 * @brief  Validates the loopback buffer against what UART5 received.
 *
 * Uses hardware CRC32 for patterns longer than 100 bytes to reduce CPU load.
 * Uses memcmp for shorter patterns. Both u5_rx_buf (reference) and u4_rx_buf
 * (result) are 4-byte aligned, satisfying the STM32 CRC peripheral requirement.
 *
 * @param  cmd  Pointer to the test command containing pattern length.
 * @retval TEST_SUCCESS if data matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        uint32_t words  = cmd->pattern_len / 4;
        uint32_t c_sent = HAL_CRC_Calculate(&hcrc, (uint32_t *)u5_rx_buf, words);
        uint32_t c_recv = HAL_CRC_Calculate(&hcrc, (uint32_t *)u4_rx_buf, words);

        if (c_sent != c_recv) {
            printf("ERROR: CRC mismatch: sent=%lu recv=%lu\r\n", c_sent, c_recv);
            return TEST_FAILURE;
        }
    } else {
        if (memcmp(u5_rx_buf, u4_rx_buf, cmd->pattern_len) != 0) {
            printf("ERROR: Data mismatch on loopback\r\n");
            return TEST_FAILURE;
        }
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  UART loopback test task entry point.
 *
 * Blocks on uartSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations loopback cycles and sends a pass/fail result to the PC.
 *
 * Priority: osPriorityNormal2 (highest among application tasks)
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartUARTTask(void const *argument)
{
    test_result_t  res;
    ip_addr_t      pc_addr;
    uint8_t        test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(uartSemHandle, portMAX_DELAY);

        test_command_t cmd = g_uart_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nUART: Task woken. Starting %d iteration(s), pattern_len=%d\r\n",
               cmd.iterations, cmd.pattern_len);

        for (int i = 0; i < cmd.iterations; i++) {

            /* ----------------------------------------------------------
             * Part 1: UART4 TX --> UART5 RX
             *
             * Prepare UART5 receiver first, then start UART4 transmitter.
             * ulTaskNotifyTake blocks until HAL_UART_RxCpltCallback
             * fires for UART5 (500 ms timeout).
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
             * Part 2: UART5 TX --> UART4 RX 
             *
             * u5_rx_buf holds the data received in Part 1 — transmit it
             * back via UART5. ulTaskNotifyTake blocks until
             * HAL_UART_RxCpltCallback fires for UART4 (500 ms timeout).
             * --------------------------------------------------------*/
            HAL_UART_Receive_DMA(&huart4, u4_rx_buf, cmd.pattern_len);
            HAL_UART_Transmit_DMA(&huart5, u5_rx_buf, cmd.pattern_len);

            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
                printf("ERROR: UART4 RX timeout on iteration %d\r\n", i);
                test_success = TEST_FAILURE;
                break;
            }

            /* ----------------------------------------------------------
             * Validation: compare u5_rx_buf (sent) vs u4_rx_buf (received).
             * Both buffers are aligned — safe for CRC peripheral access.
             * --------------------------------------------------------*/
            if (validate_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: Iteration %d — PASS\r\n", i);
        }

        printf("UART: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        /* ------------------------------------------------------------------
         * Send result to PC.
         * udp_sendto must not be called from this task — lwIP is not
         * thread-safe. Schedule the send in the lwIP core task via
         * tcpip_callback() with a heap-allocated request struct.
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
            printf("ERROR: Failed to allocate UDP send request\r\n");
        }
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL UART RX complete callback — called from DMA ISR context.
 *
 * Notifies StartUARTTask using a direct task notification and triggers
 * immediate context switch if the notified task has higher priority than
 * the currently running task.
 *
 * @param  huart  UART handle that completed reception (unused — both UARTs
 *                notify the same task sequentially).
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t x_higher_priority_task_woken = pdFALSE;

    (void)huart;
    vTaskNotifyGiveFromISR(UARTTaskHandle, &x_higher_priority_task_woken);
    portYIELD_FROM_ISR(x_higher_priority_task_woken);
}

/* ---------------------------------------------------------------------------
 * @brief  HAL UART error callback — called from DMA/UART ISR context.
 *
 * Logs the peripheral error code to the debug console for diagnostics.
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
