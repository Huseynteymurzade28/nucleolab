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
#define SPREAD_MIN  300     /* default threshold, codes: probe spread above it = floating
                                   (measured: driven ~0, floating 700-800) */
#define PROBE_MS    20u     /* one channel per tick */
#define DEBOUNCE    2       /* equal probes in a row before the flag flips */
#define OPER_CHAN   0x200u  /* STATus:OPERation bit 9: a channel was (dis)connected */
#define QUES_LIMIT  0x400u  /* STATus:QUEStionable bit 10: a channel is outside its limits */
#define LIM_LOW     0.0f    /* default limits: the whole input range */
#define LIM_UPP     3.6f
#define AVERAGES    64u     /* default of SENS:VOLT:AVER:COUN */
#define BG_AVERAGES 16u     /* background samples: keep one tick short */
#define VDDA_MS     1000u   /* background VDDA refresh */

static const char *const names[ADC_CHANNELS + 1] = {"", "A0", "A1", "A2", "A3", "A4", "A5"};
static bool conn[ADC_CHANNELS + 1];
static int streak[ADC_CHANNELS + 1], spread[ADC_CHANNELS + 1];
static uint32_t changes, next_ms, vdda_ms;
static unsigned turn = 1, averages = AVERAGES, threshold = SPREAD_MIN;
static bool detect = true;
static struct { float min, max; double sum; uint32_t n; } st[ADC_CHANNELS + 1];
static float last[ADC_CHANNELS + 1], lim_low[ADC_CHANNELS + 1], lim_upp[ADC_CHANNELS + 1];
static bool lim_on, fail[ADC_CHANNELS + 1];

/* Event log: channel, what happened, millis(); the oldest is dropped when full */
enum { EV_UNPLUGGED, EV_PLUGGED, EV_LIMIT_FAIL, EV_LIMIT_OK };
#define EV_MAX 16u
static struct { uint8_t ch, what; uint32_t ms; } ev[EV_MAX];
static unsigned ev_head, ev_count;
static void log_event(unsigned ch, unsigned what)
{
    if (ev_count == EV_MAX) { ev_head = (ev_head + 1) % EV_MAX; ev_count--; }
    unsigned i = (ev_head + ev_count++) % EV_MAX;
    ev[i].ch = (uint8_t)ch; ev[i].what = (uint8_t)what; ev[i].ms = millis();
}

static bool adc_busy(void) { return scope_state() == SC_ARMED; }
static bool connected(unsigned ch) { return conn[ch]; }

static bool probe(unsigned ch)
{
    if (!detect) return true;             /* detection off: every channel counts as connected */
    spread[ch] = adc_probe(ch);
    return spread[ch] < (int)threshold;
}

static void sample(unsigned ch)
{
    float v = adc_read_dc(ch, BG_AVERAGES) * vdda() / 4096.0f;
    if (!st[ch].n || v < st[ch].min) st[ch].min = v;
    if (!st[ch].n || v > st[ch].max) st[ch].max = v;
    st[ch].sum = st[ch].n ? st[ch].sum + v : v;
    st[ch].n++;
    last[ch] = v;
}

/* Limit state from the latest background sample; QUES bit 10 follows it */
static void check_limits(scpi_t *s, unsigned ch)
{
    bool was = fail[ch];
    fail[ch] = lim_on && conn[ch] && (last[ch] < lim_low[ch] || last[ch] > lim_upp[ch]);
    if (fail[ch] != was) log_event(ch, fail[ch] ? EV_LIMIT_FAIL : EV_LIMIT_OK);
    bool any = false;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) any |= fail[c];
    if (any && !(s->ques.cond & QUES_LIMIT)) s->ques.event |= QUES_LIMIT;
    s->ques.cond = any ? s->ques.cond | QUES_LIMIT : s->ques.cond & ~QUES_LIMIT;
}

/* *RST: settings back to their defaults */
void dvm_reset(void)
{
    averages = AVERAGES;
    detect = true;
    threshold = SPREAD_MIN;
    lim_on = false;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) { lim_low[c] = LIM_LOW; lim_upp[c] = LIM_UPP; fail[c] = false; }
}

void dvm_init(void)
{
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) conn[c] = probe(c);
    dvm_reset();
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
        log_event(turn, now ? EV_PLUGGED : EV_UNPLUGGED);
        st[turn].n = 0;                   /* a new connection starts new statistics */
    } else if (now == conn[turn]) {
        streak[turn] = 0;
    }
    if (conn[turn]) sample(turn);
    check_limits(s, turn);
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

