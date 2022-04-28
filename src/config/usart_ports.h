#pragma once

#include "target.h"

//  CONDITIONAL SELECTION OF DEFINES BASED ON USER SELECTED UART FOR SERIAL RX, TARGET MCU, AND TARGET DEFINED USART ALTERNATE MAPPING PINS
#ifdef STM32F4
#define USART1_PA10PA9 USART_PORT(1, PIN_A10, PIN_A9)
#define USART1_PB7PB6 USART_PORT(1, PIN_B7, PIN_B6)
#define USART1_PB7PA9 USART_PORT(1, PIN_B7, PIN_A9)
#define USART2_PA3PA2 USART_PORT(2, PIN_A3, PIN_A2)
#define USART3_PB11PB10 USART_PORT(3, PIN_B11, PIN_B10)
#define USART3_PC11PC10 USART_PORT(3, PIN_C11, PIN_C10)
#define USART4_PA1PA0 USART_PORT(4, PIN_A1, PIN_A0)
#define USART5_PD2PC12 USART_PORT(5, PIN_D2, PIN_C12)
#define USART6_PC7PC6 USART_PORT(6, PIN_C7, PIN_C6)
#endif

#ifdef STM32F7
#define USART1_PA10PA9 USART_PORT(1, PIN_A10, PIN_A9)
#define USART1_PB7PB6 USART_PORT(1, PIN_B7, PIN_B6)

#define USART2_PD6PD5 USART_PORT(2, PIN_D6, PIN_D5)
#define USART2_PA3PA2 USART_PORT(2, PIN_A3, PIN_A2)

#define USART3_PD8PD9 USART_PORT(3, PIN_D8, PIN_D9)
#define USART3_PB11PB10 USART_PORT(3, PIN_B11, PIN_B10)
#define USART3_PC11PC10 USART_PORT(3, PIN_C11, PIN_C10)

#define USART4_PA1PA0 USART_PORT(4, PIN_A1, PIN_A0)
#define USART4_PC11PC10 USART_PORT(4, PIN_C11, PIN_C10)

#define USART5_PD2PC12 USART_PORT(5, PIN_D2, PIN_C12)

#define USART6_PC7PC6 USART_PORT(6, PIN_C7, PIN_C6)

#define USART7_PE7PE8 USART_PORT(7, PIN_E7, PIN_E8)

#define USART8_PE0PE1 USART_PORT(8, PIN_E0, PIN_E1)
#endif

#ifdef STM32H7
#define USART1_PA10PA9 USART_PORT(1, PIN_A10, PIN_A9)
#define USART2_PD6PD5 USART_PORT(2, PIN_D6, PIN_D5)
#define USART3_PD9PD8 USART_PORT(3, PIN_D9, PIN_D8)
#define USART4_PD0PD1 USART_PORT(4, PIN_D0, PIN_D1)
#define USART6_PC7PC6 USART_PORT(6, PIN_C7, PIN_C6)
#define USART7_PE7PE8 USART_PORT(7, PIN_E7, PIN_E8)
#endif

#define USART_IDENT(channel) USART_PORT##channel
#define SOFT_SERIAL_IDENT(channel) SOFT_SERIAL_PORT##channel

typedef enum {
  USART_PORT_INVALID,

#define USART_PORT(channel, rx_pin, tx_pin) USART_IDENT(channel),
#define SOFT_SERIAL_PORT(index, rx_pin, tx_pin)
  USART_PORTS
#undef USART_PORT
#undef SOFT_SERIAL_PORT
      USART_PORTS_MAX,

#ifdef USE_SOFT_SERIAL

#define USART_PORT(channel, rx_pin, tx_pin)
#define SOFT_SERIAL_PORT(index, rx_pin, tx_pin) SOFT_SERIAL_IDENT(index) = (USART_PORTS_MAX + index - 1),
  USART_PORTS
#undef USART_PORT
#undef SOFT_SERIAL_PORT
      SOFT_SERIAL_PORTS_MAX,

#else
  SOFT_SERIAL_PORTS_MAX = USART_PORTS_MAX,
#endif

} usart_ports_t;
