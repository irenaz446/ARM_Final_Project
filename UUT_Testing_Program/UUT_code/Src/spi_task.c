/**
 * @file    spi_task.c
 * @brief   SPI loopback test task for the UUT (Unit Under Test).
 *
 * Implements a FreeRTOS task that performs a full-duplex SPI loopback test:
 *   1. Receives a test command from the Dispatcher via a binary semaphore.
 *   2. Arms SPI1 (Slave) RX DMA, then starts SPI4 (Master) TransmitReceive DMA.
 *   3. Waits for both TX-complete and RX-complete ISR notifications.
 *   4. Validates the received data (memcmp for short patterns, CRC for long).
 *   5. Sends the result back to the PC via lwIP (thread-safe, uut_send_result).
 *
 * Hardware:  STM32F756ZG
 * Wiring:    SPI4 MOSI (PE6) --> SPI1 MOSI (PB5)
 *            SPI4 SCK  (PE2) --> SPI1 SCK  (PA5)
 *            SPI4 NSS  (PE4) --> SPI1 NSS  (PA4)
 *
 * DMA:       SPI4 TX: DMA2 Stream1 Channel4
 *            SPI4 RX: DMA2 Stream3 Channel5 (dummy — drains Master RXNE)
 *            SPI1 RX: DMA2 Stream0 Channel3 (captures loopback result)
 *
 * @note   SPI1 Slave must be armed BEFORE SPI4 Master starts — SPI has no
 *         flow control to stall the Master clock.
 * @note   SPI4 uses HAL_SPI_TransmitReceive_DMA to drain its own RX FIFO
 *         and prevent overrun on SPI1 Slave.
 */

#include "uut_task.h"
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * External HAL peripheral handles (defined by CubeMX in spi.c)
 * -------------------------------------------------------------------------*/
extern SPI_HandleTypeDef hspi4; /* Master */
extern SPI_HandleTypeDef hspi1; /* Slave  */

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t spiSemHandle;
extern TaskHandle_t      SPITaskHandle;
extern test_command_t    g_spi_cmd;
extern uint16_t          g_pc_port;

/* ---------------------------------------------------------------------------
 * Private DMA buffers
 * aligned(4): required by STM32 DMA controller (word-aligned transfers)
 * static:     file-local scope — not exposed to other modules
 * -------------------------------------------------------------------------*/
static uint8_t spi4_tx_buf[MAX_PATTERN_LEN]   __attribute__((aligned(4))); /* Master TX        */
static uint8_t spi4_rx_dummy[MAX_PATTERN_LEN] __attribute__((aligned(4))); /* Master RX drain  */
static uint8_t spi1_rx_buf[MAX_PATTERN_LEN]   __attribute__((aligned(4))); /* Slave RX result  */

/* ---------------------------------------------------------------------------
 * Task notification bit masks
 *   Bit 0: SPI4 Master TxRx complete (HAL_SPI_TxRxCpltCallback)
 *   Bit 1: SPI1 Slave  RX complete   (HAL_SPI_RxCpltCallback)
 * -------------------------------------------------------------------------*/
#define SPI_NOTIFY_MASTER_DONE  (1UL << 0)
#define SPI_NOTIFY_SLAVE_DONE   (1UL << 1)
#define SPI_NOTIFY_ALL          (SPI_NOTIFY_MASTER_DONE | SPI_NOTIFY_SLAVE_DONE)

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static uint8_t validate_spi_loopback(const test_command_t *cmd);