/* Averaged ADC codes (0..4095) instead of volts; NaN where nothing is connected */
static void q_raw(scpi_t *s)
{
    uint32_t m;
    if (!chans(s, &m)) return;
    if (adc_busy()) { scpi_error(s, -221); return; }
    for (unsigned c = 1; c <= ADC_CHANNELS; c++)
        if (m & (1u << c)) scpi_out_num(s, connected(c) ? adc_read_dc(c, averages) : NAN);
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

/* CALC:LIM:LOW|UPP <volts>[,(@list)] */
static void set_limit(scpi_t *s, float *lim, double def)
{
    double x;
    uint32_t m = ALL_CH;
    if (!scpi_nparams(s, 1, 2) || !scpi_number(s, 0, "V", 0, 3.6, def, &x)) return;
    if (s->nparam > 1 && !scpi_chanlist(s, 1, 1, ADC_CHANNELS, &m)) return;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++)
        if (m & (1u << c)) lim[c] = (float)x;
}
static void get_limit(scpi_t *s, const float *lim)
{
    uint32_t m;
    if (chans(s, &m))
        for (unsigned c = 1; c <= ADC_CHANNELS; c++)
            if (m & (1u << c)) scpi_out_num(s, lim[c]);
}
static void c_llow(scpi_t *s) { set_limit(s, lim_low, LIM_LOW); }
static void c_lupp(scpi_t *s) { set_limit(s, lim_upp, LIM_UPP); }
static void q_llow(scpi_t *s) { get_limit(s, lim_low); }
static void q_lupp(scpi_t *s) { get_limit(s, lim_upp); }
static void c_lstat(scpi_t *s)
{
    bool v;
    if (scpi_nparams(s, 1, 1) && scpi_bool(s, 0, &v)) lim_on = v;
}
static void q_lstat(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, lim_on); }
static void q_lfail(scpi_t *s)
{
    uint32_t m;
    if (chans(s, &m))
        for (unsigned c = 1; c <= ADC_CHANNELS; c++)
            if (m & (1u << c)) scpi_out_int(s, fail[c]);
}

/* Oldest logged event as <channel>,<what>,<ms since boot>; 0,0,0 when empty.
   what: 0 unplugged, 1 plugged in, 2 out of limits, 3 back within limits */
static void q_event(scpi_t *s)
{
    if (!scpi_nparams(s, 0, 0)) return;
    if (!ev_count) { scpi_out_int(s, 0); scpi_out_int(s, 0); scpi_out_int(s, 0); return; }
    scpi_out_int(s, ev[ev_head].ch);
    scpi_out_int(s, ev[ev_head].what);
    scpi_out_int(s, (int32_t)ev[ev_head].ms);
    ev_head = (ev_head + 1) % EV_MAX;
    ev_count--;
}
static void q_evcount(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, (int32_t)ev_count); }

/* Off: no probing at all (no pulls ever touch the pins), every channel is
   treated as connected. Turning it back on re-detects within a few ticks. */
static void c_det(scpi_t *s)
{
    bool v;
    if (!scpi_nparams(s, 1, 1) || !scpi_bool(s, 0, &v)) return;
    detect = v;
    for (unsigned c = 1; c <= ADC_CHANNELS; c++) streak[c] = 0;
    if (!v)
        for (unsigned c = 1; c <= ADC_CHANNELS; c++) conn[c] = true;
}
static void q_det(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, detect); }
static void c_thr(scpi_t *s)
{
    double x;
    if (scpi_nparams(s, 1, 1) && scpi_number(s, 0, NULL, 50, 4000, SPREAD_MIN, &x)) threshold = (unsigned)x;
}
static void q_thr(scpi_t *s) { if (scpi_nparams(s, 0, 0)) scpi_out_int(s, (int32_t)threshold); }

static void q_probe(scpi_t *s)
{
    uint32_t m;
    if (!chans(s, &m)) return;
    if (adc_busy()) { scpi_error(s, -221); return; }
    for (unsigned c = 1; c <= ADC_CHANNELS; c++)
        if (m & (1u << c)) scpi_out_int(s, adc_probe(c));
}

const scpi_cmd_t dvm_cmds[] = {
    {"MEASure:VOLTage:RAW?", q_raw},
    {"ROUTe:CHANnel:CONNected?", q_conn}, {"ROUTe:CHANnel:CATalog?", q_cat},
    {"ROUTe:CHANnel:NAME?", q_name}, {"ROUTe:CHANnel:CHANges?", q_chg},
    {"ROUTe:CHANnel:EVENt?", q_event}, {"ROUTe:CHANnel:EVENt:COUNt?", q_evcount},
    {"ROUTe:CHANnel:DETect[:STATe]", c_det}, {"ROUTe:CHANnel:DETect[:STATe]?", q_det},
    {"ROUTe:CHANnel:DETect:THReshold", c_thr}, {"ROUTe:CHANnel:DETect:THReshold?", q_thr},
    {"DIAGnostic:CHANnel:PROBe?", q_probe},
    {"CALCulate:AVERage:MINimum?", q_smin}, {"CALCulate:AVERage:MAXimum?", q_smax},
    {"CALCulate:AVERage[:AVERage]?", q_savg}, {"CALCulate:AVERage:COUNt?", q_scnt},
    {"CALCulate:AVERage:CLEar", c_sclr},
    {"CALCulate:LIMit:LOWer[:DATA]", c_llow}, {"CALCulate:LIMit:LOWer[:DATA]?", q_llow},
    {"CALCulate:LIMit:UPPer[:DATA]", c_lupp}, {"CALCulate:LIMit:UPPer[:DATA]?", q_lupp},
    {"CALCulate:LIMit[:STATe]", c_lstat}, {"CALCulate:LIMit[:STATe]?", q_lstat},
    {"CALCulate:LIMit:FAIL?", q_lfail},
    {"[SENSe]:VOLTage:AVERage:COUNt", c_aver}, {"[SENSe]:VOLTage:AVERage:COUNt?", q_aver},
    {NULL, NULL}
};
