Final Project for SE Embedded Systems 
Introduction 
A manufacturing company is interested in testing the peripherals on the STM32F756ZG card in order to ensure hardware correctness. The company is asking you to take part in card’s hardware verification for the following peripherals:  
TIMER,  UART,  SPI,  I2C,  ADC  Eth (MAC & PHY)
The verification resembles a client server program, the testing program (server) running on the P.C, sends commands using a proprietary protocol over UDP (IP/Ethernet) to the UUT. Once the UUT Testing program receives the proprietary protocol, finds out the peripheral to be tested and starts running the test. 

Testing Procedure 
1. The Testing program running on the P.C. (the Server) communicates with UUT (the client) through the UDP/IP communication protocol. 
2. The server will send the commands (Perform the unit testing) on the UUT (the Client), and wait for the response from the UUT. 
3. The UUT Ethernet device will receive the command and using the LWIP stack pass it to the "UUT Testing Program" 
4. The test at the UUT is done by acquiring the needed parameters from the incoming command, such as: 
Which peripheral is about to be tested,
what is the string of characters sent to the peripheral. 
Test Iteration (number of times test has to be run at the UUT). 
5. Once the UUT Testing has all the data needed for the test it will run the test for the number of defined iterations, each test will result in success or failure, each peripheral will have its own set of tests (elaborated later on). 
6. The UUT test result per peripheral will be sent to the P.C. Testing program. 