/* ---------------------------------------------------------------------------
 * @brief  Validates the SPI loopback result against the transmitted pattern.
 *
 * @param  cmd  Pointer to the test command containing pattern length.
 * @retval TEST_SUCCESS if data matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_spi_loopback(const test_command_t *cmd)
{
    if (cmd->pattern_len > 100) {
        return uut_validate_crc((uint32_t *)spi4_tx_buf,
                                (uint32_t *)spi1_rx_buf,
                                cmd->pattern_len);
    }

    if (memcmp(spi4_tx_buf, spi1_rx_buf, cmd->pattern_len) != 0) {
        printf("ERROR: SPI data mismatch on loopback\r\n");
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  SPI loopback test task entry point.
 *
 * Blocks on spiSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations loopback cycles and sends a pass/fail result to the PC.
 *
 * Priority: osPriorityNormal2 — must not be preempted between arming
 * the Slave and starting the Master.
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartSPITask(void *argument)
{
    ip_addr_t pc_addr;
    uint8_t   test_success;

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
            uint32_t dummy;
            xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &dummy, 0);

            /* Copy pattern into aligned TX buffer — cmd.pattern is inside
             * a packed struct and may not be word-aligned for DMA. */
            memcpy(spi4_tx_buf, cmd.pattern, cmd.pattern_len);

            /* ----------------------------------------------------------
             * Step 1: Arm SPI1 Slave RX DMA first.
             * SPI has no flow control — the Slave cannot stall the Master.
             * --------------------------------------------------------*/
            HAL_StatusTypeDef ret = HAL_SPI_Receive_DMA(&hspi1, spi1_rx_buf,
                                                         cmd.pattern_len);
            if (ret != HAL_OK) {
                printf("ERROR: SPI1 Slave RX DMA failed: %d on iteration %d\r\n",
                       ret, i);
                test_success = TEST_FAILURE;
                break;
            }

            /* ----------------------------------------------------------
             * Step 2: Start SPI4 Master TransmitReceive DMA.
             * Drains Master RX into spi4_rx_dummy to prevent SPI1 overrun.
             * --------------------------------------------------------*/
            ret = HAL_SPI_TransmitReceive_DMA(&hspi4, spi4_tx_buf,
                                              spi4_rx_dummy, cmd.pattern_len);
            if (ret != HAL_OK) {
                printf("ERROR: SPI4 Master TxRx DMA failed: %d on iteration %d\r\n",
                       ret, i);
                HAL_SPI_Abort(&hspi1);
                test_success = TEST_FAILURE;
                break;
            }

            /* ----------------------------------------------------------
             * Step 3: Wait for both Master TxRx-done and Slave RX-done.
             * --------------------------------------------------------*/
            uint32_t notified_bits = 0;
            BaseType_t notified = xTaskNotifyWait(0, SPI_NOTIFY_ALL,
                                                  &notified_bits,
                                                  pdMS_TO_TICKS(500));

            if ((notified == pdFALSE) ||
                ((notified_bits & SPI_NOTIFY_ALL) != SPI_NOTIFY_ALL)) {
                printf("ERROR: SPI timeout on iteration %d (bits=0x%08lX)\r\n",
                       i, notified_bits);
                HAL_SPI_Abort(&hspi4);
                HAL_SPI_Abort(&hspi1);
                test_success = TEST_FAILURE;
                break;
            }

            /* Step 4: Validate */
            if (validate_spi_loopback(&cmd) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                HAL_SPI_Abort(&hspi4);
                HAL_SPI_Abort(&hspi1);
                break;
            }

            /* Clean up peripheral state for next iteration */
            HAL_SPI_Abort(&hspi4);
            HAL_SPI_Abort(&hspi1);

            printf("DEBUG: SPI iteration %d — PASS\r\n", i);
        }

        printf("SPI: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        uut_send_result(cmd.test_id, test_success, &pc_addr, g_pc_port);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI TX+RX complete callback — SPI4 Master TransmitReceive_DMA.
 * @param  hspi  SPI handle that completed.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI4) {
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI RX complete callback — SPI1 Slave Receive_DMA.
 * @param  hspi  SPI handle that completed.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI1) {
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL SPI error callback — notifies task to unblock immediately.
 * @param  hspi  SPI handle that encountered an error.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t x = pdFALSE;

    if (hspi->Instance == SPI4) {
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_MASTER_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
    if (hspi->Instance == SPI1) {
        xTaskNotifyFromISR(SPITaskHandle, SPI_NOTIFY_SLAVE_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}
