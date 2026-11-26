/* NUCLEO-L452RE-P: 80 MHz, USART2 on the ST-LINK VCP, user LED LD4 = PB13, B1 = PC13.
   (The plain NUCLEO-L452RE has its LED on PA5, which is header pin D13 on the -P.) */
#include "board.h"
#include "stm32l4xx.h"

#define BAUD 921600u
#define RXN  4096u                  /* power of two */
static volatile uint8_t rx[RXN];
static volatile uint32_t rx_w, rx_r, ticks;
static volatile bool rx_lost;
static char serial[25];

uint32_t SystemCoreClock = 4000000u;
void SystemInit(void) { SCB->CPACR |= 0xFu << 20; __DSB(); __ISB(); }
void _init(void) {}
void SysTick_Handler(void) { ticks++; }
uint32_t millis(void) { return ticks; }

static void clock_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    /* 32.768 kHz crystal disciplines the MSI, so UART and sample timing are accurate. */
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP)) {}
    if (!(RCC->BDCR & RCC_BDCR_LSERDY)) {
        RCC->BDCR |= RCC_BDCR_LSEON;
        uint32_t t0 = ticks;
        while (!(RCC->BDCR & RCC_BDCR_LSERDY) && ticks - t0 < 3000u) {}
    }
    if (RCC->BDCR & RCC_BDCR_LSERDY) RCC->CR |= RCC_CR_MSIPLLEN;

    FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_4WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {}
    /* MSI 4 MHz / 1 * 40 / 2 = 80 MHz */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_MSI | (40u << RCC_PLLCFGR_PLLN_Pos) | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
    SystemCoreClock = 80000000u;
}

#define LED_PIN 13u   /* PB13 */

static void pin_mode(GPIO_TypeDef *g, unsigned n, unsigned mode)
{
    g->MODER = (g->MODER & ~(3u << (2 * n))) | (mode << (2 * n));
}

void board_init(void)
{
    SysTick_Config(SystemCoreClock / 1000u);
    clock_init();
    SysTick_Config(SystemCoreClock / 1000u);

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN;
    (void)RCC->APB1ENR1;
    pin_mode(GPIOB, LED_PIN, 1);
    pin_mode(GPIOC, 13, 0);
    /* PC13 measured floating on this board, so use the internal pull-up. */
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(3u << 26)) | (1u << 26);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~0xFF00u) | 0x7700u;   /* PA2/PA3 AF7 */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 6)) | (1u << 6); /* RX pull-up */
    pin_mode(GPIOA, 2, 2);
    pin_mode(GPIOA, 3, 2);
    USART2->BRR = (SystemCoreClock + BAUD / 2u) / BAUD;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_EnableIRQ(USART2_IRQn);

    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    for (unsigned i = 0; i < 24; i++)
        serial[i] = "0123456789ABCDEF"[(uid[i / 8] >> (28 - 4 * (i % 8))) & 0xFu];
}

void USART2_IRQHandler(void)
{
    uint32_t isr = USART2->ISR;
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        if (isr & USART_ISR_ORE) rx_lost = true;
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    }
    if (isr & USART_ISR_RXNE) {
        uint8_t c = (uint8_t)USART2->RDR;
        if (rx_w - rx_r < RXN) { rx[rx_w % RXN] = c; rx_w++; }
        else rx_lost = true;
    }
}

int uart_getc(void)
{
    if (rx_r == rx_w) return -1;
    int c = rx[rx_r % RXN];
    rx_r++;
    return c;
}
bool uart_take_overrun(void) { bool v = rx_lost; if (v) rx_lost = false; return v; }

void uart_write(const char *s, size_t n)
{
    while (n--) {
        while (!(USART2->ISR & USART_ISR_TXE)) {}
        USART2->TDR = (uint8_t)*s++;
    }
}

void led_set(bool on) { GPIOB->BSRR = on ? 1u << LED_PIN : 1u << (LED_PIN + 16); }
bool led_get(void) { return GPIOB->ODR & (1u << LED_PIN); }
bool button_pressed(void) { return !(GPIOC->IDR & (1u << 13)); }
bool clock_is_pll(void) { return (RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL; }
bool clock_is_lse_locked(void) { return (RCC->BDCR & RCC_BDCR_LSERDY) && (RCC->CR & RCC_CR_MSIPLLEN); }
const char *board_serial(void) { return serial; }
