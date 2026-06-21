#ifndef RF_SWEEP_CORE_RF_MODEL_H
#define RF_SWEEP_CORE_RF_MODEL_H

#include <gtk/gtk.h>
#include <stdint.h>

#define CHUNK_WIDTH 7680
#define DEFAULT_PIXEL_HZ 1000.0
#define PREVIEW_MAX_WIDTH 38400
#define PNG_EXPORT_WIDTH 23040
#define GRAPH_HEIGHT 720
#define FFT_SIZE 4096
#define FFTS_PER_STEP 3
#define TUNE_SETTLE_US 120000
#define DISCARD_SECONDS_AFTER_TUNE 0.20
#define FFT_STEP_FRACTION 0.50
#define FFT_USABLE_FRACTION 1.00
#define FFT_DC_NOTCH_BINS 8
#define DEFAULT_DB_MIN -120.0
#define DEFAULT_DB_MAX 0.0

typedef struct {
    float r;
    float i;
} Complex;

typedef struct {
    double sum_power;
    double peak_power;
    guint count;
} TraceBin;

typedef struct {
    char *name;
    char *device_args;
    GHashTable *bins;
    double sweep_min_hz;
    double sweep_max_hz;
    double sample_rate;
    double gain_db;
    double sweep_seconds;
    double pixel_hz;
    guint64 total_pixels;
    GdkRGBA color;
} Reading;

typedef struct {
    const char *name;
    const char *args;
    double min_mhz;
    double max_mhz;
    double sample_rate_min_msps;
    double sample_rate_max_msps;
    double sample_rate_msps;
    const double *sample_rates_msps;
    guint sample_rates_len;
    double gain_min_db;
    double gain_max_db;
    double gain_db;
} DevicePreset;

extern const DevicePreset PRESETS[];
extern const guint PRESETS_LEN;
extern const int AVERAGE_WINDOWS[];
extern const guint AVERAGE_WINDOWS_LEN;

void reading_free(gpointer data);
GdkRGBA auto_color_for_index(guint index);
guint uint64_hash(gconstpointer value);
gboolean uint64_equal(gconstpointer a, gconstpointer b);
guint64 compute_total_pixels(double min_hz, double max_hz, double pixel_hz);
gboolean get_reading_bin_db(Reading *reading, guint64 pixel, double *db);
gboolean get_reading_average_db(Reading *reading, guint64 pixel, int window, double *db);

#endif
