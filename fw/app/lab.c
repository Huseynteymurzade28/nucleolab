/* Instrument personality: AWG, scope, measurements. */
#include "lab.h"
#include "analog.h"
#include "board.h"
#include "dvm.h"
#include "spectrum.h"
#include "stm32l4xx.h"
#include <math.h>

/* ---------- AWG ---------- */
enum { F_SIN, F_SQU, F_TRI, F_RAMP, F_DC, F_ARB };
static const char *const fn_name[] = {"SINusoid", "SQUare", "TRIangle", "RAMP", "DC", "ARB"};
static const char *const fn_short[] = {"SIN", "SQU", "TRI", "RAMP", "DC", "ARB"};
static struct { int fn; float freq, actual, amp, offs, duty; bool on; } awg;
static uint16_t table[AWG_MAX], arb[AWG_MAX];
static uint32_t arb_n;

/* f = 80 MHz * k / (div * n): pick the k, div, n with the least frequency error */
static void awg_plan(float f, uint32_t *n, uint32_t *k, uint32_t *div)
{
    uint32_t d0 = (uint32_t)ceilf(TIMER_HZ / (f * (float)AWG_MAX));
    if (d0 < 80u) d0 = 80u;
    float best = 1e9f;
    *n = 2; *k = 1; *div = d0;
    for (uint32_t d = d0; d < d0 + 24u && d <= 65536u; d++) {
        float r = TIMER_HZ / ((float)d * f);
        for (uint32_t c = 1; c <= 256u; c++) {
            float x = r * (float)c;
            if (x > (float)AWG_MAX) break;
            uint32_t p = (uint32_t)(x + 0.5f);
            if (p < 2u) continue;
            float e = fabsf((float)p - x) / x;
            if (e < best) { best = e; *n = p; *k = c; *div = d; }
            if (e == 0) return;
        }
    }
}

static uint16_t to_code(float v)
{
    float c = v / vdda() * 4095.0f + 0.5f;
    return c < 0 ? 0 : c > 4095.0f ? 4095 : (uint16_t)c;
}

static void awg_apply(void)
{
    if (!awg.on) { dac_off(); return; }
    if (awg.fn == F_DC) { dac_dc(to_code(awg.offs)); return; }
    uint32_t n, k, div;
    dac_dc(to_code(awg.offs));              /* DMA idle while the table changes */
    awg_plan(awg.freq, &n, &k, &div);
    awg.actual = TIMER_HZ * (float)k / ((float)div * (float)n);
    for (uint32_t i = 0; i < n; i++) {
        float ph = (float)((i * k) % n) / (float)n, y;
        switch (awg.fn) {
        case F_SIN: y = sinf(6.2831853f * ph); break;
        case F_SQU: y = ph < awg.duty / 100.0f ? 1.0f : -1.0f; break;
        case F_TRI: y = ph < 0.25f ? 4 * ph : ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4; break;
        case F_RAMP: y = 2 * ph - 1; break;
        default: y = (float)arb[(uint32_t)(ph * (float)arb_n)] / 2047.5f - 1.0f;
        }
        table[i] = to_code(awg.offs + 0.5f * awg.amp * y);
    }
    dac_wave(table, n, div);
}

static bool kw(scpi_t *s, const char *const *names, int n, int *out)
{
    if (!scpi_nparams(s, 1, 1)) return false;
    for (int i = 0; i < n; i++)
        if (scpi_match_keyword(s->param[0], names[i])) { *out = i; return true; }
    scpi_error(s, -224);
    return false;
}
static bool num(scpi_t *s, const char *u, double lo, double hi, double def, double *x)
{
    return scpi_nparams(s, 1, 1) && scpi_number(s, 0, u, lo, hi, def, x);
}
static bool idle_args(scpi_t *s) { return scpi_nparams(s, 0, 0); }

