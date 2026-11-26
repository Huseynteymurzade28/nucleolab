#ifndef ANALOG_H
#define ANALOG_H
#include <stdbool.h>
#include <stdint.h>
#define CAP_MAX 8192u
#define AWG_MAX 4096u
#define TIMER_HZ 80000000.0f

void  analog_init(void);
/* Voltmeter channels 1..6 = header A0..A5 (L452RE-P: PA0 PA1 PC3 PC2 PC1 PC0) */
#define ADC_CHANNELS 6u
/* one-shot, averaged; only while the scope is idle */
float adc_read_code(unsigned hw_channel, unsigned averages);
unsigned adc_hw_channel(unsigned ch);              /* scope: 1=PA0 2=PA1 3=PA4 (internal) */
float adc_read_dc(unsigned ch, unsigned averages);   /* long sample time, ADC codes */
int   adc_probe(unsigned ch);                        /* floating-pin test, see analog.c */
float vdda_measure(void);
float vdda(void);                  /* last measured */
float temp_measure(void);
uint16_t ts_cal1(void), ts_cal2(void), vref_cal(void);

void  dac_off(void);
void  dac_dc(uint16_t code);
void  dac_wave(const uint16_t *table, uint32_t n, uint32_t div);

typedef enum { SC_IDLE, SC_ARMED, SC_DONE, SC_ERROR } sc_state_t;
typedef struct {
    unsigned ch; uint32_t div, points, pre; uint16_t level; bool rising, autotrig;
} sc_cfg_t;
void  scope_start(const sc_cfg_t *c);
void  scope_abort(void);
void  scope_force(void);
void  scope_poll(void);
sc_state_t scope_state(void);
bool  scope_triggered(void);       /* last capture had a real trigger */
/* PA4 then PA-channel back to back on each trigger, DUAL_SKEW timer ticks apart */
#define DUAL_SKEW 37.0f
const uint16_t *dual_capture(unsigned ch, uint32_t div, uint32_t pairs);  /* NULL on error */
extern uint16_t scope_data[CAP_MAX];
#endif
