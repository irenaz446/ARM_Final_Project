/**
 * @file    spi_task.c
 * @brief   SPI loopback test task for the UUT (Unit Under Test).
 *
 * Implements a FreeRTOS task that performs a full-duplex SPI loopback test:
 *   1. Receives a test command from the Dispatcher via a binary semaphore.
 *   2. Arms spi1 (Slave) RX DMA, then starts spi4 (Master) TransmitReceive DMA.
 *   3. Waits for both TX-complete and RX-complete ISR notifications.
 *   4. Validates the received data (memcmp for short patterns, CRC for long).
 *   5. Sends the result back to the PC via lwIP (thread-safe, tcpip_callback).
 *
 * Hardware:  STM32F756ZG
 * Wiring:    SPI4 MOSI (PE6)  --> SPI1 MOSI (PB5)
 *            SPI4 SCK  (PE2)  --> SPI1 SCK  (PA5)
 *            SPI4 NSS  (PE4)  --> SPI1 NSS  (PA4)
 *
 * DMA:       SPI4 TX: DMA2 Stream3 Channel3
 *            spi4 RX: DMA2 Stream0 Channel3 (dummy — drains Master RXNE)
 *            SPI1 RX: DMA2 Stream3 Channel 2 (captures loopback result)
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
 * External HAL peripheral handles (defined by CubeMX in spi.c / crc.c)
 * -------------------------------------------------------------------------*/
extern SPI_HandleTypeDef hspi4; /* Master */
extern SPI_HandleTypeDef hspi1; /* Slave  */
extern CRC_HandleTypeDef hcrc; /*CRC Calculation */

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t spiSemHandle;  /* Binary semaphore: Dispatcher -> SPITask  */
extern TaskHandle_t      SPITaskHandle; /* Used by ISR to notify this task directly  */
extern test_command_t    g_spi_cmd;     /* Command written by Dispatcher before Give  */
extern uint16_t          g_pc_port;     /* Source port of last UDP packet from PC     */

/* ---------------------------------------------------------------------------
 * Private DMA buffers
 * aligned(4): required by STM32 DMA controller (word-aligned transfers)
 * static:     file-local scope — not exposed to other modules
 * -------------------------------------------------------------------------*/