static void c_func(scpi_t *s)
{
    int f;
    if (!kw(s, fn_name, 6, &f)) return;
    if (f == F_ARB && !arb_n) { scpi_error(s, -221); return; }
    awg.fn = f; awg_apply();
}
static void q_func(scpi_t *s) { scpi_out_text(s, fn_short[awg.fn]); }
static void c_freq(scpi_t *s)
{
    double x;
    if (num(s, "HZ", 1, 100e3, 1e3, &x)) { awg.freq = awg.actual = (float)x; awg_apply(); }
}
static void q_freq(scpi_t *s) { scpi_out_num(s, awg.actual); }
static bool fits(scpi_t *s, float amp, float offs)
{
    if (offs - amp / 2 >= -1e-4f && offs + amp / 2 <= vdda() + 1e-4f) return true;
    scpi_error(s, -221);
    return false;
}
static void c_amp(scpi_t *s)
{
    double x;
    if (num(s, "V", 0, 3.6, 2, &x) && fits(s, (float)x, awg.offs)) { awg.amp = (float)x; awg_apply(); }
}
static void q_amp(scpi_t *s) { scpi_out_num(s, awg.amp); }
static void c_offs(scpi_t *s)
{
    double x;
    if (num(s, "V", 0, 3.6, 1.65, &x) && fits(s, awg.fn == F_DC ? 0 : awg.amp, (float)x)) { awg.offs = (float)x; awg_apply(); }
}
static void q_offs(scpi_t *s) { scpi_out_num(s, awg.offs); }
static void c_duty(scpi_t *s)
{
    double x;
    if (num(s, "PCT", 1, 99, 50, &x)) { awg.duty = (float)x; awg_apply(); }
}
static void q_duty(scpi_t *s) { scpi_out_num(s, awg.duty); }
static void c_out(scpi_t *s)
{
    bool v;
    if (!scpi_nparams(s, 1, 1) || !scpi_bool(s, 0, &v)) return;
    /* DC mode may have moved the offset beyond what the amplitude allows */
    if (v && awg.fn != F_DC && !fits(s, awg.amp, awg.offs)) return;
    awg.on = v; awg_apply();
}
static void q_out(scpi_t *s) { scpi_out_int(s, awg.on); }
static void c_arb(scpi_t *s)
{
    const uint8_t *p; size_t n;
    if (!scpi_nparams(s, 1, 1) || !scpi_block(s, 0, &p, &n)) return;
    if (n < 4 || n % 2 || n > 2 * AWG_MAX) { scpi_error(s, -222); return; }
    for (size_t i = 0; i < n; i += 2)
        if (p[i + 1] > 15) { scpi_error(s, -222); return; }
    for (size_t i = 0; i < n; i += 2) arb[i / 2] = (uint16_t)(p[i] | p[i + 1] << 8);
    arb_n = (uint32_t)n / 2;
    if (awg.fn == F_ARB) awg_apply();
}
static void q_arbn(scpi_t *s) { scpi_out_int(s, (int32_t)arb_n); }

/* ---------- scope ---------- */
static struct { unsigned ch; uint32_t div, points; float level, pos; int slope, sweep; } sc;
static struct { bool valid; uint32_t points, pre; float dt, lsb; } cap;
static const char *const slopes[] = {"POSitive", "NEGative"}, *const sweeps[] = {"AUTO", "NORMal"};

static void start(bool autotrig)
{
    sc_cfg_t c = {sc.ch, sc.div, sc.points, (uint32_t)(sc.pos / 100.0f * (float)(sc.points - 1)),
                  to_code(sc.level), sc.slope == 0, autotrig};
    cap.valid = false;
    cap.points = c.points; cap.pre = c.pre;
    cap.dt = (float)sc.div / TIMER_HZ;
    cap.lsb = vdda() / 4096.0f;
    scope_start(&c);
}
static int wait_ms(uint32_t limit)
{
    uint32_t t0 = millis();
    while (scope_state() == SC_ARMED) {
        scope_poll();
        if (millis() - t0 > limit) { scope_abort(); return -214; }
    }
    if (scope_state() == SC_ERROR) return -240;
    cap.valid = scope_state() == SC_DONE;
    return 0;
}
int lab_wait(scpi_t *s) { (void)s; return scope_state() == SC_ARMED ? wait_ms(20000) : 0; }

