################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../UUT_code/Src/adc_task.c \
../UUT_code/Src/i2c_task.c \
../UUT_code/Src/io_tools.c \
../UUT_code/Src/spi_task.c \
../UUT_code/Src/timer_task.c \
../UUT_code/Src/uart_task.c \
../UUT_code/Src/uut_task.c 

OBJS += \
./UUT_code/Src/adc_task.o \
./UUT_code/Src/i2c_task.o \
./UUT_code/Src/io_tools.o \
./UUT_code/Src/spi_task.o \
./UUT_code/Src/timer_task.o \
./UUT_code/Src/uart_task.o \
./UUT_code/Src/uut_task.o 

C_DEPS += \
./UUT_code/Src/adc_task.d \
./UUT_code/Src/i2c_task.d \
./UUT_code/Src/io_tools.d \
./UUT_code/Src/spi_task.d \
./UUT_code/Src/timer_task.d \
./UUT_code/Src/uart_task.d \
./UUT_code/Src/uut_task.d 


# Each subdirectory must supply rules for building sources it contributes
UUT_code/Src/%.o UUT_code/Src/%.su UUT_code/Src/%.cyclo: ../UUT_code/Src/%.c UUT_code/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F756xx -c -I../Core/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc -I../Drivers/STM32F7xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F7xx/Include -I../Drivers/CMSIS/Include -I../LWIP/App -I../LWIP/Target -I../Middlewares/Third_Party/LwIP/src/include -I../Middlewares/Third_Party/LwIP/system -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1 -I../Drivers/BSP/Components/lan8742 -I../Middlewares/Third_Party/LwIP/src/include/netif/ppp -I../Middlewares/Third_Party/LwIP/src/include/lwip -I../Middlewares/Third_Party/LwIP/src/include/lwip/apps -I../Middlewares/Third_Party/LwIP/src/include/lwip/priv -I../Middlewares/Third_Party/LwIP/src/include/lwip/prot -I../Middlewares/Third_Party/LwIP/src/include/netif -I../Middlewares/Third_Party/LwIP/src/include/compat/posix -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/arpa -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/net -I../Middlewares/Third_Party/LwIP/src/include/compat/posix/sys -I../Middlewares/Third_Party/LwIP/src/include/compat/stdc -I../Middlewares/Third_Party/LwIP/system/arch -I"C:/Users/iraze/STM32CubeIDE/workspace_1.19.0/UUT_Testing_Program/UUT_code/Inc" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-UUT_code-2f-Src

clean-UUT_code-2f-Src:
	-$(RM) ./UUT_code/Src/adc_task.cyclo ./UUT_code/Src/adc_task.d ./UUT_code/Src/adc_task.o ./UUT_code/Src/adc_task.su ./UUT_code/Src/i2c_task.cyclo ./UUT_code/Src/i2c_task.d ./UUT_code/Src/i2c_task.o ./UUT_code/Src/i2c_task.su ./UUT_code/Src/io_tools.cyclo ./UUT_code/Src/io_tools.d ./UUT_code/Src/io_tools.o ./UUT_code/Src/io_tools.su ./UUT_code/Src/spi_task.cyclo ./UUT_code/Src/spi_task.d ./UUT_code/Src/spi_task.o ./UUT_code/Src/spi_task.su ./UUT_code/Src/timer_task.cyclo ./UUT_code/Src/timer_task.d ./UUT_code/Src/timer_task.o ./UUT_code/Src/timer_task.su ./UUT_code/Src/uart_task.cyclo ./UUT_code/Src/uart_task.d ./UUT_code/Src/uart_task.o ./UUT_code/Src/uart_task.su ./UUT_code/Src/uut_task.cyclo ./UUT_code/Src/uut_task.d ./UUT_code/Src/uut_task.o ./UUT_code/Src/uut_task.su

.PHONY: clean-UUT_code-2f-Src

