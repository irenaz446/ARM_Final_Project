/**
 * @file    uut_task.c
 * @brief   Shared utilities for all UUT peripheral test tasks.
 *
 * Implements the common functionality used by every peripheral task:
 *
 *   uut_send_result  — thread-safe UDP result sender via tcpip_callback.
 *   uut_validate_crc — hardware CRC32 comparison for long patterns.
 *
 * Both functions were previously copy-pasted into every task file
 * (uart_task.c, spi_task.c, i2c_task.c, adc_task.c, timer_task.c).
 * Centralising them here eliminates the duplication and ensures any
 * fix or improvement applies to all tests at once.
 */

#include "uut_task.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * External CRC handle (defined by CubeMX in crc.c)
 * -------------------------------------------------------------------------*/
extern CRC_HandleTypeDef hcrc;

/* ---------------------------------------------------------------------------
 * Private: lwIP core callback that executes the actual UDP send.
 *
 * Called by tcpip_callback() inside defaultTask (the lwIP owner).
 * All lwIP API calls here are safe because we are in the lwIP task context.
 * Frees the udp_send_req_t allocated by uut_send_result() before returning.
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
 * @brief  Sends a test result to the PC via the lwIP core task.
 *
 * Allocates a udp_send_req_t on the FreeRTOS heap, populates it with the
 * result and destination, then delegates the actual UDP send to
 * send_result_callback via tcpip_callback(). This is the correct pattern
 * for calling lwIP from any task other than defaultTask.
 *
 * @param  test_id   Test identifier echoed back to the PC.
 * @param  result    TEST_SUCCESS or TEST_FAILURE.
 * @param  pc_addr   Destination IP address.
 * @param  pc_port   Destination port (g_pc_port from the incoming packet).
 * @retval None
 * -------------------------------------------------------------------------*/
void uut_send_result(uint32_t test_id, uint8_t result,
                     const ip_addr_t *pc_addr, uint16_t pc_port)
{
    udp_send_req_t *req = pvPortMalloc(sizeof(udp_send_req_t));

    if (req != NULL) {
        req->res.test_id = test_id;
        req->res.result  = result;
        req->addr        = *pc_addr;
        req->port        = pc_port;
        tcpip_callback(send_result_callback, req);
    } else {
        printf("ERROR: uut_send_result — heap allocation failed\r\n");
        vPortFree(req);
    }
}

/* ---------------------------------------------------------------------------
 * @brief  Validates two aligned buffers using hardware CRC32.
 *
 * HAL_CRC_Calculate requires word-aligned input. Both pointers must point
 * to buffers declared with __attribute__((aligned(4))). Only complete
 * 32-bit words are validated (len / 4); any trailing bytes are ignored.
 *
 * @param  sent      Word-aligned pointer to transmitted data.
 * @param  received  Word-aligned pointer to received data.
 * @param  len       Total byte count (only len/4 words are checked).
 * @retval TEST_SUCCESS if CRC matches, TEST_FAILURE otherwise.
 * -------------------------------------------------------------------------*/
uint8_t uut_validate_crc(const uint32_t *sent, const uint32_t *received,
                          uint8_t len)
{
    /* HAL_CRC_Calculate takes a non-const pointer — cast is safe because
     * the CRC peripheral only reads the data, never writes it. */
    uint32_t c_sent = HAL_CRC_Calculate(&hcrc, (uint32_t *)sent,     (uint32_t)len);
    uint32_t c_recv = HAL_CRC_Calculate(&hcrc, (uint32_t *)received, (uint32_t)len);

    if (c_sent != c_recv) {
        printf("ERROR: CRC mismatch: sent=0x%08lX recv=0x%08lX\r\n",
               c_sent, c_recv);
        return TEST_FAILURE;
    }

    return TEST_SUCCESS;
}