static void busy_guard(void) { scope_abort(); }
static void c_ch(scpi_t *s) { double x; if (num(s, NULL, 1, 3, 1, &x)) sc.ch = (unsigned)x; }
static void q_ch(scpi_t *s) { scpi_out_int(s, (int32_t)sc.ch); }
static void c_sr(scpi_t *s) { double x; if (num(s, "HZ", 1e3, 1e6, 1e6, &x)) sc.div = (uint32_t)(80e6 / x + 0.5); }
static void q_sr(scpi_t *s) { scpi_out_num(s, TIMER_HZ / (float)sc.div); }
static void c_pts(scpi_t *s) { double x; if (num(s, NULL, 16, CAP_MAX, 1024, &x)) sc.points = (uint32_t)x; }
static void q_pts(scpi_t *s) { scpi_out_int(s, (int32_t)sc.points); }
static void c_lev(scpi_t *s) { double x; if (num(s, "V", 0, 3.6, 1.65, &x)) sc.level = (float)x; }
static void q_lev(scpi_t *s) { scpi_out_num(s, sc.level); }
static void c_pos(scpi_t *s) { double x; if (num(s, "PCT", 0, 100, 50, &x)) sc.pos = (float)x; }
static void q_pos(scpi_t *s) { scpi_out_num(s, sc.pos); }
static void c_slo(scpi_t *s) { int i; if (kw(s, slopes, 2, &i)) sc.slope = i; }
static void q_slo(scpi_t *s) { scpi_out_text(s, sc.slope ? "NEG" : "POS"); }
static void c_swe(scpi_t *s) { int i; if (kw(s, sweeps, 2, &i)) sc.sweep = i; }
static void q_swe(scpi_t *s) { scpi_out_text(s, sc.sweep ? "NORM" : "AUTO"); }
static void c_force(scpi_t *s) { if (idle_args(s)) { if (scope_state() == SC_ARMED) scope_force(); else scpi_error(s, -211); } }
static void c_init(scpi_t *s)
{
    if (!idle_args(s)) return;
    if (scope_state() == SC_ARMED) { scpi_error(s, -213); return; }
    start(sc.sweep == 0);
}
static void c_abor(scpi_t *s) { if (idle_args(s)) scope_abort(); }
static void q_state(scpi_t *s)
{
    static const char *const n[] = {"IDLE", "ARM", "DONE", "ERR"};
    scpi_out_text(s, n[scope_state()]);
}
static bool have_data(scpi_t *s)
{
    if (scope_state() == SC_DONE) cap.valid = true;
    if (!cap.valid) scpi_error(s, -230);
    return cap.valid;
}
/* points, seconds/sample, volts/code, trigger index, real trigger */
static void q_pre(scpi_t *s)
{
    if (!idle_args(s) || !have_data(s)) return;
    scpi_out_int(s, (int32_t)cap.points);
    scpi_out_num(s, cap.dt);
    scpi_out_num(s, cap.lsb);
    scpi_out_int(s, (int32_t)cap.pre);
    scpi_out_int(s, scope_triggered());
}
static void q_data(scpi_t *s)               /* uint16 little endian */
{
    if (!idle_args(s) || !have_data(s)) return;
    scpi_out_block(s, cap.points * 2u);
    scpi_out_raw(s, (const char *)scope_data, cap.points * 2u);
}

