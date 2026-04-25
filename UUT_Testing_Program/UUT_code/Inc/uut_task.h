#ifndef INC_UUT_TASK_H_
#define INC_UUT_TASK_H_

#include "lwip/udp.h"
#include "lwip/tcpip.h"

/* ---------------------------------------------------------------------------
 * Private type: UDP send request
 * Allocated on heap, passed to tcpip_callback, freed inside the callback.
 * -------------------------------------------------------------------------*/
typedef struct {
    test_result_t res;  /* Test result to send to PC   */
    ip_addr_t     addr; /* Destination IP (PC address) */
    u16_t         port; /* Destination port (PC source port) */
} udp_send_req_t;



#endif /* INC_UUT_TASK_H_ */
