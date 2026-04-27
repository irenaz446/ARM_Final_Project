/**
 * @file    uut_task.h
 * @brief   Shared types, macros and API for all UUT peripheral test tasks.
 *
 * This header is included by every peripheral task file (uart_task.c,
 * spi_task.c, i2c_task.c, adc_task.c, timer_task.c) and provides:
 *
 *   - udp_send_req_t  : heap-allocated UDP send request passed to tcpip_callback
 *   - uut_send_result : thread-safe UDP result sender (call from any task)
 *   - uut_validate_crc: hardware CRC32 comparison helper for long patterns
 */

#ifndef INC_UUT_TASK_H_
#define INC_UUT_TASK_H_

#include "pc_test_uut.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "FreeRTOS.h"

/* ---------------------------------------------------------------------------
 * UDP send request
 * Allocated on the FreeRTOS heap by uut_send_result(), passed to
 * tcpip_callback(), and freed inside the lwIP core callback.
 * -------------------------------------------------------------------------*/
typedef struct {
    test_result_t res;  /* Test result to send to PC         */
    ip_addr_t     addr; /* Destination IP address (PC)       */
    u16_t         port; /* Destination port (PC source port) */
} udp_send_req_t;

/* ---------------------------------------------------------------------------
 * @brief  Sends a test result to the PC via the lwIP core task.
 *
 * Safe to call from any FreeRTOS task. Allocates a udp_send_req_t on the
 * heap, populates it, and schedules send_result_callback to run inside
 * defaultTask (the lwIP owner) via tcpip_callback().
 *
 * The caller must NOT free the request — it is freed inside the callback.
 *
 * @param  test_id   Test identifier echoed back to the PC.
 * @param  result    TEST_SUCCESS or TEST_FAILURE.
 * @param  pc_addr   Destination IP address (typically parsed from PC_IP).
 * @param  pc_port   Destination port (g_pc_port saved from the UDP packet).
 * @retval None
 * -------------------------------------------------------------------------*/
void uut_send_result(uint32_t test_id, uint8_t result,
                     const ip_addr_t *pc_addr, uint16_t pc_port);

/* ---------------------------------------------------------------------------
 * @brief  Validates two aligned buffers using hardware CRC32.
 *
 * Used by tasks that handle patterns longer than 100 bytes. Both buffers
 * must be 4-byte aligned (use __attribute__((aligned(4)))).
 *
 * @param  sent      Pointer to the transmitted data buffer (word-aligned).
 * @param  received  Pointer to the received data buffer (word-aligned).
 * @param  len       Number of bytes to validate. Only complete 32-bit words
 *                   are checked (len / 4 words).
 * @retval TEST_SUCCESS if CRC matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
uint8_t uut_validate_crc(const uint32_t *sent, const uint32_t *received,
                          uint8_t len);

#endif /* INC_UUT_TASK_H_ */
