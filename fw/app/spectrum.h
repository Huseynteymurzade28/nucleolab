#ifndef SPECTRUM_H
#define SPECTRUM_H
#include <stdint.h>
typedef struct { uint32_t bins; float df, fund, vrms, thd, snr, sinad, scale; } spec_t;
void spectrum_init(void);
void spectrum_compute(const uint16_t *x, uint32_t points, float dt, float lsb, spec_t *r);
int16_t spectrum_bin(const spec_t *r, uint32_t i);
void bode_phasor(const uint16_t *p, uint32_t pairs, double turns, float *ref, float *gain, float *phase);
#endif
