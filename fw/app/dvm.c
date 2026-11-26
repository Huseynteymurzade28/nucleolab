/* Multi-channel DC voltmeter: channel n = Arduino pin A(n-1).
   A background task probes each input in turn (adc_probe) and keeps a
   debounced "connected" flag, so plugging a wire in or out is noticed
   within a few hundred ms. A connected channel is also sampled on its
   turn, which feeds the running statistics (CALC:AVER...). */
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
#define AVERAGES    64u     /* default of SENS:VOLT:AVER:COUN */
#define BG_AVERAGES 16u     /* background samples: keep one tick short */
#define VDDA_MS     1000u   /* background VDDA refresh */

static const char *const names[ADC_CHANNELS + 1] = {"", "A0", "A1", "A2", "A3", "A4", "A5"};
static bool conn[ADC_CHANNELS + 1];
static int streak[ADC_CHANNELS + 1], spread[ADC_CHANNELS + 1];
static uint32_t changes, next_ms, vdda_ms;
static unsigned turn = 1, averages = AVERAGES;
static struct { float min, max; double sum; uint32_t n; } st[ADC_CHANNELS + 1];

static bool adc_busy(void) { return scope_state() == SC_ARMED; }
static bool connected(unsigned ch) { return conn[ch]; }

static bool probe(unsigned ch)
{
    spread[ch] = adc_probe(ch);
    return spread[ch] < SPREAD_MIN;
}

static void sample(unsigned ch)
{
    float v = adc_read_dc(ch, BG_AVERAGES) * vdda() / 4096.0f;
    if (!st[ch].n || v < st[ch].min) st[ch].min = v;
    if (!st[ch].n || v > st[ch].max) st[ch].max = v;
    st[ch].sum = st[ch].n ? st[ch].sum + v : v;
    st[ch].n++;
}

/* *RST: settings back to their defaults */
void dvm_reset(void)
{
    averages = AVERAGES;
}

void dvm_init(void)
{
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) conn[c] = probe(c);
}

void dvm_poll(scpi_t *s)
{
    if (adc_busy() || (int32_t)(millis() - next_ms) < 0) return;
    next_ms = millis() + PROBE_MS;
    if ((int32_t)(millis() - vdda_ms) >= 0) { vdda_measure(); vdda_ms = millis() + VDDA_MS; }
    if (++turn > ADC_CHANNELS) turn = 1;
    bool now = probe(turn);
    if (now != conn[turn] && ++streak[turn] >= DEBOUNCE) {
        streak[turn] = 0;
        conn[turn] = now;
        changes++;
        s->oper.event |= OPER_CHAN;
        st[turn].n = 0;                   /* a new connection starts new statistics */
    } else if (now == conn[turn]) {
        streak[turn] = 0;
    }
    if (conn[turn]) sample(turn);
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
        if (m & (1u << c)) scpi_out_num(s, connected(c) ? adc_read_dc(c, averages) * lsb : NAN);
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
/* Running statistics of the background samples, per listed channel; NaN
   (count 0) for a channel without samples */
enum { S_MIN, S_MAX, S_AVG, S_CNT };
static void stat_query(scpi_t *s, int what)
{
    uint32_t m;
    if (!chans(s, &m)) return;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) {
        if (!(m & (1u << c))) continue;
        uint32_t n = st[c].n;
        switch (what) {
        case S_MIN: scpi_out_num(s, n ? st[c].min : NAN); break;
        case S_MAX: scpi_out_num(s, n ? st[c].max : NAN); break;
        case S_AVG: scpi_out_num(s, n ? (float)(st[c].sum / n) : NAN); break;
        default:    scpi_out_int(s, (int32_t)n);
        }
    }
}
static void q_smin(scpi_t *s) { stat_query(s, S_MIN); }
static void q_smax(scpi_t *s) { stat_query(s, S_MAX); }
static void q_savg(scpi_t *s) { stat_query(s, S_AVG); }
static void q_scnt(scpi_t *s) { stat_query(s, S_CNT); }
static void c_sclr(scpi_t *s)
{
    uint32_t m;
    if (chans(s, &m))
        for (unsigned c = 1; c <= ADC_CHANNELS; c++)
            if (m & (1u << c)) st[c].n = 0;
}

static void c_aver(scpi_t *s)
{
    double x;
    if (scpi_nparams(s, 1, 1) && scpi_number(s, 0, NULL, 1, 256, AVERAGES, &x)) averages = (unsigned)x;
}
static void q_aver(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, (int32_t)averages); }

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
    {"CALCulate:AVERage:MINimum?", q_smin}, {"CALCulate:AVERage:MAXimum?", q_smax},
    {"CALCulate:AVERage[:AVERage]?", q_savg}, {"CALCulate:AVERage:COUNt?", q_scnt},
    {"CALCulate:AVERage:CLEar", c_sclr},
    {"[SENSe]:VOLTage:AVERage:COUNt", c_aver}, {"[SENSe]:VOLTage:AVERage:COUNt?", q_aver},
    {NULL, NULL}
};
