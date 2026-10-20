#include "stm32f30x_conf.h"
#include "30010_io.h"
#include <string.h>

//OpenLog Init
static void openlog_init(void)
{
    // Activate clock to GPIOA and USART1.
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    // GPIO Pin config    
    GPIO_InitTypeDef gpio;
    GPIO_StructInit(&gpio);
        gpio.GPIO_Pin   = GPIO_Pin_9 | GPIO_Pin_10;
        gpio.GPIO_Mode  = GPIO_Mode_AF;                 //AF = Alternate Function (Lets SD-module control pins)
        gpio.GPIO_Speed = GPIO_Speed_10MHz;
        gpio.GPIO_OType = GPIO_OType_PP;                //Push-Pull
        gpio.GPIO_PuPd  = GPIO_PuPd_UP;                 //Pull-up, UART is high when not transmitting
    GPIO_Init(GPIOA, &gpio);

    // Alternate Function 7 is used to connect Pin 9 as TX and Pin 10 as RX
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_7);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_7);

    // USART Conf settings (standard)
    USART_InitTypeDef uart;
    USART_StructInit(&uart);
        uart.USART_BaudRate            = 9600;
        uart.USART_WordLength          = USART_WordLength_8b;
        uart.USART_StopBits            = USART_StopBits_1;
        uart.USART_Parity              = USART_Parity_No;
        uart.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;     //Enable both Transmit (TX) and Recieve (RX) for USART
        uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(USART1, &uart);                          
    USART_Cmd(USART1, ENABLE);
}

//Sends byte to Openlog
static void openlog_put_char(uint8_t c)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) 
    {
    }

    USART_SendData(USART1, c);
}

//sends termination string to OpenLog
//const char *text is a pointer to text that we dont change
static void openlog_write(const char *text) 
{
    while (*text) {
        openlog_put_char((uint8_t)*text++); 
    }
}

//checks for specific sign from OpenLog
static void openlog_wait_for(uint8_t expected) 
{
    for (;;) //infinite Loop
    {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            uint8_t c = (uint8_t)USART_ReceiveData(USART1);

            // If byte matches end function. Other signs will be read and ignored
            if (c == expected) {
                return;
            }

            // Forward the received byte to the PC through USART2.
            uart_put_char(c);
        }
    }
}

//Read files on SD-card
static void openlog_print_response(void)
{
    for (;;) {
        
        //Clear an overrun error if one occurred.
        if (USART_GetFlagStatus(USART1, USART_FLAG_ORE) != RESET) {
            USART1->ICR = USART_ICR_ORECF;
        }
        
        // Read each available byte from OpenLog.
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            uint8_t c = (uint8_t)USART_ReceiveData(USART1);

            // OpenLog has returned to command mode.
            if (c == '>') {
                return;
            }

            // Forward the received byte to the PC through USART2.
            uart_put_char(c);
        }
    }
}

//Read a filename from the PC through USART2 (PA9)
static void pc_read_filename(char *filename, unsigned int size)
{
    unsigned int i = 0;

    for (;;) {
        // Wait for a character from the PC.
        if (uart_get_count() == 0) {
            continue;
        }

        char c = (char)uart_get_char();

        // Accept CR or LF as Enter.
        if (c == '\r' || c == '\n') {
            // Ignore empty lines, including the LF in CRLF.
            if (i == 0) {
                continue;
            }

            filename[i] = '\0';
            printf("\n");
            return;
        }

        // Allow the user to erase the previous character.
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                printf("\b \b");
            }
        }
        // Store and echo printable characters while space remains.
        else if (c >= 32 && c <= 126 && i < size - 1) {
            filename[i++] = c;
            uart_put_char((uint8_t)c);
        }
    }
}

//Read user input for selecting Files on SD-card (through Putty)
static int pc_read_line(char *buffer, unsigned int capacity)
{
    static uint8_t skip_lf = 0;
    unsigned int length = 0;
    uint8_t overflow = 0;

    for (;;) {
        if (uart_get_count() == 0) {
            continue;
        }

        uint8_t c = uart_get_char();

        // Ignore the LF belonging to a previous CRLF sequence.
        if (skip_lf) {
            skip_lf = 0;

            if (c == '\n') {
                continue;
            }
        }

        if (c == '\r' || c == '\n') {
            skip_lf = (c == '\r');
            buffer[length] = '\0';
            printf("\n");

            return !overflow;
        }

        // Handle Backspace and Delete.
        if (c == '\b' || c == 127) {
            if (length > 0 && !overflow) {
                length--;
                printf("\b \b");
            }

            continue;
        }

        // Accept printable ASCII characters only.
        if (c >= 32 && c <= 126) {
            if (!overflow && length < capacity - 1) {
                buffer[length++] = (char)c;

                // Echo the character to the Serial Monitor.
                uart_put_char(c);
            } else {
                // Discard the rest of an oversized line.
                overflow = 1;
            }
        }
    }
}

//Check if filename follows 8.3 format.
static int valid_filename(const char *name)
{
    //Standard OpenLog filenames use the 8.3 format.
    unsigned int base_length = 0;
    unsigned int extension_length = 0;
    uint8_t dot_found = 0;

    while (*name) {
        char c = *name++;

        //Checks for '.' when reading file name to specifiy file-type
        //eg: LOG123 '.' TXT -> LOG123.TXT
        if (c == '.') {
            if (dot_found || base_length == 0) {
                return 0;
                //Rejects if '.' appears more than once
            }

            dot_found = 1;
            // The dot itself is not counted as a name character.
            continue;
        }

        // Allows characters: A-Z, a-z, 0-9, underscore, and hyphen.
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return 0;
        }

        // Count the character according to its position.
        if (dot_found) {
            extension_length++;
        } else {
            base_length++;
        }
    }

    //Checks for valid file-name:
    return base_length >= 1 && base_length <= 8 &&
           extension_length <= 3 &&
           (!dot_found || extension_length >= 1);
        //Return 1 for a valid filename, or 0 for an invalid filename.
}

int main(void)
{
    uart_init(115200);  // USART2: PC / Serial Monitor    PA9
    openlog_init();   // USART1: OpenLog                PA10

    printf("Wait for OpenLog. Turn on or Restart module.\n");

    openlog_wait_for('<');

    openlog_write("Test fra STM32 Gruppe 3\r\n");
    openlog_write("Tid_ms,Vaerdi\r\n");
    openlog_write("0,123\r\n");

    // Standard-escape: End Logging and save Buffer
    openlog_put_char(26);
    openlog_put_char(26);
    openlog_put_char(26);

    openlog_wait_for('>');

        printf("Logging finished.\n");

    char filename[32];

    while (1) {
        // Show the available files.
        printf("\nFiles on the SD card:\n");
        openlog_write("ls\r");
        openlog_print_response();

        // Wait for a filename from the Serial Monitor.
        printf("\nEnter a filename and press Enter:\n");
        pc_read_filename(filename, sizeof(filename));

        printf("\nReading %s:\n", filename);

        // Send the complete command: read <filename><CR>.
        openlog_write("read ");
        openlog_write(filename);
        openlog_write("\r");

        // Forward the response from OpenLog to the PC.
        openlog_print_response();

        printf("\nRead command finished.\n");

        
    }
}