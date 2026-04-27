/**
 * @file    timer_task.c
 * @brief   Timer loopback test task for the UUT (Unit Under Test).
 *
 * Tests TIM2 (APB1, 32-bit general purpose) accuracy using TIM1 (APB2,
 * 16-bit advanced) as an independent reference clock on a different bus.
 *
 * Test design:
 *   - TIM1 is configured as a free-running microsecond reference counter.
 *     Prescaler = (APB2_CLK / 1_000_000) - 1, Period = 0xFFFF (max 16-bit).
 *     It runs continuously — we snapshot its counter before and after TIM2
 *     fires to measure actual elapsed time.
 *
 *   - TIM2 is the Device Under Test (DUT). It is configured to generate an
 *     update event (overflow) after exactly TIMER_TEST_PERIOD_MS milliseconds.
 *     Prescaler = (APB1_CLK / 10_000) - 1 → 0.1ms per tick.
 *     Period = (TIMER_TEST_PERIOD_MS * 10) - 1 → overflow at target time.
 *
 *   Per iteration:
 *     1. Reset and start both timers.
 *     2. Record TIM1 counter snapshot (t_start).
 *     3. Block on task notification until TIM2 update ISR fires.
 *     4. Record TIM1 counter snapshot (t_end).
 *     5. Compute elapsed_us = (t_end - t_start) with 16-bit wraparound handling.
 *     6. Validate elapsed_us is within TIMER_TOLERANCE_PERCENT of expected.
 *
 * Hardware:  STM32F756ZG
 * TIM1:      APB2 bus, 16-bit advanced timer — reference
 * TIM2:      APB1 bus, 32-bit general purpose timer — device under test
 *
 * CubeMX configuration required:
 *   TIM1: Clock Source = Internal Clock, Counter Period = 65535,
 *         Prescaler = (APB2_TIMER_CLK / 1000000) - 1  (1 MHz → 1us/tick)
 *         auto-reload preload = Enable, no output, no interrupt needed
 *
 *   TIM2: Clock Source = Internal Clock,
 *         Prescaler = (APB1_TIMER_CLK / 10000) - 1    (10 kHz → 0.1ms/tick)
 *         Counter Period = (TIMER_TEST_PERIOD_MS * 10) - 1
 *         auto-reload preload = Enable
 *         NVIC: TIM2 global interrupt — Enable
 *
 * @note   APB1 timer clock on STM32F756 = 108 MHz (x2 multiplier applied).
 *         APB2 timer clock on STM32F756 = 216 MHz.
 *         Verify your actual clock tree in CubeMX before setting prescalers.
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
 * External HAL peripheral handles (defined by CubeMX in tim.c)
 * -------------------------------------------------------------------------*/
extern TIM_HandleTypeDef htim1; /* APB2 — reference counter  */
extern TIM_HandleTypeDef htim2; /* APB1 — device under test  */

/* ---------------------------------------------------------------------------
 * Shared inter-task variables (defined in freertos.c, owned by Dispatcher)
 * -------------------------------------------------------------------------*/
extern TaskHandle_t      TimerTaskHandle; /* Used by ISR to notify this task */
extern SemaphoreHandle_t timerSemHandle;  /* Dispatcher -> TimerTask trigger  */
extern test_command_t    g_timer_cmd;     /* Command from Dispatcher          */
extern uint16_t          g_pc_port;       /* PC source port for UDP reply     */

/* ---------------------------------------------------------------------------
 * Test parameters
 * -------------------------------------------------------------------------*/

/* TIM2 test period in milliseconds — how long TIM2 runs before overflow.
 * Keep short enough that TIM1 (16-bit, 1us/tick = 65.535ms max) doesn't
 * wrap more than once. 50ms is safe. */
#define TIMER_TEST_PERIOD_MS      50U

/* Expected elapsed time in microseconds */
#define TIMER_EXPECTED_US         (TIMER_TEST_PERIOD_MS * 1000U)

/* Acceptance window: ±5% of expected period.
 * Accounts for timer startup latency and ISR entry overhead. */
#define TIMER_TOLERANCE_PERCENT   5U

/* Task notification bit */
#define TIMER_NOTIFY_TIM2_DONE    (1UL << 0)

/* ---------------------------------------------------------------------------
 * Private function prototypes
 * -------------------------------------------------------------------------*/
static uint8_t run_timer_iteration(uint32_t *elapsed_us_out);
static uint8_t validate_timer(uint32_t elapsed_us);

/* ---------------------------------------------------------------------------
 * @brief  Runs one timer iteration.
 *
 * Resets TIM1 and TIM2, starts both, records start time, waits for TIM2
 * update interrupt, records end time, computes elapsed microseconds.
 *
 * TIM1 is 16-bit so we handle wraparound explicitly. At 1us/tick, TIM1
 * wraps at 65.535ms. Since TIMER_TEST_PERIOD_MS = 50ms < 65.535ms,
 * wraparound is handled by checking if t_end < t_start.
 *
 * @param  elapsed_us_out  Pointer to store measured elapsed time in us.
 * @retval TEST_SUCCESS on success, TEST_FAILURE on timeout.
 * -------------------------------------------------------------------------*/
