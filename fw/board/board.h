#ifndef BOARD_H
#define BOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void board_init(void);
void uart_write(const char *s, size_t n);
int  uart_getc(void);              /* -1 when empty */
bool uart_take_overrun(void);
void led_set(bool on);
bool led_get(void);
bool button_pressed(void);
uint32_t millis(void);
bool clock_is_pll(void);
bool clock_is_lse_locked(void);
const char *board_serial(void);    /* 24 hex digits */
#endif
