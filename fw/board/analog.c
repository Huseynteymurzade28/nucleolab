/* ADC1 (TIM2 paced, DMA1 ch1) and DAC1 (TIM6 paced, DMA1 ch3) */
#include "analog.h"
#include "board.h"
#include "stm32l4xx.h"

#define RING 16384u                 /* samples */
#define HYST 16                     /* codes, about 13 mV */
static uint16_t ring[RING];
uint16_t scope_data[CAP_MAX];
static volatile uint32_t laps;
static volatile sc_state_t state;
static sc_cfg_t cfg;
static uint64_t scan, trig;
static bool have_trig, real_trig, primed, force;
static uint32_t t_start;
static float vdd = 3.3f;

uint16_t vref_cal(void) { return *(const volatile uint16_t *)0x1FFF75AAu; }
uint16_t ts_cal1(void)  { return *(const volatile uint16_t *)0x1FFF75A8u; }
uint16_t ts_cal2(void)  { return *(const volatile uint16_t *)0x1FFF75CAu; }
/* Voltmeter channel n = Arduino header pin A(n-1), NUCLEO-L452RE-P (MB1319)
   wiring. (The plain NUCLEO-L452RE has A2 = PA4 and A3 = PB0 instead.) */
static const struct { GPIO_TypeDef *port; uint8_t pin, hw; } chmap[ADC_CHANNELS + 1] = {
    {0, 0, 0},
    {GPIOA, 0, 5},   /* 1: A0 PA0 */
    {GPIOA, 1, 6},   /* 2: A1 PA1 */
    {GPIOC, 3, 4},   /* 3: A2 PC3 */
    {GPIOC, 2, 3},   /* 4: A3 PC2 */
    {GPIOC, 1, 2},   /* 5: A4 PC1 */
    {GPIOC, 0, 1},   /* 6: A5 PC0 */
};
/* Scope channels (ACQ:CHAN): 1 = PA0, 2 = PA1, 3 = PA4, the DAC output read internally */
unsigned adc_hw_channel(unsigned ch) { return ch == 1 ? 5u : ch == 2 ? 6u : 9u; }

static void wait_us(uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT;
    while (DWT->CYCCNT - t0 < us * 80u) {}
}

static void adc_stop(void)
{
    TIM2->CR1 = 0;
    if (ADC1->CR & ADC_CR_ADSTART) {
        ADC1->CR |= ADC_CR_ADSTP;
        while (ADC1->CR & ADC_CR_ADSTP) {}
    }
    DMA1_Channel1->CCR = 0;
    DMA1->IFCR = DMA_IFCR_CGIF1;
    ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOC | ADC_ISR_EOS;
}

void analog_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    RCC->AHB2ENR |= RCC_AHB2ENR_ADCEN | RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN | RCC_APB1ENR1_TIM6EN | RCC_APB1ENR1_DAC1EN;
    (void)RCC->APB1ENR1;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) {              /* A0..A5 analog, no pulls */
        GPIO_TypeDef *g = chmap[c].port;
        unsigned sh = 2u * chmap[c].pin;
        g->PUPDR &= ~(3u << sh);
        g->MODER |= 3u << sh;
    }
    GPIOA->PUPDR &= ~(3u << 8);
    GPIOA->MODER |= 3u << 8;                                     /* PA4: DAC out / scope ch 3 */

    ADC1_COMMON->CCR = ADC_CCR_CKMODE_0 | ADC_CCR_VREFEN | ADC_CCR_TSEN; /* HCLK/1 */
    ADC1->CR = 0;                                                /* leave deep power down */
    ADC1->CR = ADC_CR_ADVREGEN;
    wait_us(30);
    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) {}
    wait_us(2);
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) {}
    /* ch0 640.5; ch1..6, 9 24.5 */
    ADC1->SMPR1 = 7u | (3u << 3) | (3u << 6) | (3u << 9) | (3u << 12) | (3u << 15) | (3u << 18) | (3u << 27);
    ADC1->SMPR2 = (7u << 21) | (3u << 15);                       /* ch17 640.5; ch15 24.5 */

    DMA1_CSELR->CSELR = (DMA1_CSELR->CSELR & ~0xF0Fu) | (6u << 8); /* ch1=ADC1, ch3=DAC1 */
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    TIM2->CR2 = 2u << TIM_CR2_MMS_Pos;
    TIM6->CR2 = 2u << TIM_CR2_MMS_Pos;
    dac_off();
    wait_us(200);
    vdda_measure();
}

void DMA1_Channel1_IRQHandler(void)
{
    if (DMA1->ISR & DMA_ISR_TCIF1) { DMA1->IFCR = DMA_IFCR_CTCIF1; laps++; }
}

