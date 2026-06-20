#include "rf_model.h"

#include <math.h>

const DevicePreset PRESETS[] = {
    {"RTL-SDR", "driver=rtlsdr", 24.0, 1766.0, 0.25, 3.2, 2.4, 0.0, 50.0, 35.0},
    {"HackRF One", "driver=hackrf", 1.0, 6000.0, 2.0, 20.0, 10.0, 0.0, 76.0, 32.0},
    {"Mirics/SDRplay", "driver=miri", 0.15, 1900.0, 0.25, 10.0, 2.4, 0.0, 50.0, 35.0},
    {"SoapySDR personalizado", "", 24.0, 1766.0, 0.25, 20.0, 2.4, 0.0, 80.0, 35.0},
};

const guint PRESETS_LEN = G_N_ELEMENTS(PRESETS);

const int AVERAGE_WINDOWS[] = {1, 3, 5, 9, 15, 31};
const guint AVERAGE_WINDOWS_LEN = G_N_ELEMENTS(AVERAGE_WINDOWS);

static const double COLOR_HUES[] = {0.00, 0.58, 0.13, 0.78, 0.33, 0.92, 0.50, 0.08, 0.70, 0.25};

void reading_free(gpointer data)
{
    Reading *reading = data;
    if (!reading) {
        return;
    }

    g_free(reading->name);
    g_free(reading->device_args);
    if (reading->bins) {
        g_hash_table_destroy(reading->bins);
    }
    g_free(reading);
}

GdkRGBA auto_color_for_index(guint index)
{
    double h = COLOR_HUES[index % G_N_ELEMENTS(COLOR_HUES)];
    double s = 0.82;
    double v = 0.96;
    double c = v * s;
    double hp = h * 6.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    if (hp < 1.0) {
        r = c; g = x;
    } else if (hp < 2.0) {
        r = x; g = c;
    } else if (hp < 3.0) {
        g = c; b = x;
    } else if (hp < 4.0) {
        g = x; b = c;
    } else if (hp < 5.0) {
        r = x; b = c;
    } else {
        r = c; b = x;
    }

    double m = v - c;
    GdkRGBA color = {r + m, g + m, b + m, 1.0};
    return color;
}

guint uint64_hash(gconstpointer value)
{
    guint64 v = *(const guint64 *)value;
    return (guint)(v ^ (v >> 32));
}

gboolean uint64_equal(gconstpointer a, gconstpointer b)
{
    return *(const guint64 *)a == *(const guint64 *)b;
}

guint64 compute_total_pixels(double min_hz, double max_hz, double pixel_hz)
{
    if (max_hz <= min_hz) {
        return 1;
    }
    return (guint64)ceil((max_hz - min_hz) / pixel_hz) + 1;
}

gboolean get_reading_bin_db(Reading *reading, guint64 pixel, double *db)
{
    TraceBin *bin = g_hash_table_lookup(reading->bins, &pixel);
    if (!bin || bin->count == 0) {
        return FALSE;
    }

    *db = bin->sum_db / bin->count;
    return TRUE;
}

gboolean get_reading_average_db(Reading *reading, guint64 pixel, int window, double *db)
{
    if (window <= 1) {
        return get_reading_bin_db(reading, pixel, db);
    }

    guint64 half = (guint64)(window / 2);
    guint64 start = pixel > half ? pixel - half : 0;
    guint64 end = MIN(reading->total_pixels - 1, pixel + half);
    double sum = 0.0;
    guint count = 0;

    for (guint64 p = start; p <= end; p++) {
        double raw_db;
        if (get_reading_bin_db(reading, p, &raw_db)) {
            sum += raw_db;
            count++;
        }
    }

    if (count == 0) {
        return FALSE;
    }

    *db = sum / count;
    return TRUE;
}