static uint8_t spi4_tx_buf[MAX_PATTERN_LEN]   __attribute__((aligned(4))); /* Master TX pattern      */
static uint8_t spi4_rx_dummy[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* Master RX drain buffer */
static uint8_t spi1_rx_buf[MAX_PATTERN_LEN]   __attribute__((aligned(4))); /* Slave RX result        */

/* ---------------------------------------------------------------------------
 * Private notification bit masks
 *
 * SPITask waits for TWO notifications per iteration using xTaskNotifyWait:
 *   - Bit 0: spi4 Master TxRx complete (HAL_SPI_TxRxCpltCallback)
 *   - Bit 1: spi1 Slave  RX complete   (HAL_SPI_RxCpltCallback)
 * -------------------------------------------------------------------------*/
#define SPI_NOTIFY_MASTER_DONE  (1UL << 0)
#define SPI_NOTIFY_SLAVE_DONE   (1UL << 1)

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static void    send_result_callback(void *arg);
static uint8_t validate_spi_loopback(const test_command_t *cmd);

/* ---------------------------------------------------------------------------
 * @brief  lwIP core callback — sends UDP result packet to PC.
 *
 * Executed inside the lwIP core task context (safe to call all lwIP APIs).
 * Frees the udp_send_req_t allocated by StartSPITask before returning.
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
 * @brief  Validates the SPI loopback result against the transmitted pattern.
 *
 * Uses hardware CRC32 for patterns longer than 100 bytes to reduce CPU load.
 * Uses memcmp for shorter patterns. Both spi4_tx_buf and spi1_rx_buf are
 * 4-byte aligned, satisfying the STM32 CRC peripheral requirement.
 *
 * @param  cmd  Pointer to the test command containing pattern length.
 * @retval TEST_SUCCESS if data matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_spi_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        uint32_t words  = cmd->pattern_len / 4;
        uint32_t c_sent = HAL_CRC_Calculate(&hcrc, (uint32_t *)spi4_tx_buf, words);
        uint32_t c_recv = HAL_CRC_Calculate(&hcrc, (uint32_t *)spi1_rx_buf, words);

        if (c_sent != c_recv) {
            printf("ERROR: SPI CRC mismatch: sent=%lu recv=%lu\r\n", c_sent, c_recv);
            return TEST_FAILURE;
        }
    } else {
        if (memcmp(spi4_tx_buf, spi1_rx_buf, cmd->pattern_len) != 0) {
            printf("ERROR: SPI data mismatch on loopback\r\n");
            return TEST_FAILURE;
        }
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  SPI loopback test task entry point.
 *
 * Blocks on spiSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations loopback cycles and sends a pass/fail result to the PC.
 *
 * Priority: osPriorityNormal2 (highest among application tasks) — must not
 * be preempted between arming the Slave and starting the Master.
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartSPITask(void *argument)
{
    test_result_t  res;
    ip_addr_t      pc_addr;
    uint8_t        test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(spiSemHandle, portMAX_DELAY);

        /* Take a local copy — Dispatcher may overwrite g_spi_cmd immediately */
        test_command_t cmd = g_spi_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nSPI: Task woken. Starting %d iteration(s), pattern_len=%d\r\n",
               cmd.iterations, cmd.pattern_len);
        for (int i = 0; i < cmd.iterations; i++) {
            /* Clear stale notification bits from any previous iteration */
            uint32_t dummy_bits;
            xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &dummy_bits, 0);

            /* Copy pattern into aligned TX buffer.
             * cmd.pattern is at offset 7 in a packed struct — not word-aligned,
             * which would cause a DMA fault on STM32. */
            memcpy(spi4_tx_buf, cmd.pattern, cmd.pattern_len);

            /* ----------------------------------------------------------
             * Step 1: Arm spi1 Slave RX DMA first.
             *
             * SPI has no flow control — the Slave cannot stall the Master.
             * SPITask runs at highest priority so nothing preempts it
             * between this call and Step 2.
             * --------------------------------------------------------*/
            HAL_StatusTypeDef ret = HAL_SPI_Receive_DMA(&hspi1, spi1_rx_buf,
                                                         cmd.pattern_len);
            if (ret != HAL_OK) {
                printf("ERROR: spi1 Slave RX DMA failed: %d on iteration %d\r\n",
                       ret, i);
                test_success = TEST_FAILURE;
                break;
            }

            /* ----------------------------------------------------------
             * Step 2: Start spi4 Master TransmitReceive DMA.
             *
             * HAL_SPI_TransmitReceive_DMA drives both TX DMA (spi4_tx_buf)
             * and RX DMA (spi4_rx_dummy) on spi4 simultaneously.
             * --------------------------------------------------------*/
            ret = HAL_SPI_TransmitReceive_DMA(&hspi4, spi4_tx_buf,
                                              spi4_rx_dummy, cmd.pattern_len);
            if (ret != HAL_OK) {
                printf("ERROR: spi4 Master TxRx DMA failed: %d on iteration %d\r\n",
                       ret, i);
                HAL_SPI_Abort(&hspi1);
                test_success = TEST_FAILURE;
                break;
            }

            /* Step 3: Wait for BOTH Master TxRx-done AND Slave RX-done */
            uint32_t notified_bits = 0;
            BaseType_t notified = xTaskNotifyWait(0,
                                                  SPI_NOTIFY_MASTER_DONE | SPI_NOTIFY_SLAVE_DONE,
                                                  &notified_bits,
                                                  pdMS_TO_TICKS(500));

            if ((notified == pdFALSE) ||
                ((notified_bits & (SPI_NOTIFY_MASTER_DONE | SPI_NOTIFY_SLAVE_DONE)) !=
                                  (SPI_NOTIFY_MASTER_DONE | SPI_NOTIFY_SLAVE_DONE))) {
                printf("ERROR: SPI timeout on iteration %d (bits=0x%08lX)\r\n",
                       i, notified_bits);
                HAL_SPI_Abort(&hspi4);
                HAL_SPI_Abort(&hspi1);
                test_success = TEST_FAILURE;
                break;
            }
            printf("DEBUG: SPI iteration %d — transfer complete. Validating...\r\n", i);

            /* ----------------------------------------------------------
             * Step 4: Validate spi4_tx_buf (sent) vs spi1_rx_buf (received).
             * Both buffers are word-aligned — safe for CRC peripheral.
             * --------------------------------------------------------*/
            if (validate_spi_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                HAL_SPI_Abort(&hspi4);
                HAL_SPI_Abort(&hspi1);
                break;
            }

            /* Clean up for next iteration */
            HAL_SPI_Abort(&hspi4);
            HAL_SPI_Abort(&hspi1);
            printf("DEBUG: SPI iteration %d — PASS\r\n", i);
        }

        printf("SPI: Test finished. Result: %s\r\n",
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
            printf("ERROR: SPI: Failed to allocate UDP send request\r\n");
        }
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI TX+RX complete callback — called when spi4 Master
 *         HAL_SPI_TransmitReceive_DMA completes.
 *
 * Sets SPI_NOTIFY_MASTER_DONE in SPITask's notification value.
 * SPITask unblocks only when both MASTER_DONE and SLAVE_DONE are set.
 *
 * @param  hspi  SPI handle that completed the transfer.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI4) {
        xTaskNotifyFromISR(SPITaskHandle,
                           SPI_NOTIFY_MASTER_DONE,
                           eSetBits,
                           &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI RX complete callback — called when spi1 Slave
 *         HAL_SPI_Receive_DMA completes.
 *
 * Sets SPI_NOTIFY_SLAVE_DONE in SPITask's notification value.
 * SPITask unblocks only when both MASTER_DONE and SLAVE_DONE are set.
 *
 * @param  hspi  SPI handle that completed reception.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI1) {
        xTaskNotifyFromISR(SPITaskHandle,
                           SPI_NOTIFY_SLAVE_DONE,
                           eSetBits,
                           &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI error callback — called from DMA/SPI ISR context.
 *
 * Notifies SPITask immediately so it unblocks and reports failure rather
 * than waiting the full 500 ms timeout.
 *
 * @param  hspi  SPI handle that encountered an error.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
	BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI4) {
        printf("ERROR: spi4 error, code=0x%08lX\r\n", hspi->ErrorCode);
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
    if (hspi->Instance == SPI1) {
        printf("ERROR: spi1 error, code=0x%08lX\r\n", hspi->ErrorCode);
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}
