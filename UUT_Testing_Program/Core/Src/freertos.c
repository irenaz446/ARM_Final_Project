/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "pc_test_uut.h"
#include "queue.h"
#include "semphr.h"
#include "usart.h"
#include "lwip/udp.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define QUEUE_COUNT     5
#define QUEUE_ITEM_SIZE sizeof(test_command_t)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
TaskHandle_t      UARTTaskHandle;
TaskHandle_t      SPITaskHandle;
TaskHandle_t      I2CTaskHandle;
TaskHandle_t      ADCTaskHandle;
TaskHandle_t      TimerTaskHandle;
TaskHandle_t      DispatcherTaskHandle;
QueueHandle_t     periphQueueHandle;
SemaphoreHandle_t uartSemHandle;
SemaphoreHandle_t spiSemHandle; 
SemaphoreHandle_t i2cSemHandle;
SemaphoreHandle_t adcSemHandle;
SemaphoreHandle_t timerSemHandle;

/* Shared: Dispatcher writes, peripheral task reads */
test_command_t    g_uart_cmd;   
test_command_t    g_spi_cmd;
test_command_t 	  g_i2c_cmd;
test_command_t    g_adc_cmd;
test_command_t    g_timer_cmd;

uint16_t g_pc_port;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

void StartDispatcherTask(void *argument);
void StartUARTTask(void *argument);
void StartSPITask(void *argument);
void StartI2CTask(void *argument);
void StartADCTask(void *argument);
void StartTimerTask(void *argument);
void udp_listener_init(void);
void udp_receive_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
	/* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
	/* add semaphores, ... */
	uartSemHandle = xSemaphoreCreateBinary();
	spiSemHandle  = xSemaphoreCreateBinary();
	i2cSemHandle  = xSemaphoreCreateBinary();
	adcSemHandle = xSemaphoreCreateBinary();
	timerSemHandle = xSemaphoreCreateBinary();
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
	/* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
	/* add queues, ... */
	periphQueueHandle = xQueueCreate(QUEUE_COUNT, QUEUE_ITEM_SIZE);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
	/* add threads, ... */
	xTaskCreate(StartDispatcherTask, "Dispatcher",
				configMINIMAL_STACK_SIZE * 2,
				NULL, osPriorityNormal1,   /* Priority 25 */
				&DispatcherTaskHandle);

	/* UARTTask — runs hardware test, highest priority */
	xTaskCreate(StartUARTTask,
			    "UARTTask",
				configMINIMAL_STACK_SIZE * 8,
				NULL, osPriorityNormal2,   /* Priority 26 */
				&UARTTaskHandle);
    xTaskCreate(StartSPITask,
                "SPITask",
                configMINIMAL_STACK_SIZE * 8,
                NULL,
                osPriorityNormal2,   /* Priority 26 */
                &SPITaskHandle);
    xTaskCreate(StartI2CTask,
                "I2CTask",
                configMINIMAL_STACK_SIZE * 8,
                NULL,
                osPriorityNormal2,   /* Priority 26 */
                &I2CTaskHandle);
	xTaskCreate(StartADCTask,
				"ADCTask",
				configMINIMAL_STACK_SIZE * 4,
	            NULL, osPriorityNormal1,
				&ADCTaskHandle);
	xTaskCreate(StartTimerTask,
				"TimerTask",
				configMINIMAL_STACK_SIZE * 4,
	            NULL, osPriorityNormal2,
				&TimerTaskHandle);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
	/* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
	/* Initialize UDP Listener */
	udp_listener_init();
	/* Infinite loop */
    for (;;) {
        osDelay(1);
    }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/**
 * @brief  DispatcherTask — routes commands from the queue to peripheral tasks.
 *
 * Reads test_command_t items from periphQueueHandle and wakes the appropriate
 * peripheral task via its semaphore.
 *
 * @param  argument  Unused.
 * @retval None 
 * -------------------------------------------------------------------------*/
void StartDispatcherTask(void *argument) {
    test_command_t cmd;

    for (;;) {
        if (xQueueReceive(periphQueueHandle, &cmd, portMAX_DELAY) == pdPASS) {
            switch (cmd.peripheral_id) {
				case TIMER_TEST:
					g_timer_cmd = cmd;
					xSemaphoreGive(timerSemHandle);
					break;

                case UART_TEST: /* UART */
                    g_uart_cmd = cmd;
                    xSemaphoreGive(uartSemHandle);
                    break;

                case SPI_TEST:
                    g_spi_cmd = cmd;
                    xSemaphoreGive(spiSemHandle);
                    break;

                case I2C_TEST:
					g_i2c_cmd = cmd;
					xSemaphoreGive(i2cSemHandle);
					break;

                case ADC_TEST:
                    g_adc_cmd = cmd;
                    xSemaphoreGive(adcSemHandle);
                    break;

                default:
                    printf("Dispatcher: unknown peripheral ID %d\r\n", cmd.peripheral_id);
                    break;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * @brief  Initializes the lwIP UDP listener on UUT_PORT.
 *
 * Must be called from defaultTask only (lwIP context).
 * Registers udp_receive_callback to handle all incoming packets.
 *
 * @retval None
 * -------------------------------------------------------------------------*/
void udp_listener_init(void)
{
    struct udp_pcb *upcb = udp_new();

    printf("UUT: Setting up UDP listener on port %d...\r\n", UUT_PORT);

    if (upcb == NULL) {
        printf("ERROR: Could not allocate UDP PCB\r\n");
        return;
    }

    err_t err = udp_bind(upcb, IP_ADDR_ANY, UUT_PORT);
    if (err != ERR_OK) {
        printf("ERROR: UDP bind failed (err=%d)\r\n", err);
        return;
    }

    udp_recv(upcb, udp_receive_callback, NULL);
    printf("UUT: UDP listener ready\r\n");
}

/* ---------------------------------------------------------------------------
 * @brief  UDP receive callback — called by lwIP when a packet arrives.
 *
 * Copies the packet payload into a local test_command_t struct, saves the
 * sender's source port (needed to reply), frees the pbuf, then pushes the
 * command into periphQueueHandle for the Dispatcher.
 *
 * @param  arg   Unused user argument.
 * @param  upcb  UDP PCB that received the packet (unused).
 * @param  p     Received pbuf chain (must be freed before returning).
 * @param  addr  Source IP address of the sender.
 * @param  port  Source port of the sender — saved to g_pc_port.
 * @retval None
 * -------------------------------------------------------------------------*/
void udp_receive_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                          const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)upcb;
    (void)addr;

    if (p == NULL) {
        return;
    }

    test_command_t *rcv_packet = (test_command_t *)p->payload;

    printf("\r\nNetwork: packet received (Test ID=%lu, Periph=%d)\r\n",
    		rcv_packet->test_id, rcv_packet->peripheral_id);

    g_pc_port = port;

    test_command_t cmd_copy;
    memcpy(&cmd_copy, rcv_packet, sizeof(test_command_t));
    pbuf_free(p);

    /* Push to Dispatcher queue */
    if (xQueueSendToBack(periphQueueHandle, &cmd_copy, 0) != pdPASS) {
        printf("WARNING: Peripheral queue full! \r\n");
    }
}
/* USER CODE END Application */