Proprietary Protocols 
1. The command sent from the P.C. Testing Program to the UUT will contain the following: 
Test-ID – 4 Bytes (a number given to the test so it will be easy to map it to the later on test result). 
Peripheral to be tested – 1 Byte (a bitfield for the peripheral being tested (only one at a time!): 1 –Timer, 2 – UART, SPI – 4, I2C – 8, ADC – 16). 
Iterations – 1 Byte (the number of iterations the test should run at the UUT (Unit Under Test) ). 
Bit pattern length – 1 Byte (the size of the Bit pattern string sent to UUT). 
Bit pattern – Bit pattern length (the actual string of characters sent to the UUT). 
2. The result protocol sent from the UUT back to the P.C. Testing Program will contain the following: 
Test-ID – 4 Bytes (a number given to the test so it'll be easy to map it to the later on test result). 
Test Result – 1 Byte (bitfield: 1 – test succeeded, 0xff –test failed). 

P.C. Testing Program (Server Side) 
1. Implementing the Testing Program using CLI (Command Line Interface) is good enough.
2. Implementation should be done using C \C++ on a Linux machine. 
3. P.C. Testing Program should have persistent (saved on file system) testing records per Test-ID, and be ready to print on demand. including: 
TEST-ID. 
Date and time test has been sent to the UUT. 
Test length measured in seconds (the amount of time it took to send the test command until receiving the result). 
Result – Success or Failure. 
4. All Test results should be kept in the local database (sqlite or .csv file)

UUT Testing Program (Client Side) 
1. Once the UUT Testing Program receives the test command, it will acquire the needed parameters and initiate the test on the required peripheral. 
2. Tests will vary between peripherals; the following includes Unit Testing for every Peripheral: 
 UART, I2C, SPI:  
The below procedure is described for UART testing but it stands for I2C and SPI as well.  1. Peripheral testing is required to be done using DMA mode if possible. 
2. Each Peripheral testing will require peripheral parameters, please choose (assume) parameters needed at your convenience (i.e. for UART you can assume BAUD Rate 115200, 8bit Data, 1 Stop Bit, No parity). 
3.  For the amount of needed iteration, the Testing Program will send the received Bit Pattern to the UART4, which in turn will pass the data to the UART5 port on the UUT.  
4. UART5 will send back the received string to UART4 (predefined program waiting for incoming data).  
5. For every iteration, the UUT Testing program will receive the incoming data from UART4 and compare it to sent data ( CRC Compare should be use for large bulks of data – above 100 bytes).  
6. If the testing has been successful for all iterations; a success result should be sent to the Testing P.C. Program.  
7. If at any time during the iterations a test has failed, testing should be stopped and a Failure result should be sent to the P.C. Testing Program. 
 ADC:  
Use ADC required parameters at your own convenience (i.e. 12 bit ADC).  
2. Running the test beforehand, we should already have the bitstream for the analog to digital conversion at the current voltage. 
 For each iteration Run the conversion and compare the ADC result with already known result.  
Send the P.C. Testing Program the final test result (after all the iterations). 
Timer  
This test is left for you to decide on design and implementation, think of the best way to measure time \ count pulses  
Assume Timer required parameters at your own convenience. 

The following requirements are mandatory: 
1. Functionality: Code must be compliable, executable and achieve its goals. 
2. Deep understanding of the design: The full understanding of the project requirements and the created solution. Explaining any technical decisions. 
3. Clean functions and classes: Code must be compact. Each element achieves a specific purpose. Try to avoid many arguments, meaningless names and use of the flag arguments. 
4. Comments: Explain yourself in code, don’t be redundant, and don’t comment out code - just remove it. Doxygen type is required.
5. No "Code smell": Avoid needless complexity or repetitions. The code must be simple and understandable. 
6. Tests: The tests that were done already (also manual ones are acceptable for this project) must be built as independent, fast, and repeatable units. Pay attention to the unexpected behavior and edge cases coverage. 

Code Convention

1. Coding Style & Readability:
Consistent Naming:  
Use snake_case for variables/functions (e.g., calculate_sum()).  
Prefix globals with g_ (e.g., g_config).  
Use uppercase for macros (e.g., #define MAX_SIZE 100). 
Indentation & Formatting:  
Use 4-space indentation (no tabs). 
 Always brace {} blocks, even for single-line statements.
 Comments: 
 Explain why, not what (self-documenting code > comments).  
Use // for single-line, /* */ for multi-line. 
 Document function purpose, parameters, and return values. 

2. Design Principles
Modularity:  
Split code into .h (declarations) and .c (implementations). 
 Limit file scope (e.g., one struct/API per header). 
Encapsulation:  
Use static for file-local functions/variables.  
Opaque structs for data hiding (expose only pointers in headers). 
KISS & YAGNI:  
Avoid over-engineering; implement only what’s needed. 
Dependency Management:  
Minimize header includes; use forward declarations where possible.  
Guard headers against double inclusion: 
#ifndef MODULE_H 
#define MODULE_H 
/* ... code ... */ 
#endif 

3. Memory Management 
Zero Overflows:  
Use calloc over malloc for zero-initialized memory.
 Bounds Checking:  
Replace strcpy/sprintf with strncpy/snprintf. 
Resource Cleanup: 
 Free memory in reverse allocation order.  
Use goto for centralized error cleanup (adv topic) 

4. Error Handling 
Return Codes:  
Use 0/-1 or custom enum for status. 
Error Propagation:  
Check return values of all fallible functions (e.g., I/O, memory). 
Assertions:  
Use assert(ptr != NULL); for invariants (disabled in NDEBUG builds). 
Errno for System Errors: 
Log strerror(errno) on failures. 

5. Testing Strategies 
Unit Testing:  
Use frameworks like Unity or Check. 
 Test edge cases (e.g., NULL, empty strings, boundary values). 
Example test: 
void test_addition() { 
TEST_ASSERT_EQUAL(5, add(2, 3)); // Check equality
 } 
Static Analysis:  
Tools: clang-tidy, cppcheck, or splint.  
Example: cppcheck --enable=all mycode.c. 
Dynamic Analysis:  
Use Valgrind for memory leaks:  
Use AddressSanitizer (GCC/Clang): 
Fuzz Testing:  
Tools: AFL (American Fuzzy Lop) for crash discovery. 

6. Secure Coding 
Avoid Unsafe Functions:  
Replace gets with fgets, strcat with strncat. 
Integer Safety:  
Check for overflow before operations: 
if (a > INT_MAX - b) { /* handle overflow */ } 
Pointer Validation:  Check NULL before dereferencing. 

7. Build & Automation 
Compiler Flags:  
Enable warnings: -Wall -Wextra -Wpedantic -Werror.  
Debug symbols: -g. 
Makefiles:  
Automate builds, tests, and cleanup. 

8. Documentation 
Doxygen:  
Generate API docs from comments 

9. Performnce Considerations 
Optimize Last:  
Profile first (e.g., gprof, perf).
Prefer clarity over micro-optimizations. 
Cache Efficiency:  
Use contiguous memory (arrays vs. linked lists).

What
Value
HSE
Bypass (25MHz from ST-Link)
HAL Timebase
TIM6 (not SysTick)
ETH Mode
RMII
ETH NVIC Priority
5
FreeRTOS Heap
30720 bytes
INTERFACE_THREAD_STACK_SIZE
512
ethernetif_input priority
osPriorityNormal
MX_LWIP_Init() location
inside StartDefaultTask



Key lessons for STM32 + FreeRTOS + LwIP:
Always use HSE Bypass on Nucleo-144 (ST-Link provides the clock)
Always use TIM6 as HAL timebase, never SysTick
MX_LWIP_Init() must be inside a FreeRTOS task, not in main()
ETH interrupt priority must be 5 or higher
ethernetif_input task must NOT run at osPriorityRealtime
INTERFACE_THREAD_STACK_SIZE must be at least 512
Always put manual fixes inside /* USER CODE BEGIN */ blocks so CubeMX doesn't overwrite them

