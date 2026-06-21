#ifndef RF_SWEEP_CORE_RF_DSP_H
#define RF_SWEEP_CORE_RF_DSP_H

#include "rf_model.h"

void fft_inplace(Complex *buf, unsigned n);
void spectrum_power_from_cf32(const float *samples,
                              unsigned n,
                              double sample_rate,
                              double *offsets_hz,
                              double *powers,
                              gboolean *usable);

#endif