float adc_read_code(unsigned hw, unsigned avg)
{
    adc_stop();
    ADC1->CFGR = ADC_CFGR_JQDIS;
    ADC1->SQR1 = hw << ADC_SQR1_SQ1_Pos;
    uint32_t sum = 0;
    for (unsigned i = 0; i <= avg; i++) {          /* first conversion discarded */
        ADC1->CR |= ADC_CR_ADSTART;
        while (!(ADC1->ISR & ADC_ISR_EOC)) {}
        uint32_t v = ADC1->DR;
        if (i) sum += v;
    }
    ADC1->ISR = ADC_ISR_EOS | ADC_ISR_EOC;
    return (float)sum / (float)avg;
}

/* Sample-time field of hw channel: register and shift */
static volatile uint32_t *smpr(unsigned hw, unsigned *sh)
{
    *sh = 3u * (hw % 10u);
    return hw < 10u ? &ADC1->SMPR1 : &ADC1->SMPR2;
}

/* DC level with the longest sample time, so a high source impedance
   (a potentiometer wiper, up to ~50 kOhm) still settles */
float adc_read_dc(unsigned ch, unsigned avg)
{
    unsigned hw = chmap[ch >= 1 && ch <= ADC_CHANNELS ? ch : 1].hw, sh;
    volatile uint32_t *r = smpr(hw, &sh);
    uint32_t old = *r;
    *r = (old & ~(7u << sh)) | (7u << sh);
    float v = adc_read_code(hw, avg);
    *r = old;
    return v;
}

static uint16_t convert_once(unsigned hw)
{
    ADC1->SQR1 = hw << ADC_SQR1_SQ1_Pos;
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC)) {}
    return (uint16_t)ADC1->DR;
}

int adc_probe_pin(GPIO_TypeDef *g, unsigned pin, unsigned hw);

/* Is something driving the pin? Charge the pin's own capacitance through the
   weak pull-up, switch back to analog and sample at once (24.5 cycles); then
   the same through the pull-down. A floating pin keeps the charge, so the two
   readings are far apart; a driven pin reads its own voltage both times.
   Returns up-reading minus down-reading in ADC codes. */
int adc_probe(unsigned ch)
{
    if (ch < 1 || ch > ADC_CHANNELS) return 0;
    return adc_probe_pin(chmap[ch].port, chmap[ch].pin, chmap[ch].hw);
}

int adc_probe_pin(GPIO_TypeDef *g, unsigned pin, unsigned hw)
{
    unsigned sh = 2u * pin;
    adc_stop();
    ADC1->CFGR = ADC_CFGR_JQDIS;
    convert_once(hw);                     /* park the sampling capacitor on this pin */
    int v[2];
    for (int k = 0; k < 2; k++) {
        /* repeated, so the ADC's sampling capacitor ends up charged too
           and no longer drags the pin back towards the previous level */
        for (int rep = 0; rep < 4; rep++) {
            g->PUPDR = (g->PUPDR & ~(3u << sh)) | ((k ? 2u : 1u) << sh);
            g->MODER &= ~(3u << sh);      /* input: the pull charges the pin */
            wait_us(20);
            g->MODER |= 3u << sh;         /* analog again */
            g->PUPDR &= ~(3u << sh);
            wait_us(5);                   /* a driven pin settles, a floating one keeps its charge */
            v[k] = convert_once(hw);      /* no IRQ masking: an interrupt here only
                                             delays the sample, the charge holds for ms */
        }
    }
    ADC1->ISR = ADC_ISR_EOS | ADC_ISR_EOC;
    g->MODER |= 3u << sh;
    return v[0] - v[1];
}


float vdda_measure(void) { vdd = 3.0f * (float)vref_cal() / adc_read_code(0, 256); return vdd; }
float vdda(void) { return vdd; }
float temp_measure(void)
{
    float k = vdda_measure() / 3.0f;
    float c = adc_read_code(17, 256) * k;
    /* CAL1 at 30 C, CAL2 at 130 C, both with VDDA = 3.0 V */
    return 30.0f + (c - (float)ts_cal1()) * 100.0f / (float)(ts_cal2() - ts_cal1());
}

/* ---------- DAC ---------- */
static void dac_stop(void)
{
    TIM6->CR1 = 0;
    DAC1->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_TEN1);
    DMA1_Channel3->CCR = 0;
    DMA1->IFCR = DMA_IFCR_CGIF3;
    DAC1->SR = DAC_SR_DMAUDR1;
}
void dac_off(void) { dac_stop(); DAC1->CR = 0; DAC1->MCR = 0; }
void dac_dc(uint16_t code)
{
    dac_stop();
    DAC1->CR |= DAC_CR_EN1;
    DAC1->DHR12R1 = code;
    wait_us(20);
}
void dac_wave(const uint16_t *t, uint32_t n, uint32_t div)
{
    dac_stop();
    DMA1_Channel3->CPAR = (uint32_t)&DAC1->DHR12R1;
    DMA1_Channel3->CMAR = (uint32_t)t;
    DMA1_Channel3->CNDTR = n;
    DMA1_Channel3->CCR = DMA_CCR_PL_1 | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_MINC |
                         DMA_CCR_CIRC | DMA_CCR_DIR | DMA_CCR_EN;
    DAC1->CR = DAC_CR_EN1;
    wait_us(20);
    DAC1->CR |= DAC_CR_TEN1 | DAC_CR_DMAEN1;       /* TSEL = 0: TIM6 TRGO */
    TIM6->PSC = 0;
    TIM6->ARR = div - 1u;
    TIM6->CNT = 0;
    TIM6->CR1 = TIM_CR1_CEN;
}

