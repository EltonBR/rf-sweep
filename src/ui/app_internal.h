#ifndef RF_SWEEP_UI_APP_INTERNAL_H
#define RF_SWEEP_UI_APP_INTERNAL_H

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include "../core/rf_model.h"
#include "../core/rf_dsp.h"
#include "../core/rf_persistence.h"
#include <cairo.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *drawing_area;
    GtkWidget *device_combo;
    GtkWidget *args_entry;
    GtkWidget *min_spin;
    GtkWidget *max_spin;
    GtkWidget *sample_rate_combo;
    GtkWidget *gain_spin;
    GtkWidget *pixel_hz_spin;
    GtkWidget *average_combo;
    GtkWidget *name_entry;
    GtkWidget *scan_button;
    GtkWidget *stop_button;
    GtkWidget *save_button;
    GtkWidget *png_button;
    GtkWidget *import_button;
    GtkWidget *clear_button;
    GtkWidget *subtract_button;
    GtkWidget *fit_check;
    GtkWidget *progress_bar;
    GtkWidget *status_label;
    GtkWidget *scroll_window;

    GHashTable *trace_bins;
    GPtrArray *readings;
    Reading *last_scan_reading;
    GThread *scan_thread;
    GMutex state_lock;
    guint redraw_timer_id;

    double sweep_min_hz;
    double sweep_max_hz;
    double capture_sample_rate;
    double capture_gain_db;
    double capture_sweep_seconds;
    double pixel_hz;
    int average_window;
    guint64 total_pixels;
    int preview_width;
    gboolean has_trace;
    gboolean scanning;
    gboolean stop_requested;
    gboolean data_dirty;
    gboolean fit_to_window;
} App;

typedef struct {
    App *app;
    char *args;
    double min_hz;
    double max_hz;
    double sample_rate;
    double gain_db;
} ScanParams;

typedef struct {
    App *app;
    char *message;
    double progress;
    gboolean done;
    gboolean queue_draw;
} UiUpdate;

void set_status(App *app, const char *message);
void post_ui(App *app, const char *message, double progress, gboolean done, gboolean queue_draw);
gboolean should_stop(App *app);
void reset_trace(App *app);
Reading *create_reading_from_capture(App *app);
gpointer scan_thread_main(gpointer data);

void render_spectrum(App *app, cairo_t *cr, int width, int height, guint64 start_pixel, guint64 pixel_count, gboolean with_axes);
gboolean draw_cb(GtkWidget *widget, cairo_t *cr, gpointer user_data);
gboolean motion_cb(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean get_reading_db_at_hz(Reading *reading, double hz, int average_window, double *db);

void save_png(GtkButton *button, gpointer user_data);
void export_view_png(GtkButton *button, gpointer user_data);
void import_readings(GtkButton *button, gpointer user_data);
void subtract_readings(GtkButton *button, gpointer user_data);
void clear_readings(GtkButton *button, gpointer user_data);

#endif
