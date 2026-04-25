#ifndef PC_TEST_UUT_H
#define PC_TEST_UUT_H

#include <stdint.h>

#define UUT_IP	"192.168.10.2"
#define PC_IP  	"192.168.10.3"
#define UUT_PORT 5005
#define TIMEOUT_SEC 10
#define	TEST_SUCCESS 	0x01
#define	TEST_FAILURE 	0x00

#define TIMER_TEST   1
#define UART_TEST    2
#define SPI_TEST     4
#define I2C_TEST     8
#define ADC_TEST     16

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