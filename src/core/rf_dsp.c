#include "rf_dsp.h"

#include <math.h>

static unsigned reverse_bits(unsigned x, unsigned bits)
{
    unsigned y = 0;
    for (unsigned i = 0; i < bits; i++) {
        y = (y << 1) | (x & 1U);
        x >>= 1U;
    }
    return y;
}

void fft_inplace(Complex *buf, unsigned n)
{
    unsigned bits = 0;
    while ((1U << bits) < n) {
        bits++;
    }

    for (unsigned i = 0; i < n; i++) {
        unsigned j = reverse_bits(i, bits);
        if (j > i) {
            Complex tmp = buf[i];
            buf[i] = buf[j];
            buf[j] = tmp;
        }
    }

    for (unsigned len = 2; len <= n; len <<= 1U) {
        double angle = -2.0 * G_PI / (double)len;
        float wr_step = (float)cos(angle);
        float wi_step = (float)sin(angle);

        for (unsigned i = 0; i < n; i += len) {
            float wr = 1.0f;
            float wi = 0.0f;
            for (unsigned j = 0; j < len / 2; j++) {
                Complex u = buf[i + j];
                Complex v = buf[i + j + len / 2];
                float tr = wr * v.r - wi * v.i;
                float ti = wr * v.i + wi * v.r;

                buf[i + j].r = u.r + tr;
                buf[i + j].i = u.i + ti;
                buf[i + j + len / 2].r = u.r - tr;
                buf[i + j + len / 2].i = u.i - ti;

                float next_wr = wr * wr_step - wi * wi_step;
                wi = wr * wi_step + wi * wr_step;
                wr = next_wr;
            }
        }
    }
}