/* ---------- measurements: fresh capture with the current ACQ settings ---------- */
typedef struct { float min, max, mean, rms, ac, freq; } meas_t;
static bool measure(scpi_t *s, meas_t *m)
{
    if (!idle_args(s)) return false;
    if (scope_state() == SC_ARMED) { scpi_error(s, -213); return false; }
    start(true);
    int e = wait_ms(20000);
    if (e) { scpi_error(s, e); return false; }
    uint32_t n = cap.points, lo = 65535, hi = 0; uint64_t sum = 0, sq = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = scope_data[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v; sq += (uint64_t)v * v;
    }
    double mean = (double)sum / n, var = (double)sq / n - mean * mean;
    m->min = (float)lo * cap.lsb; m->max = (float)hi * cap.lsb;
    m->mean = (float)mean * cap.lsb;
    m->rms = sqrtf((float)((double)sq / n)) * cap.lsb;
    m->ac = var > 0 ? sqrtf((float)var) * cap.lsb : 0;
    /* interpolated rising crossings of mid level, with 10 % hysteresis */
    float mid = 0.5f * (float)(lo + hi), hy = 0.1f * (float)(hi - lo), first = 0, last = 0;
    int cnt = 0; bool armed = false;
    float cross = 0; bool pending = false;
    for (uint32_t i = 1; i < n; i++) {
        float a = scope_data[i - 1], b = scope_data[i];
        if (b < mid - hy) { armed = true; pending = false; }
        if (armed && !pending && a < mid && b >= mid) { cross = (float)(i - 1) + (mid - a) / (b - a); pending = true; }
        if (armed && pending && b > mid + hy) {
            if (!cnt) first = cross;
            last = cross; cnt++; armed = pending = false;
        }
    }
    m->freq = cnt > 1 && hi - lo > 40u ? (float)(cnt - 1) / ((last - first) * cap.dt) : NAN;
    return true;
}
#define MEAS(name, expr) static void name(scpi_t *s) { meas_t m; if (measure(s, &m)) scpi_out_num(s, expr); }
/* With a channel list it is the multi-channel DC voltmeter instead */
static void q_mdc(scpi_t *s) { meas_t m; if (s->nparam) dvm_measure(s); else if (measure(s, &m)) scpi_out_num(s, m.mean); }
MEAS(q_mac, m.ac) MEAS(q_mrms, m.rms) MEAS(q_mmin, m.min)
MEAS(q_mmax, m.max) MEAS(q_mpp, m.max - m.min) MEAS(q_mfreq, m.freq)

static bool adc_free(scpi_t *s)
{
    if (!idle_args(s)) return false;
    if (scope_state() == SC_ARMED) { scpi_error(s, -221); return false; }
    return true;
}
static void q_temp(scpi_t *s) { if (adc_free(s)) scpi_out_num(s, temp_measure()); }
static void q_vdda(scpi_t *s) { if (adc_free(s)) scpi_out_num(s, vdda_measure()); }
static void q_cal(scpi_t *s)
{
    scpi_out_int(s, vref_cal()); scpi_out_int(s, ts_cal1()); scpi_out_int(s, ts_cal2());
}
/* DAC steps read back on PA0 (jumper) and PA4 (internal): worst error in volts of each */
static void q_loop(scpi_t *s)
{
    if (!adc_free(s)) return;
    float worst[2] = {0, 0};
    for (int i = 0; i < 5; i++) {
        uint16_t code = (uint16_t)(600 + 700 * i);
        dac_dc(code);
        for (int c = 0; c < 2; c++) {
            float e = fabsf(adc_read_code(adc_hw_channel(c ? 3 : 1), 64) - (float)code) * vdda() / 4096.0f;
            if (e > worst[c]) worst[c] = e;
        }
    }
    awg_apply();
    scpi_out_num(s, worst[0]); scpi_out_num(s, worst[1]);
}

