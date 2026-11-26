#include "board.h"
#include "lab.h"
#include "dvm.h"
#include "scpi.h"
#include "stm32l4xx.h"
#include <string.h>

static scpi_t scpi;
static char idn[80];

static void c_led(scpi_t *s) { bool v; if (scpi_nparams(s, 1, 1) && scpi_bool(s, 0, &v)) led_set(v); }
static void q_led(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, led_get()); }
static void q_btn(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, button_pressed()); }
static void q_upt(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_num(s, millis() / 1000.0); }
static void q_clk(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, (int32_t)SystemCoreClock); }

/* Echo used by the bridge to find the end of a reply */
static void q_sync(scpi_t *s)
{
    double x;
    if (scpi_nparams(s, 1, 1) && scpi_number(s, 0, NULL, 0, 2e9, 0, &x)) { scpi_out_text(s, "SYNC"); scpi_out_int(s, (int32_t)x); }
}
static const scpi_cmd_t cmds[] = {
    {"SYSTem:SYNC?", q_sync},
    {"OUTPut:LED[:STATe]", c_led}, {"OUTPut:LED[:STATe]?", q_led},
    {"DIGital:INPut?", q_btn},
    {"SYSTem:UPTime?", q_upt}, {"SYSTem:CLOCk?", q_clk},
    {NULL, NULL}
};

static const scpi_cmd_t *const tables[] = {cmds, lab_cmds, dvm_cmds, NULL};
static void dev_reset(void) { led_set(false); lab_reset(); dvm_reset(); }
static const scpi_port_t port = {uart_write, dev_reset, lab_selftest, idn, lab_wait};

/* A fault (e.g. a supply glitch while wiring) restarts the board instead of
   leaving it hung until the next power cycle */
void HardFault_Handler(void) { NVIC_SystemReset(); }

int main(void)
{
    board_init();
    lab_init();
    uint32_t last = 0;
    strcpy(idn, "HUSO,NucleoLab-L452,");
    strcat(idn, board_serial());
    strcat(idn, ",1.0.0");
    scpi_init(&scpi, &port, tables);
    for (;;) {
        int c = uart_getc();
        lab_poll(&scpi);
        dvm_poll(&scpi);
        if (c >= 0) { scpi_input(&scpi, (char)c); last = millis(); continue; }
        if (millis() - last > 500u) scpi_input_timeout(&scpi);
        if (uart_take_overrun()) scpi_error(&scpi, -363);
        else __WFI();
    }
}
