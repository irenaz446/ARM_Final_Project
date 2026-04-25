#include "main.h"

extern UART_HandleTypeDef huart3;

#define UART_DEBUG &huart3

// printf
int __io_putchar(int ch)
{
	HAL_UART_Transmit(UART_DEBUG, (uint8_t *)&ch, 1, 0xFFFF);
	return ch;
}

int _write(int file, char *ptr, int len)
{
	HAL_UART_Transmit(UART_DEBUG, (uint8_t *)ptr, len, 0xFFFF);
	return len;
}

// scanf
// scanf
int _read(int file, char *ptr, int len)
{
	int ch=0;
	HAL_UART_Receive(UART_DEBUG, (uint8_t*)&ch, 1, HAL_MAX_DELAY);
	HAL_UART_Transmit(UART_DEBUG, (uint8_t *)&ch, 1,HAL_MAX_DELAY);
	if(ch==13)
	{
		ch=10;
		HAL_UART_Transmit(UART_DEBUG, (uint8_t *)&ch, 1,HAL_MAX_DELAY);
	}
	else if(ch==8)
	{
		ch=0x30;
		HAL_UART_Transmit(UART_DEBUG, (uint8_t *)&ch, 1,HAL_MAX_DELAY);
	}

	*ptr=ch;

	return 1;
}
