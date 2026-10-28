#pragma once
#define UART_NUM_2 2
#define pdMS_TO_TICKS(ms) (ms)
enum {UART_SIGNAL_RXD_INV=1,UART_SIGNAL_TXD_INV=2,UART_SIGNAL_INV_DISABLE=0};
inline void uart_set_line_inverse(int,int) {}
inline void uart_flush_input(int) {}
inline int uart_wait_tx_done(int,int) { return 0; }
