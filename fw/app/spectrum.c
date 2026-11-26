/* On-board FFT (THD / SNR / spectrum) and two-channel Bode measurement. */
#include "spectrum.h"
#include "analog.h"
#include <math.h>
#define NMAX 4096u
static float re[NMAX] __attribute__((section(".sram2")));
static float im[NMAX] __attribute__((section(".sram2")));
static float st[NMAX * 3 / 4];
static uint8_t mask[NMAX / 2 + 1];

void spectrum_init(void)
{
    for (uint32_t i = 0; i < NMAX * 3 / 4; i++) st[i] = sinf(6.2831853f * (float)i / (float)NMAX);
}
static float sin_t(uint32_t k) { return k < NMAX * 3 / 4 ? st[k] : -st[k - NMAX / 2]; }
static float cos_t(uint32_t k) { return sin_t((k + NMAX / 4) % NMAX); }

/* 4-term Blackman-Harris, u = position in 1/4096 of the record */
static float bh(uint32_t u)
{
    return 0.35875f - 0.48829f * cos_t(u % NMAX) + 0.14128f * cos_t(2 * u % NMAX) - 0.01168f * cos_t(3 * u % NMAX);
}

static void fft(uint32_t n)
{
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        uint32_t h = len / 2, step = NMAX / len;
        for (uint32_t k = 0; k < h; k++) {
            float c = cos_t(k * step), s = -sin_t(k * step);
            for (uint32_t i = k; i < n; i += len) {
                float x = re[i + h] * c - im[i + h] * s, y = re[i + h] * s + im[i + h] * c;
                re[i + h] = re[i] - x; im[i + h] = im[i] - y;
                re[i] += x; im[i] += y;
            }
        }
    }
}

void spectrum_compute(const uint16_t *x, uint32_t points, float dt, float lsb, spec_t *r)
{
    uint32_t n = NMAX;
    while (n > points) n >>= 1;
    float mean = 0, sw = 0, sw2 = 0;
    for (uint32_t i = 0; i < n; i++) mean += (float)x[i];
    mean /= (float)n;
    for (uint32_t i = 0; i < n; i++) {
        float w = bh(i * (NMAX / n));
        sw += w; sw2 += w * w;
        re[i] = ((float)x[i] - mean) * lsb * w;
        im[i] = 0;
    }
    fft(n);
    uint32_t half = n / 2, pk = 5;
    for (uint32_t i = 0; i < half; i++) {           /* re[] becomes |X|^2 */
        re[i] = re[i] * re[i] + im[i] * im[i];
        mask[i] = i < 5;
        if (i >= 5 && re[i] > re[pk]) pk = i;
    }
    mask[half] = 1;
    float ps = 2.0f / ((float)n * sw2), fund = 0, harm = 0, noise = 0, cen = 0;
    for (uint32_t i = pk - 4; i <= pk + 4 && i < half; i++) {
        if (!mask[i]) { fund += re[i]; cen += re[i] * (float)i; mask[i] = 1; }
    }
    float fb = fund > 0 ? cen / fund : (float)pk;
    for (int h = 2; h <= 9; h++) {
        float f = fmodf(fb * (float)h, (float)n);
        if (f > (float)half) f = (float)n - f;       /* aliased harmonic */
        int c = (int)(f + 0.5f);
        for (int i = c - 4; i <= c + 4; i++)
            if (i >= 0 && i < (int)half && !mask[i]) { harm += re[i]; mask[i] = 1; }
    }
    for (uint32_t i = 0; i < half; i++) if (!mask[i]) noise += re[i];
    r->bins = half;
    r->df = 1.0f / ((float)n * dt);
    r->fund = fb * r->df;
    r->vrms = sqrtf(fund * ps);
    r->thd = 10.0f * log10f((harm + 1e-20f) / (fund + 1e-20f));
    r->snr = 10.0f * log10f((fund + 1e-20f) / (noise + 1e-20f));
    r->sinad = 10.0f * log10f((fund + 1e-20f) / (noise + harm + 1e-20f));
    r->scale = 2.0f / (sw * sw);
    /* More energy next to DC than at the peak: record too short to resolve the tone. */
    for (uint32_t i = 1; i < 5; i++)
        if (re[i] > re[pk]) { r->fund = r->vrms = r->thd = r->snr = r->sinad = NAN; break; }
}
/* centi-dBV, reuses the last spectrum_compute() result */
int16_t spectrum_bin(const spec_t *r, uint32_t i)
{
    float p = re[i] * r->scale;
    return (int16_t)(1000.0f * log10f(p > 1e-18f ? p : 1e-18f));
}

/* interleaved reference/response pairs; turns = cycles of the test tone per pair */
void bode_phasor(const uint16_t *p, uint32_t pairs, double turns, float *ref, float *gain, float *phase)
{
    uint32_t inc = (uint32_t)(turns * 4294967296.0), ph = 0;
    float m[2] = {0, 0}, ar[2] = {0, 0}, ai[2] = {0, 0}, sw = 0;
    for (uint32_t i = 0; i < 2 * pairs; i++) m[i & 1] += p[i];
    m[0] /= (float)pairs; m[1] /= (float)pairs;
    for (uint32_t i = 0; i < pairs; i++, ph += inc) {
        float w = bh(i * NMAX / pairs), a = (float)ph * 1.4629180e-9f; /* 2 pi / 2^32 */
        float c = cosf(a) * w, s = sinf(a) * w;
        sw += w;
        for (int k = 0; k < 2; k++) {
            float v = (float)p[2 * i + k] - m[k];
            ar[k] += v * c; ai[k] -= v * s;
        }
    }
    float a0 = sqrtf(ar[0] * ar[0] + ai[0] * ai[0]), a1 = sqrtf(ar[1] * ar[1] + ai[1] * ai[1]);
    *ref = 2.0f * a0 / sw;                           /* peak, in codes */
    *gain = a1 / (a0 + 1e-9f);
    *phase = atan2f(ai[1], ar[1]) - atan2f(ai[0], ar[0]);
}
