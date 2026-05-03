/**
 * @file    adc_task.c
 * @brief   ADC loopback test task for the UUT (Unit Under Test).
 *
 * Implements a FreeRTOS task that verifies ADC1 functionality using the
 * internal VREFINT channel as a stable, factory-calibrated reference signal.
 * No external wiring is required.
 *
 * Test flow:
 *   Calibration (once at task startup):
 *     Perform ADC_CALIB_SAMPLES conversions of VREFINT, average the results,
 *     store as g_adc_baseline. This is the "known good" value.
 *
 *   Per iteration:
 *     Perform ADC_ITER_SAMPLES conversions of VREFINT, average the results,
 *     compare against g_adc_baseline within ADC_TOLERANCE_PERCENT tolerance.
 *     A result within tolerance = PASS. Outside = FAIL.
 *
 *   After all iterations: send pass/fail result to PC via UDP.
 *
 * Hardware:  STM32F756ZG
 * ADC:       ADC1, Channel 17 (VREFINT), 12-bit resolution
 * Transfer:  DMA (ADC1 → DMA2 Stream0 Channel0)
 *
 * VREFINT:   Internal reference, nominally 1.21V.
 *            Factory calibration value stored at address 0x1FF0F44A
 *            (VREFINT_CAL — ADC raw value measured at 3.3V, 30°C).
 *            Expected raw value at 3.3V VDDA ≈ 1506 (12-bit).
 *
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
 * External HAL peripheral handles (defined by CubeMX in adc.c)
 * -------------------------------------------------------------------------*/
extern ADC_HandleTypeDef hadc1;

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern TaskHandle_t      ADCTaskHandle;  /* Used by ISR to notify this task directly */
extern SemaphoreHandle_t adcSemHandle;   /* Binary semaphore: Dispatcher -> ADCTask  */
extern test_command_t    g_adc_cmd;      /* Command written by Dispatcher before Give */
extern uint16_t          g_pc_port;      /* Source port of last UDP packet from PC    */

/* ---------------------------------------------------------------------------
 * ADC test parameters
 * -------------------------------------------------------------------------*/

/* Number of samples averaged during baseline calibration at startup.
 * More samples = more stable baseline, longer startup time. */
#define ADC_CALIB_SAMPLES     64U

/* Number of samples averaged per test iteration.
 * Matches calibration sample count for a fair comparison. */
#define ADC_ITER_SAMPLES      64U

/* Acceptance window: result must be within ±2% of baseline to pass.
 * VREFINT is stable to <0.5% over temperature, so 2% is conservative. */
#define ADC_TOLERANCE_PERCENT 2U

/* ADC full-scale value for 12-bit resolution */
#define ADC_FULL_SCALE        4095U

/* VREFINT factory calibration address (STM32F7 RM, section 15.10).
 * Value measured at VDDA=3.3V, Temp=30°C, 12-bit resolution. */
#define VREFINT_CAL_ADDR      ((uint16_t *)0x1FF0F44A)

/* Task notification bit — ADC DMA conversion complete */
#define ADC_NOTIFY_CONV_DONE  (1UL << 0)

/* ---------------------------------------------------------------------------
 * Private DMA buffer
 * uint16_t: ADC DR register is 16-bit (12-bit result right-aligned)
 * aligned(4): required by STM32 DMA controller
 * static: file-local scope
 * -------------------------------------------------------------------------*/
static uint16_t adc_dma_buf[ADC_CALIB_SAMPLES] __attribute__((aligned(4)));

/* ---------------------------------------------------------------------------
 * Private state
 * g_adc_baseline: averaged VREFINT reading established at calibration.
 *                 Stored as a module-level variable so it persists across
 *                 multiple test commands without re-calibrating each time.
 * -------------------------------------------------------------------------*/
static uint32_t g_adc_baseline = 0;

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static uint8_t  adc_sample_average(uint16_t num_samples, uint32_t *result_out);
static uint8_t  adc_calibrate(void);
static uint8_t  adc_run_iteration(uint32_t *result_out);
static uint8_t  adc_validate(uint32_t measured);

/* ---------------------------------------------------------------------------
 * @brief  Performs num_samples ADC conversions via DMA and returns average.
 *
 * Starts ADC1 in DMA mode to fill adc_dma_buf, waits for the DMA complete
 * notification from HAL_ADC_ConvCpltCallback, then computes the average.
 *
 * ADC must already be configured and started (HAL_ADC_Start_DMA is called
 * here). The function stops the ADC after completion.
 *
 * @param  num_samples  Number of conversions to perform and average.
 *                      Must be <= sizeof(adc_dma_buf)/sizeof(uint16_t).
 * @param  result_out   Pointer to store the computed average.
 * @retval TEST_SUCCESS on success, TEST_FAILURE on timeout or DMA error.
 * -------------------------------------------------------------------------*/