static uint8_t run_timer_iteration(uint32_t *elapsed_us_out)
{
    uint32_t notif   = 0;
    uint16_t t_start = 0;
    uint16_t t_end   = 0;

    /* Clear stale notifications */
    xTaskNotifyWait(0xFFFFFFFF, 0xFFFFFFFF, &notif, 0);

    /* Reset and start TIM1 reference counter from 0 */
    __HAL_TIM_SET_COUNTER(&htim1, 0);
    HAL_TIM_Base_Start(&htim1);

    /* Record start time immediately after TIM1 starts */
    t_start = (uint16_t)__HAL_TIM_GET_COUNTER(&htim1);

    /* Reset TIM2 DUT counter and start — interrupt fires on overflow */
    __HAL_TIM_SET_COUNTER(&htim2, 0);
    HAL_TIM_Base_Start_IT(&htim2);

    /* Wait for TIM2 update interrupt notification (2x expected period timeout) */
    BaseType_t notified = xTaskNotifyWait(0,
                                          0xFFFFFFFF,
                                          &notif,
                                          pdMS_TO_TICKS(TIMER_TEST_PERIOD_MS * 2));

    /* Record end time immediately after notification */
    t_end = (uint16_t)__HAL_TIM_GET_COUNTER(&htim1);

    /* Stop both timers */
    HAL_TIM_Base_Stop_IT(&htim2);
    HAL_TIM_Base_Stop(&htim1);

    if ((notified == pdFALSE) || !(notif & TIMER_NOTIFY_TIM2_DONE)) {
        printf("ERROR: Timer timeout — TIM2 update interrupt never fired\r\n");
        return TEST_FAILURE;
    }

    /* Compute elapsed time with 16-bit wraparound handling.
     * If t_end < t_start, TIM1 wrapped once during the measurement. */
    if (t_end >= t_start) {
        *elapsed_us_out = (uint32_t)(t_end - t_start);
    } else {
        *elapsed_us_out = (uint32_t)(0xFFFFU - t_start + t_end + 1U);
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Validates measured elapsed time against expected period.
 *
 * @param  elapsed_us  Measured time in microseconds.
 * @retval TEST_SUCCESS if within tolerance, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
static uint8_t validate_timer(uint32_t elapsed_us)
{
    uint32_t tolerance_us = (TIMER_EXPECTED_US * TIMER_TOLERANCE_PERCENT) / 100U;
    uint32_t lower        = TIMER_EXPECTED_US - tolerance_us;
    uint32_t upper        = TIMER_EXPECTED_US + tolerance_us;

    if (elapsed_us < lower || elapsed_us > upper) {
        printf("ERROR: Timer period out of range: measured=%lu us "
               "expected=%u us window=[%lu, %lu]\r\n",
               elapsed_us, TIMER_EXPECTED_US, lower, upper);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * @brief  Timer test task entry point.
 *
 * Blocks on timerSemHandle until the Dispatcher wakes it with a command.
 * Runs cmd.iterations timer measurement cycles.
 * Sends pass/fail result to the PC via UDP.
 *
 * Priority: osPriorityNormal2.
 *
 * @param  argument  Unused.
 * @retval None (infinite loop)
 * -------------------------------------------------------------------------*/
void StartTimerTask(void *argument)
{
    ip_addr_t      pc_addr;
    uint8_t        test_success;

    ipaddr_aton(PC_IP, &pc_addr);

    for (;;) {
        /* Wait for Dispatcher to signal a new command */
        xSemaphoreTake(timerSemHandle, portMAX_DELAY);

        /* Local copy — Dispatcher may overwrite g_timer_cmd immediately */
        test_command_t cmd = g_timer_cmd;
        test_success = TEST_SUCCESS;

        printf("\r\nTIMER: Task woken. Starting %d iteration(s)\r\n",
               cmd.iterations);
        printf("TIMER: DUT=TIM2 (APB1), Reference=TIM1 (APB2), "
               "Target period=%dms, Tolerance=%d%%\r\n",
               TIMER_TEST_PERIOD_MS, TIMER_TOLERANCE_PERCENT);

        for (int i = 0; i < cmd.iterations; i++) {
            uint32_t elapsed_us = 0;

            if (run_timer_iteration(&elapsed_us) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("TIMER: iteration %d — elapsed=%luus (expected=%dus)\r\n",
                   i, elapsed_us, TIMER_EXPECTED_US);

            if (validate_timer(elapsed_us) != TEST_SUCCESS) {
                test_success = TEST_FAILURE;
                break;
            }

            printf("DEBUG: TIMER iteration %d — PASS\r\n", i);
        }

        printf("TIMER: Test finished. Result: %s\r\n",
               (test_success == TEST_SUCCESS) ? "PASS" : "FAIL");

        /* Send result to PC via lwIP core task (thread-safe) */
        uut_send_result(cmd.test_id, test_success, &pc_addr, g_pc_port);
    }
}