/* ---------- scope ---------- */
static uint64_t written(void)
{
    __disable_irq();
    uint32_t n = DMA1_Channel1->CNDTR, l = laps;
    if (DMA1->ISR & DMA_ISR_TCIF1) { n = DMA1_Channel1->CNDTR; l++; }
    __enable_irq();
    return (uint64_t)l * RING + (RING - n);
}

static void arm(void)
{
    adc_stop();
    NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    laps = 0; scan = 0; have_trig = false; primed = false;
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)ring;
    DMA1_Channel1->CNDTR = RING;
    DMA1_Channel1->CCR = DMA_CCR_PL | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_MINC |
                         DMA_CCR_CIRC | DMA_CCR_TCIE | DMA_CCR_EN;
    ADC1->SQR1 = adc_hw_channel(cfg.ch) << ADC_SQR1_SQ1_Pos;
    ADC1->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | ADC_CFGR_EXTEN_0 |
                 (11u << ADC_CFGR_EXTSEL_Pos);      /* TIM2_TRGO */
    TIM2->PSC = 0;
    TIM2->ARR = cfg.div - 1u;
    TIM2->EGR = TIM_EGR_UG;
    ADC1->CR |= ADC_CR_ADSTART;
    TIM2->CR1 = TIM_CR1_CEN;
    t_start = millis();
    state = SC_ARMED;
}

void scope_start(const sc_cfg_t *c) { cfg = *c; force = false; arm(); }
void scope_abort(void) { adc_stop(); if (state == SC_ARMED) state = SC_IDLE; }
void scope_force(void) { force = true; }
sc_state_t scope_state(void) { return state; }
bool scope_triggered(void) { return real_trig; }

void scope_poll(void)
{
    if (state != SC_ARMED) return;
    if (ADC1->ISR & ADC_ISR_OVR || DMA1->ISR & DMA_ISR_TEIF1) { adc_stop(); state = SC_ERROR; return; }
    uint64_t w = written();
    if (!have_trig) {
        if (w - scan > RING - 1024u) { scan = w - RING / 2; primed = false; } /* fell behind */
        while (scan < w) {
            int v = ring[scan % RING], l = cfg.level;
            if (primed && (cfg.rising ? v >= l : v <= l)) {
                primed = false;
                if (scan >= cfg.pre) { trig = scan; have_trig = real_trig = true; break; }
            } else if (cfg.rising ? v < l - HYST : v > l + HYST) primed = true;
            scan++;
        }
        bool timeout = cfg.autotrig && millis() - t_start > 50u;
        if (!have_trig && (force || timeout) && w > cfg.pre) {
            trig = w; have_trig = true; real_trig = false;
        }
        if (!have_trig) return;
    }
    uint64_t first = trig - cfg.pre;
    if (w < first + cfg.points) return;
    if (w - first > RING - 64u) { arm(); return; }   /* overwritten: try again */
    adc_stop();
    for (uint32_t i = 0; i < cfg.points; i++) scope_data[i] = ring[(first + i) % RING];
    state = SC_DONE;
}

const uint16_t *dual_capture(unsigned ch, uint32_t div, uint32_t pairs)
{
    scope_abort();
    adc_stop();
    DMA1_Channel1->CPAR = (uint32_t)&ADC1->DR;
    DMA1_Channel1->CMAR = (uint32_t)ring;
    DMA1_Channel1->CNDTR = 2u * pairs;
    DMA1_Channel1->CCR = DMA_CCR_PL | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0 | DMA_CCR_MINC | DMA_CCR_EN;
    ADC1->SQR1 = 1u | (adc_hw_channel(3) << ADC_SQR1_SQ1_Pos) | (adc_hw_channel(ch) << ADC_SQR1_SQ2_Pos);
    ADC1->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_DMAEN | ADC_CFGR_EXTEN_0 | (11u << ADC_CFGR_EXTSEL_Pos);
    TIM2->PSC = 0;
    TIM2->ARR = div - 1u;
    TIM2->EGR = TIM_EGR_UG;
    ADC1->CR |= ADC_CR_ADSTART;
    TIM2->CR1 = TIM_CR1_CEN;
    uint32_t t0 = millis(), limit = (uint32_t)((float)div * (float)pairs / 80000.0f) + 100u;
    bool ok = true;
    while (DMA1_Channel1->CNDTR) if (millis() - t0 > limit) { ok = false; break; }
    if (ADC1->ISR & ADC_ISR_OVR) ok = false;
    adc_stop();
    return ok ? ring : 0;
}
