/* Multi-channel DC voltmeter: channel n = Arduino pin A(n-1).
   A background task probes each input in turn (adc_probe) and keeps a
   debounced "connected" flag, so plugging a wire in or out is noticed
   within a few hundred ms. */
#include "dvm.h"
#include "analog.h"
#include "board.h"
#include <math.h>

#define ALL_CH      0x7Eu   /* bits 1..6 */
#define SPREAD_MIN  300     /* codes: probe spread above this = floating
                                   (measured: driven ~0, floating 700-800) */
#define PROBE_MS    20u     /* one channel per tick */
#define DEBOUNCE    2       /* equal probes in a row before the flag flips */
#define OPER_CHAN   0x200u  /* STATus:OPERation bit 9: a channel was (dis)connected */
#define AVERAGES    64u

static const char *const names[ADC_CHANNELS + 1] = {"", "A0", "A1", "A2", "A3", "A4", "A5"};
static bool conn[ADC_CHANNELS + 1];
static int streak[ADC_CHANNELS + 1], spread[ADC_CHANNELS + 1];
static uint32_t changes, next_ms;
static unsigned turn = 1;

static bool adc_busy(void) { return scope_state() == SC_ARMED; }
static bool connected(unsigned ch) { return conn[ch]; }

static bool probe(unsigned ch)
{
    spread[ch] = adc_probe(ch);
    return spread[ch] < SPREAD_MIN;
}

void dvm_init(void)
{
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) conn[c] = probe(c);
}

void dvm_poll(scpi_t *s)
{
    if (adc_busy() || (int32_t)(millis() - next_ms) < 0) return;
    next_ms = millis() + PROBE_MS;
    if (++turn > ADC_CHANNELS) turn = 1;
    bool now = probe(turn);
    if (now == conn[turn]) { streak[turn] = 0; return; }
    if (++streak[turn] < DEBOUNCE) return;
    streak[turn] = 0;
    conn[turn] = now;
    changes++;
    s->oper.event |= OPER_CHAN;
}

/* Optional channel list parameter; default all channels */
static bool chans(scpi_t *s, uint32_t *mask)
{
    if (!scpi_nparams(s, 0, 1)) return false;
    if (s->nparam == 0) { *mask = ALL_CH; return true; }
    return scpi_chanlist(s, 0, 1, ADC_CHANNELS, mask);
}

/* Volts per listed channel; 9.91E+37 (SCPI NaN) where nothing is connected */
void dvm_measure(scpi_t *s)
{
    uint32_t m;
    if (!chans(s, &m)) return;
    if (adc_busy()) { scpi_error(s, -221); return; }
    float lsb = vdda_measure() / 4096.0f;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++)
        if (m & (1u << c)) scpi_out_num(s, connected(c) ? adc_read_dc(c, AVERAGES) * lsb : NAN);
}

static void q_conn(scpi_t *s)
{
    uint32_t m;
    if (chans(s, &m))
        for (unsigned c = 1; c <= ADC_CHANNELS; c++)
            if (m & (1u << c)) scpi_out_int(s, connected(c));
}
static void q_cat(scpi_t *s)
{
    if (!scpi_nparams(s, 0, 0)) return;
    char buf[2 * ADC_CHANNELS + 4] = "(@";
    unsigned n = 2;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) {
        if (!connected(c)) continue;
        if (n > 2) buf[n++] = ',';
        buf[n++] = (char)('0' + c);
    }
    buf[n++] = ')';
    buf[n] = 0;
    scpi_out_text(s, buf);
}
static void q_name(scpi_t *s)
{
    uint32_t m;
    if (chans(s, &m))
        for (unsigned c = 1; c <= ADC_CHANNELS; c++)
            if (m & (1u << c)) scpi_out_quoted(s, names[c]);
}
static void q_chg(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, (int32_t)changes); }
static void q_probe(scpi_t *s)
{
    uint32_t m;
    if (!chans(s, &m)) return;
    if (adc_busy()) { scpi_error(s, -221); return; }
    for (unsigned c = 1; c <= ADC_CHANNELS; c++)
        if (m & (1u << c)) scpi_out_int(s, adc_probe(c));
}

const scpi_cmd_t dvm_cmds[] = {
    {"ROUTe:CHANnel:CONNected?", q_conn}, {"ROUTe:CHANnel:CATalog?", q_cat},
    {"ROUTe:CHANnel:NAME?", q_name}, {"ROUTe:CHANnel:CHANges?", q_chg},
    {"DIAGnostic:CHANnel:PROBe?", q_probe},
    {NULL, NULL}
};
