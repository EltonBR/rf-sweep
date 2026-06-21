#include "rf_dsp.h"

#include <math.h>
#include <stdlib.h>

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

void spectrum_power_from_cf32(const float *samples,
                              unsigned n,
                              double sample_rate,
                              double *offsets_hz,
                              double *powers,
                              gboolean *usable)
{
    Complex fft[FFT_SIZE];
    double mean_i = 0.0;
    double mean_q = 0.0;
    double coherent_gain = 0.0;

    if (n != FFT_SIZE) {
        return;
    }

    for (unsigned i = 0; i < n; i++) {
        mean_i += samples[2 * i];
        mean_q += samples[2 * i + 1];
    }
    mean_i /= n;
    mean_q /= n;

    for (unsigned i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * G_PI * (double)i / (double)(n - 1));
        fft[i].r = (float)((samples[2 * i] - mean_i) * w);
        fft[i].i = (float)((samples[2 * i + 1] - mean_q) * w);
        coherent_gain += w;
    }

    fft_inplace(fft, n);

    const unsigned edge_bins = (unsigned)floor(((1.0 - FFT_USABLE_FRACTION) * 0.5) * n);
    const unsigned center_bin = n / 2;

    for (unsigned i = 0; i < n; i++) {
        unsigned shifted = (i + center_bin) % n;
        int centered = (int)i - (int)center_bin;
        double offset = centered * (sample_rate / (double)n);
        double i_power = fft[shifted].r * fft[shifted].r + fft[shifted].i * fft[shifted].i;
        double norm_power = i_power / (coherent_gain * coherent_gain + 1e-30);
        gboolean in_edge = edge_bins > 0 && (i < edge_bins || i >= n - edge_bins);
        gboolean in_dc = FFT_DC_NOTCH_BINS > 0 && abs(centered) <= FFT_DC_NOTCH_BINS;

        offsets_hz[i] = offset;
        powers[i] = norm_power;
        usable[i] = !in_edge && !in_dc;
    }
}