/* ---------- spectrum of the last capture ---------- */
static bool spec(scpi_t *s, spec_t *r)
{
    if (!idle_args(s) || !have_data(s)) return false;
    if (cap.points < 64) { scpi_error(s, -221); return false; }
    spectrum_compute(scope_data, cap.points, cap.dt, cap.lsb, r);
    return true;
}
static void q_spec(scpi_t *s)               /* block of int16 LE centi-dBV */
{
    spec_t r;
    if (!spec(s, &r)) return;
    scpi_out_block(s, r.bins * 2u);
    for (uint32_t i = 0; i < r.bins; i++) {
        int16_t v = spectrum_bin(&r, i);
        scpi_out_raw(s, (const char *)&v, 2);
    }
}
/* bins, bin width Hz, fundamental Hz, fundamental Vrms, THD dB, SNR dB, SINAD dB */
static void q_dist(scpi_t *s)
{
    spec_t r;
    if (!spec(s, &r)) return;
    scpi_out_int(s, (int32_t)r.bins); scpi_out_num(s, r.df); scpi_out_num(s, r.fund);
    scpi_out_num(s, r.vrms); scpi_out_num(s, r.thd); scpi_out_num(s, r.snr); scpi_out_num(s, r.sinad);
}

/* ---------- Bode: BODE:POINt? <freq>[,<channel>] -> Hz, dB, degrees, reference Vpp ---------- */
#define BPAIRS 4096u
/* DAC images must not alias within 12 bins of the tone */
static bool images_clear(float f, float fdac, float fadc)
{
    float guard = 12.0f * fadc / (float)BPAIRS;
    for (int m = 1; m <= 8; m++)
        for (int sg = -1; sg <= 1; sg += 2) {
            float a = fmodf((float)m * fdac + (float)sg * f, fadc);
            if (a > fadc / 2) a = fadc - a;
            if (fabsf(a - f) < guard) return false;
        }
    return true;
}
static void q_bode(scpi_t *s)
{
    double fd, chd = 1;
    if (!scpi_nparams(s, 1, 2) || !scpi_number(s, 0, "HZ", 10, 100e3, 1e3, &fd)) return;
    if (s->nparam > 1 && !scpi_number(s, 1, NULL, 1, 3, 1, &chd)) return;
    if (scope_state() == SC_ARMED) { scpi_error(s, -213); return; }
    uint32_t n, k, div;
    float f = (float)fd;
    float amp = 2.0f, offs = vdda() / 2;
    awg_plan(f, &n, &k, &div);
    f = TIMER_HZ * (float)k / ((float)div * (float)n);
    dac_dc(to_code(offs));
    for (uint32_t i = 0; i < n; i++)
        table[i] = to_code(offs + 0.5f * amp * sinf(6.2831853f * (float)((i * k) % n) / (float)n));
    dac_wave(table, n, div);
    uint32_t t0 = millis(), settle = 20u + (uint32_t)(2000.0f / f);
    while (millis() - t0 < settle) {}
    uint32_t ad = (uint32_t)(TIMER_HZ * 12.0f / (f * (float)BPAIRS));
    if (ad < 163u) ad = 163u;
    ad |= 1u;
    for (int i = 0; i < 60 && !images_clear(f, TIMER_HZ / (float)div, TIMER_HZ / (float)ad); i++) ad += 2;
    const uint16_t *p = dual_capture((unsigned)chd, ad, BPAIRS);
    if (!p) { awg_apply(); scpi_error(s, -240); return; }
    float ref, gain, ph;
    double turns = (double)k * ad / ((double)div * n);
    bode_phasor(p, BPAIRS, turns, &ref, &gain, &ph);
    awg_apply();
    ph = ph * 57.29578f - 360.0f * f * DUAL_SKEW / TIMER_HZ;
    while (ph > 180.0f) ph -= 360.0f;
    while (ph <= -180.0f) ph += 360.0f;
    scpi_out_num(s, f);
    scpi_out_num(s, gain > 1e-6f ? 20.0f * log10f(gain) : -120.0f);
    scpi_out_num(s, ph);
    scpi_out_num(s, 2.0f * ref * vdda() / 4096.0f);
}