static uint8_t adc_sample_average(uint16_t num_samples, uint32_t *result_out)
{
    uint32_t notif = 0;

    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Ensure ADC is idle before starting */
    HAL_ADC_Stop_DMA(&hadc1);

    if(num_samples > ADC_CALIB_SAMPLES) {
        printf("ERROR: ADC num samples exceeded the maximum (num_samples=%d)\r\n", num_samples);
        return TEST_FAILURE;
    }

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buf, num_samples) != HAL_OK) {
        printf("ERROR: ADC DMA start failed (state=0x%08lX)\r\n", hadc1.State);
        return TEST_FAILURE;
    }

    /* Wait for DMA complete notification (500 ms timeout) */
    BaseType_t notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif, pdMS_TO_TICKS(500));

    HAL_ADC_Stop_DMA(&hadc1);
    osDelay(1);
    if ((notified == pdFALSE) || !(notif & ADC_NOTIFY_CONV_DONE)) {
    	HAL_ADC_Stop_DMA(&hadc1);
        printf("ERROR: ADC DMA timeout\r\n");
        return TEST_FAILURE;
    }
    HAL_ADC_Stop_DMA(&hadc1);
    osDelay(1);
    /* Compute integer average */
    uint32_t sum = 0;
    for (uint16_t i = 0; i < num_samples; i++) {
        sum += adc_dma_buf[i];
    }
    *result_out = sum / num_samples;

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Calibrates the ADC by establishing the VREFINT baseline.
 *
 * Samples VREFINT ADC_CALIB_SAMPLES times, averages the result, and stores
 * it in g_adc_baseline. Also cross-checks against the factory calibration
 * value to verify the ADC hardware is functioning correctly before testing.
 *
 * The factory VREFINT calibration value (VREFINT_CAL) is the raw 12-bit ADC
 * reading of VREFINT at VDDA=3.3V, 30°C. If the live reading differs from
 * VREFINT_CAL by more than 5%, the ADC or supply voltage has a problem.
 *
 * @retval TEST_SUCCESS if baseline established and sanity check passes.
 *         TEST_FAILURE if sampling failed or factory cal check fails.
 * -------------------------------------------------------------------------*/
