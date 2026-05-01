#ifndef PC_TEST_UUT_H
#define PC_TEST_UUT_H

#include <stdint.h>

#define UUT_IP	"192.168.10.2"      /* IP Address of UUT device */  
#define PC_IP  	"192.168.10.3"      /* IP Address of the Linux test PC */ 
#define UUT_PORT 5005               /* UDP port the UUT listens on */
#define TIMEOUT_SEC 10              /* PC-side receive timeout in seconds */
#define	TEST_SUCCESS 	0x01        /* Peripheral test passed all iterations */
#define	TEST_FAILURE 	0x00        /* Peripheral test failed at least once  */
#define MAX_PATTERN_LEN    255      /* Maximum number of bytes in a test pattern */

#define TIMER_TEST   1      /* TIM2 accuracy test (APB1 vs APB2 reference)   */
#define UART_TEST    2      /* UART4 <-> UART5 full-duplex loopback          */
#define SPI_TEST     4      /* SPI4 (Master) <-> SPI1 (Slave) loopback       */
#define I2C_TEST     8      /* I2C4 (Master) <-> I2C2 (Slave) loopback       */
#define ADC_TEST     16     /* ADC1 VREFINT accuracy test (internal reference)  */

/**
 * @brief Command structure sent to UUT.
 */
typedef struct __attribute__((packed)) {
    uint32_t test_id;
    uint8_t  peripheral_id;
    uint8_t  iterations;
    uint8_t  pattern_len;
    uint8_t  pattern[255];
} test_command_t;

/**
 * @brief Result structure received from UUT.
 */
typedef struct __attribute__((packed)) {
    uint32_t test_id;
    uint8_t  result;
} test_result_t;

#endif /* PC_TEST_UUT_H */