void lab_reset(void)
{
    busy_guard();
    awg.fn = F_SIN; awg.freq = awg.actual = 1000; awg.amp = 2; awg.offs = 1.65f; awg.duty = 50; awg.on = false;
    arb_n = 0;
    sc.ch = 1; sc.div = 80; sc.points = 1024; sc.level = 1.65f; sc.pos = 50; sc.slope = sc.sweep = 0;
    cap.valid = false;
    awg_apply();
    vdda_measure();
}
void lab_init(void) { analog_init(); spectrum_init(); lab_reset(); dvm_init(); }
void lab_poll(scpi_t *s)
{
    scope_poll();
    bool armed = scope_state() == SC_ARMED;
    if ((s->oper.cond & 0x10) && !armed) s->oper.event |= 0x10;   /* measurement finished */
    s->oper.cond = armed ? 0x10 : 0;
    if (scope_state() == SC_ERROR) { scope_abort(); }
}
/* bit 0 not on PLL, bit 1 clock not crystal-locked, bit 2 VDDA out of range, bit 3 DAC->ADC internal */
int lab_selftest(void)
{
    int r = (clock_is_pll() ? 0 : 1) | (clock_is_lse_locked() ? 0 : 2);
    scope_abort();
    float v = vdda_measure();
    if (v < 3.0f || v > 3.6f) r |= 4;
    for (int i = 0; i < 5; i++) {
        uint16_t code = (uint16_t)(600 + 700 * i);
        dac_dc(code);
        if (fabsf(adc_read_code(adc_hw_channel(3), 64) - (float)code) > 40) r |= 8;
    }
    awg_apply();
    return r;
}

const scpi_cmd_t lab_cmds[] = {
    {"SOURce:FUNCtion[:SHAPe]", c_func}, {"SOURce:FUNCtion[:SHAPe]?", q_func},
    {"SOURce:FUNCtion:DCYCle", c_duty}, {"SOURce:FUNCtion:DCYCle?", q_duty},
    {"SOURce:FREQuency", c_freq}, {"SOURce:FREQuency?", q_freq},
    {"SOURce:VOLTage[:AMPLitude]", c_amp}, {"SOURce:VOLTage[:AMPLitude]?", q_amp},
    {"SOURce:VOLTage:OFFSet", c_offs}, {"SOURce:VOLTage:OFFSet?", q_offs},
    {"SOURce:DATA:ARB", c_arb}, {"SOURce:DATA:ARB:POINts?", q_arbn},
    {"OUTPut[:STATe]", c_out}, {"OUTPut[:STATe]?", q_out},
    {"ACQuire:CHANnel", c_ch}, {"ACQuire:CHANnel?", q_ch},
    {"ACQuire:SRATe", c_sr}, {"ACQuire:SRATe?", q_sr},
    {"ACQuire:POINts", c_pts}, {"ACQuire:POINts?", q_pts}, {"ACQuire:STATe?", q_state},
    {"TRIGger:LEVel", c_lev}, {"TRIGger:LEVel?", q_lev},
    {"TRIGger:SLOPe", c_slo}, {"TRIGger:SLOPe?", q_slo},
    {"TRIGger:SWEep", c_swe}, {"TRIGger:SWEep?", q_swe},
    {"TRIGger:POSition", c_pos}, {"TRIGger:POSition?", q_pos}, {"TRIGger:FORCe", c_force},
    {"INITiate[:IMMediate]", c_init}, {"ABORt", c_abor},
    {"WAVeform:PREamble?", q_pre}, {"WAVeform:DATA?", q_data},
    {"MEASure:VOLTage[:DC]?", q_mdc}, {"MEASure:VOLTage:AC?", q_mac},
    {"MEASure:VOLTage:RMS?", q_mrms}, {"MEASure:VOLTage:MINimum?", q_mmin},
    {"MEASure:VOLTage:MAXimum?", q_mmax}, {"MEASure:VOLTage:PTPeak?", q_mpp},
    {"MEASure:FREQuency?", q_mfreq}, {"MEASure:TEMPerature?", q_temp},
    {"CALCulate:SPECtrum?", q_spec}, {"CALCulate:DISTortion?", q_dist}, {"BODE:POINt?", q_bode},
    {"SYSTem:VDDA?", q_vdda}, {"DIAGnostic:CALibration?", q_cal}, {"DIAGnostic:LOOPback?", q_loop},
    {NULL, NULL}
};