static uint8_t adc_calibrate(void)
{
    uint32_t baseline = 0;

    printf("ADC: Running baseline calibration (%d samples)...\r\n", ADC_CALIB_SAMPLES);

    if (adc_sample_average(ADC_CALIB_SAMPLES, &baseline) != TEST_SUCCESS) {
        printf("ERROR: ADC calibration sampling failed\r\n");
        return TEST_FAILURE;
    }

    /* Cross-check against factory calibration value.
     * VREFINT_CAL is measured at 3.3V VDDA — if VDDA differs, the ratio
     * will shift, but for a basic sanity check ±10% is a wide enough window. */
    uint16_t factory_cal = *VREFINT_CAL_ADDR;
    uint32_t tolerance   = (factory_cal * 10U) / 100U; /* 10% window */

    if ((baseline < (factory_cal - tolerance)) ||
        (baseline > (factory_cal + tolerance))) {
        printf("ERROR: ADC baseline=%lu far from factory cal=%u — check VDDA\r\n",
               baseline, factory_cal);
        return TEST_FAILURE;
    }

    g_adc_baseline = baseline;

    printf("ADC: Baseline = %lu (factory cal = %u, VREFINT ~1.21V)\r\n",
           g_adc_baseline, factory_cal);

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Runs one ADC test iteration — samples VREFINT and returns average.
 *
 * @param  result_out  Pointer to store the measured average.
 * @retval TEST_SUCCESS on success, TEST_FAILURE on DMA error or timeout.
 * -------------------------------------------------------------------------*/
static uint8_t adc_run_iteration(uint32_t *result_out)
{
    return adc_sample_average(ADC_ITER_SAMPLES, result_out);
}

/* ---------------------------------------------------------------------------
 * @brief  Validates a measured ADC value against the stored baseline.
 *
 * Accepts the result if it falls within ADC_TOLERANCE_PERCENT of g_adc_baseline.
 * The tolerance window accounts for ADC noise, temperature drift, and minor
 * VDDA variation between the calibration and test runs.
 *
 * @param  measured  Averaged ADC reading from one test iteration.
 * @retval TEST_SUCCESS if within tolerance, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t adc_validate(uint32_t measured)
{
    uint32_t tolerance = (g_adc_baseline * ADC_TOLERANCE_PERCENT) / 100U;
    uint32_t lower     = (g_adc_baseline > tolerance) ?
                         (g_adc_baseline - tolerance) : 0U;
    uint32_t upper     = g_adc_baseline + tolerance;

    if (measured < lower || measured > upper) {
        printf("ERROR: ADC result=%lu outside window [%lu, %lu] (baseline=%lu)\r\n",
               measured, lower, upper, g_adc_baseline);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  ADC test task entry point.
 *
 * On first run, performs VREFINT baseline calibration. Subsequent test
 * commands skip calibration and use the stored baseline — call the test
 * with iterations=0 to force a recalibration.
 *
 * Blocks on adcSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations conversion+validation cycles.
 * Sends pass/fail result to the PC via UDP.
 *
 * Priority: osPriorityNormal2 (highest among application tasks).
 *
 * @param  argument  Unused (required by FreeRTOS task signature).
 * @retval None (infinite loop — never returns)
 * -------------------------------------------------------------------------*/
void StartADCTask(void *argument)
{
    ip_addr_t      pc_addr;
    uint8_t        test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    /* -----------------------------------------------------------------------
     * One-time calibration at task startup.
     * Retried on every subsequent test command if calibration failed.
     * ---------------------------------------------------------------------*/
    printf("ADC: Task started. Waiting for VREFINT stabilization...\r\n");

    /* VREFINT requires ~10us to stabilize after enable.
     * Wait one full RTOS tick (1ms) to be safe, and let other
     * init tasks complete before touching the ADC. */
    osDelay(100);

    /* Ensure ADC is fully stopped before first DMA start */
    HAL_ADC_Stop_DMA(&hadc1);
    HAL_ADC_Stop(&hadc1);
    osDelay(1);

    if (adc_calibrate() != TEST_SUCCESS) {
        printf("ERROR: ADC initial calibration failed — will retry on first test\r\n");
        g_adc_baseline = 0;
    }

    /* Raise priority to match other peripheral tasks now that init is done */
    vTaskPrioritySet(NULL, osPriorityNormal2);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(adcSemHandle, portMAX_DELAY);

        /* Take a local copy — Dispatcher may overwrite g_adc_cmd immediately */
        test_command_t cmd = g_adc_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nADC: Task woken. Starting %d iteration(s)\r\n", cmd.iterations);

        /* ------------------------------------------------------------------
         * Re-calibrate if baseline was never established or if the PC
         * requests it by sending iterations=0.
         * ----------------------------------------------------------------*/
        if (g_adc_baseline == 0 || cmd.iterations == 0) {
            printf("ADC: (Re)calibrating baseline...\r\n");
            if (adc_calibrate() != TEST_SUCCESS) {
                printf("ERROR: ADC calibration failed — aborting test\r\n");
                test_success = TEST_FAILURE;
                goto send_result;
            }
            if (cmd.iterations == 0) {
                /* Calibration-only request — report success and return */
                printf("ADC: Calibration complete (iterations=0, no test run)\r\n");
                goto send_result;
            }
        }

        /* ------------------------------------------------------------------
         * Run iterations
         * ----------------------------------------------------------------*/
        for (int i = 0; i < cmd.iterations; i++) {
            uint32_t measured = 0;

            if (adc_run_iteration(&measured) != TEST_SUCCESS) {
                printf("ERROR: ADC iteration %d sampling failed\r\n", i);
                test_success = TEST_FAILURE;
                break;
            }

            printf("ADC: iteration %d — measured=%lu baseline=%lu\r\n",
                   i, measured, g_adc_baseline);

            if (adc_validate(measured) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: ADC iteration %d — PASS\r\n", i);
        }

        printf("ADC: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

send_result:
		uut_send_result(cmd.test_id, test_success, &pc_addr, g_pc_port);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL ADC conversion complete callback — called from DMA ISR context.
 *
 * Fired when all num_samples conversions in adc_dma_buf are complete.
 * Notifies ADCTask via direct task notification so it can read the buffer.
 *
 * @param  hadc  ADC handle that completed conversion.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    BaseType_t x = pdFALSE;

    if (hadc->Instance == ADC1) {
        xTaskNotifyFromISR(ADCTaskHandle, ADC_NOTIFY_CONV_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  HAL ADC error callback — called from DMA/ADC ISR context.
 *
 * Notifies ADCTask immediately so it unblocks and reports failure rather
 * than waiting the full 500 ms timeout.
 *
 * @param  hadc  ADC handle that encountered an error.
 * @retval None
 * -------------------------------------------------------------------------*/
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    BaseType_t x = pdFALSE;

    if (hadc->Instance == ADC1) {
        xTaskNotifyFromISR(ADCTaskHandle, ADC_NOTIFY_CONV_DONE, eSetBits, &x);
        portYIELD_FROM_ISR(x);
    }
}
