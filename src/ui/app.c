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
#include <locale.h>
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
    GtkWidget *sample_rate_spin;
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
    GtkWidget *progress_bar;
    GtkWidget *status_label;

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

static Reading *create_reading_from_capture(App *app);

static void set_status(App *app, const char *message)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), message);
}

static gboolean ui_update_cb(gpointer data)
{
    UiUpdate *update = data;
    App *app = update->app;

    if (update->message) {
        set_status(app, update->message);
    }
    if (update->progress >= 0.0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar),
                                      fmax(0.0, fmin(1.0, update->progress)));
    }
    if (update->queue_draw) {
        app->data_dirty = TRUE;
    }
    if (update->done) {
        g_mutex_lock(&app->state_lock);
        app->scanning = FALSE;
        app->stop_requested = FALSE;
        if (app->scan_thread) {
            g_thread_unref(app->scan_thread);
        }
        app->scan_thread = NULL;
        g_mutex_unlock(&app->state_lock);

        if (app->has_trace && app->readings) {
            Reading *reading = create_reading_from_capture(app);
            if (reading) {
                g_ptr_array_add(app->readings, reading);
                app->last_scan_reading = reading;
                char *next_name = g_strdup_printf("Leitura %u", app->readings->len + 1);
                gtk_entry_set_text(GTK_ENTRY(app->name_entry), next_name);
                g_free(next_name);
            }
        }

        gtk_widget_set_sensitive(app->scan_button, TRUE);
        gtk_widget_set_sensitive(app->stop_button, FALSE);
        gtk_widget_queue_draw(app->drawing_area);
    }

    g_free(update->message);
    g_free(update);
    return G_SOURCE_REMOVE;
}

static void post_ui(App *app, const char *message, double progress, gboolean done, gboolean queue_draw)
{
    UiUpdate *update = g_new0(UiUpdate, 1);
    update->app = app;
    update->message = message ? g_strdup(message) : NULL;
    update->progress = progress;
    update->done = done;
    update->queue_draw = queue_draw;
    g_idle_add(ui_update_cb, update);
}

static gboolean redraw_timer_cb(gpointer user_data)
{
    App *app = user_data;

    if (app->data_dirty) {
        app->data_dirty = FALSE;
        gtk_widget_queue_draw(app->drawing_area);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean should_stop(App *app)
{
    gboolean stop;
    g_mutex_lock(&app->state_lock);
    stop = app->stop_requested;
    g_mutex_unlock(&app->state_lock);
    return stop;
}

static void reset_trace(App *app)
{
    if (!app->trace_bins) {
        app->trace_bins = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_free);
    } else {
        g_hash_table_remove_all(app->trace_bins);
    }

    app->total_pixels = compute_total_pixels(app->sweep_min_hz, app->sweep_max_hz, app->pixel_hz);
    app->preview_width = (int)MIN((guint64)PREVIEW_MAX_WIDTH, app->total_pixels);

    app->has_trace = FALSE;
    app->data_dirty = TRUE;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    gtk_widget_set_size_request(app->drawing_area, app->preview_width, GRAPH_HEIGHT);
    gtk_widget_queue_draw(app->drawing_area);
}

static Reading *create_reading_from_capture(App *app)
{
    if (!app->has_trace || !app->trace_bins || g_hash_table_size(app->trace_bins) == 0) {
        return NULL;
    }

    Reading *reading = g_new0(Reading, 1);
    const char *name = gtk_entry_get_text(GTK_ENTRY(app->name_entry));
    const char *args = gtk_entry_get_text(GTK_ENTRY(app->args_entry));
    guint color_index = app->readings ? app->readings->len : 0;

    reading->name = g_strdup(name && name[0] ? name : "Leitura");
    reading->device_args = g_strdup(args ? args : "");
    reading->sweep_min_hz = app->sweep_min_hz;
    reading->sweep_max_hz = app->sweep_max_hz;
    reading->sample_rate = app->capture_sample_rate;
    reading->gain_db = app->capture_gain_db;
    reading->sweep_seconds = app->capture_sweep_seconds;
    reading->pixel_hz = app->pixel_hz;
    reading->total_pixels = app->total_pixels;
    reading->color = auto_color_for_index(color_index);
    reading->bins = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_free);

    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_mutex_lock(&app->state_lock);
    g_hash_table_iter_init(&iter, app->trace_bins);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint64 *new_key = g_new(guint64, 1);
        TraceBin *new_bin = g_new(TraceBin, 1);
        *new_key = *(guint64 *)key;
        *new_bin = *(TraceBin *)value;
        g_hash_table_insert(reading->bins, new_key, new_bin);
    }
    g_mutex_unlock(&app->state_lock);

    return reading;
}

static void add_power_band(App *app, double freq_hz, double bandwidth_hz, double db)
{
    double low_hz = freq_hz - bandwidth_hz * 0.5;
    double high_hz = freq_hz + bandwidth_hz * 0.5;
    if (high_hz < app->sweep_min_hz || low_hz > app->sweep_max_hz) {
        return;
    }

    low_hz = fmax(low_hz, app->sweep_min_hz);
    high_hz = fmin(high_hz, app->sweep_max_hz);
    guint64 x1 = (guint64)floor((low_hz - app->sweep_min_hz) / app->pixel_hz);
    guint64 x2 = (guint64)ceil((high_hz - app->sweep_min_hz) / app->pixel_hz);
    x1 = MIN(x1, app->total_pixels - 1);
    x2 = MIN(MAX(x1 + 1, x2), app->total_pixels);

    g_mutex_lock(&app->state_lock);
    for (guint64 x = x1; x < x2; x++) {
        TraceBin *bin = g_hash_table_lookup(app->trace_bins, &x);
        if (!bin) {
            guint64 *key = g_new(guint64, 1);
            *key = x;
            bin = g_new0(TraceBin, 1);
            g_hash_table_insert(app->trace_bins, key, bin);
        }
        bin->sum_db += db;
        bin->count++;
    }
    app->has_trace = TRUE;
    g_mutex_unlock(&app->state_lock);
}

static void process_fft(App *app, const float *samples, double center_hz, double sample_rate)
{
    Complex fft[FFT_SIZE];

    for (int i = 0; i < FFT_SIZE; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * G_PI * (double)i / (FFT_SIZE - 1));
        fft[i].r = (float)(samples[2 * i] * w);
        fft[i].i = (float)(samples[2 * i + 1] * w);
    }

    fft_inplace(fft, FFT_SIZE);

    for (int i = 0; i < FFT_SIZE; i++) {
        int shifted = (i + FFT_SIZE / 2) % FFT_SIZE;
        double offset = ((double)i / (FFT_SIZE - 1) - 0.5) * sample_rate;
        double freq_hz = center_hz + offset;
        double p = fft[shifted].r * fft[shifted].r + fft[shifted].i * fft[shifted].i;
        double db = 10.0 * log10(p / FFT_SIZE + 1e-20);
        add_power_band(app, freq_hz, sample_rate / FFT_SIZE, db);
    }
}

static gpointer scan_thread_main(gpointer data)
{
    ScanParams *params = data;
    App *app = params->app;
    SoapySDRDevice *device = NULL;
    SoapySDRStream *stream = NULL;
    float *samples = NULL;
    char *message = NULL;
    gint64 sweep_start_us = g_get_monotonic_time();

    device = SoapySDRDevice_makeStrArgs(params->args);
    if (!device) {
        message = g_strdup_printf("Erro SoapySDR: %s", SoapySDRDevice_lastError());
        goto done;
    }

    int rc = SoapySDRDevice_setSampleRate(device, SOAPY_SDR_RX, 0, params->sample_rate);
    if (rc != 0) {
        message = g_strdup_printf("Falha ao configurar sample rate: %s", SoapySDRDevice_lastError());
        goto done;
    }
    SoapySDRDevice_setGainMode(device, SOAPY_SDR_RX, 0, false);
    SoapySDRDevice_setGain(device, SOAPY_SDR_RX, 0, params->gain_db);

    size_t channel = 0;
    stream = SoapySDRDevice_setupStream(device, SOAPY_SDR_RX, SOAPY_SDR_CF32, &channel, 1, NULL);
    if (!stream) {
        message = g_strdup_printf("Falha no stream SoapySDR: %s", SoapySDRDevice_lastError());
        goto done;
    }

    rc = SoapySDRDevice_activateStream(device, stream, 0, 0, 0);
    if (rc != 0) {
        message = g_strdup_printf("Falha ao ativar stream: %s", SoapySDRDevice_lastError());
        goto done;
    }

    samples = g_new0(float, FFT_SIZE * 2);
    double step_hz = params->sample_rate * 0.72;
    int total_steps = MAX(1, (int)ceil((params->max_hz - params->min_hz) / step_hz));
    gint64 last_ui_update = 0;

    for (int step = 0; step < total_steps && !should_stop(app); step++) {
        double center_hz;
        if ((params->max_hz - params->min_hz) <= params->sample_rate) {
            center_hz = (params->min_hz + params->max_hz) * 0.5;
        } else {
            center_hz = params->min_hz + step_hz * step + params->sample_rate * 0.5;
            center_hz = MIN(center_hz, params->max_hz - params->sample_rate * 0.5);
            center_hz = MAX(center_hz, params->min_hz + params->sample_rate * 0.5);
        }

        rc = SoapySDRDevice_setFrequency(device, SOAPY_SDR_RX, 0, center_hz, NULL);
        if (rc != 0) {
            message = g_strdup_printf("Falha ao sintonizar %.3f MHz: %s",
                                      center_hz / 1000000.0,
                                      SoapySDRDevice_lastError());
            goto done;
        }

        usleep(45000);

        size_t got = 0;
        while (got < FFT_SIZE && !should_stop(app)) {
            void *buffs[] = {samples + got * 2};
            int flags = 0;
            long long time_ns = 0;
            rc = SoapySDRDevice_readStream(device, stream, buffs, FFT_SIZE - got,
                                           &flags, &time_ns, 300000);
            if (rc == SOAPY_SDR_TIMEOUT || rc == SOAPY_SDR_OVERFLOW) {
                continue;
            }
            if (rc < 0) {
                message = g_strdup_printf("Erro lendo amostras: %s", SoapySDRDevice_lastError());
                goto done;
            }
            got += (size_t)rc;
        }

        if (got == FFT_SIZE) {
            process_fft(app, samples, center_hz, params->sample_rate);
        }

        gint64 now = g_get_monotonic_time();
        if (step + 1 == total_steps || now - last_ui_update >= 250000) {
            double progress = (double)(step + 1) / total_steps;
            char status[160];
            g_snprintf(status, sizeof(status), "Capturando %.1f / %.1f MHz (%d/%d)",
                       center_hz / 1000000.0, params->max_hz / 1000000.0,
                       step + 1, total_steps);
            post_ui(app, status, progress, FALSE, FALSE);
            last_ui_update = now;
        }
    }

    if (!message) {
        message = g_strdup(should_stop(app) ? "Scan parado." : "Scan finalizado.");
    }

done:
    if (stream) {
        SoapySDRDevice_deactivateStream(device, stream, 0, 0);
        SoapySDRDevice_closeStream(device, stream);
    }
    if (device) {
        SoapySDRDevice_unmake(device);
    }
    g_free(samples);

    g_mutex_lock(&app->state_lock);
    app->capture_sweep_seconds = (g_get_monotonic_time() - sweep_start_us) / 1000000.0;
    g_mutex_unlock(&app->state_lock);

    post_ui(app, message, should_stop(app) ? -1.0 : 1.0, TRUE, TRUE);
    g_free(message);
    g_free(params->args);
    g_free(params);
    return NULL;
}

static double graph_y(double db, double top, double plot_h, double db_min, double db_max)
{
    double clamped = fmax(db_min, fmin(db_max, db));
    double t = (clamped - db_min) / (db_max - db_min);
    return top + plot_h - (t * plot_h);
}

static gboolean get_bin_db_locked(App *app, guint64 pixel, double *db)
{
    TraceBin *bin = g_hash_table_lookup(app->trace_bins, &pixel);
    if (!bin || bin->count == 0) {
        return FALSE;
    }

    *db = bin->sum_db / bin->count;
    return TRUE;
}

static gboolean get_moving_average_db_locked(App *app, guint64 pixel, int window, double *db)
{
    if (window <= 1) {
        return get_bin_db_locked(app, pixel, db);
    }

    guint64 half = (guint64)(window / 2);
    guint64 start = pixel > half ? pixel - half : 0;
    guint64 end = MIN(app->total_pixels - 1, pixel + half);
    double sum = 0.0;
    guint count = 0;

    for (guint64 p = start; p <= end; p++) {
        double raw_db;
        if (get_bin_db_locked(app, p, &raw_db)) {
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

static void get_view_range(App *app, double *min_hz, double *max_hz)
{
    gboolean found = FALSE;
    *min_hz = app->sweep_min_hz > 0.0 ? app->sweep_min_hz : 24000000.0;
    *max_hz = app->sweep_max_hz > 0.0 ? app->sweep_max_hz : 1766000000.0;

    if (!app->readings) {
        return;
    }

    for (guint i = 0; i < app->readings->len; i++) {
        Reading *reading = g_ptr_array_index(app->readings, i);
        if (!found) {
            *min_hz = reading->sweep_min_hz;
            *max_hz = reading->sweep_max_hz;
            found = TRUE;
        } else {
            *min_hz = fmin(*min_hz, reading->sweep_min_hz);
            *max_hz = fmax(*max_hz, reading->sweep_max_hz);
        }
    }
}

static void get_signal_range(App *app, double *db_min, double *db_max)
{
    gboolean found = FALSE;
    *db_min = DEFAULT_DB_MIN;
    *db_max = DEFAULT_DB_MAX;

    if (app->readings) {
        for (guint r = 0; r < app->readings->len; r++) {
            Reading *reading = g_ptr_array_index(app->readings, r);
            GHashTableIter iter;
            gpointer key;
            gpointer value;
            g_hash_table_iter_init(&iter, reading->bins);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                double db;
                if (!get_reading_average_db(reading, *(guint64 *)key, app->average_window, &db)) {
                    continue;
                }
                if (!found) {
                    *db_min = db;
                    *db_max = db;
                    found = TRUE;
                } else {
                    *db_min = fmin(*db_min, db);
                    *db_max = fmax(*db_max, db);
                }
            }
        }
    }

    if (!found) {
        return;
    }

    double span = *db_max - *db_min;
    double margin = fmax(2.0, span * 0.08);
    if (span < 1.0) {
        double mid = (*db_min + *db_max) * 0.5;
        *db_min = mid - 5.0;
        *db_max = mid + 5.0;
    } else {
        *db_min -= margin;
        *db_max += margin;
    }
}

static void render_spectrum(App *app,
                            cairo_t *cr,
                            int width,
                            int height,
                            guint64 start_pixel,
                            guint64 pixel_count,
                            gboolean with_axes)
{
    (void)start_pixel;
    (void)pixel_count;
    const double left = 104.0;
    const double right = 22.0;
    const double top = 24.0;
    const double bottom = 54.0;
    double margin_left = with_axes ? left : 0.0;
    double margin_right = with_axes ? right : 0.0;
    double margin_top = with_axes ? top : 0.0;
    double margin_bottom = with_axes ? bottom : 0.0;
    double plot_w = MAX(10.0, width - margin_left - margin_right);
    double plot_h = MAX(10.0, height - margin_top - margin_bottom);
    double min_hz;
    double max_hz;
    get_view_range(app, &min_hz, &max_hz);
    double db_min;
    double db_max;
    get_signal_range(app, &db_min, &db_max);

    cairo_set_source_rgb(cr, 0.015, 0.018, 0.024);
    cairo_paint(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);

    cairo_set_source_rgb(cr, 0.025, 0.031, 0.040);
    cairo_rectangle(cr, margin_left, margin_top, plot_w, plot_h);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.0);
    int freq_divisions = with_axes ? MIN(200, MAX(16, width / 90)) : 16;
    for (int i = 0; i <= freq_divisions; i++) {
        double x = margin_left + plot_w * (double)i / freq_divisions;
        double hz = min_hz + (max_hz - min_hz) * (double)i / freq_divisions;
        double mhz = hz / 1000000.0;
        char label[32];

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_move_to(cr, x, margin_top);
        cairo_line_to(cr, x, margin_top + plot_h);
        cairo_stroke(cr);

        if (with_axes) {
            g_snprintf(label, sizeof(label), "%.3f", mhz);
            cairo_set_source_rgba(cr, 0.88, 0.91, 0.94, 0.82);
            cairo_move_to(cr, x - 22, margin_top + plot_h + 22);
            cairo_show_text(cr, label);
        }
    }

    for (int i = 0; i <= 10; i++) {
        double db = db_min + (db_max - db_min) * (double)i / 10.0;
        double y = graph_y(db, margin_top, plot_h, db_min, db_max);
        char label[32];
        g_snprintf(label, sizeof(label), "%.0f dB", db);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        cairo_move_to(cr, margin_left, y);
        cairo_line_to(cr, margin_left + plot_w, y);
        cairo_stroke(cr);

        if (with_axes) {
            cairo_set_source_rgba(cr, 0.88, 0.91, 0.94, 0.82);
            cairo_move_to(cr, 42, y + 4);
            cairo_show_text(cr, label);
        }
    }

    cairo_set_source_rgba(cr, 0.80, 0.86, 0.92, 0.72);
    cairo_rectangle(cr, margin_left, margin_top, plot_w, plot_h);
    cairo_stroke(cr);

    if (with_axes) {
        cairo_set_source_rgba(cr, 0.88, 0.91, 0.94, 0.90);
        cairo_move_to(cr, margin_left + plot_w * 0.5 - 52, height - 15);
        cairo_show_text(cr, "Frequencia (MHz)");
    }

    if (!app->readings || app->readings->len == 0) {
        cairo_set_source_rgba(cr, 0.88, 0.91, 0.94, 0.52);
        cairo_move_to(cr, margin_left + 18, margin_top + 28);
        cairo_show_text(cr, "Sem leituras. Clique em Scan ou Importar.");
        return;
    }

    cairo_set_line_width(cr, 1.4);
    for (guint r = 0; r < app->readings->len; r++) {
        Reading *reading = g_ptr_array_index(app->readings, r);
        cairo_set_source_rgba(cr, reading->color.red, reading->color.green, reading->color.blue, 0.92);
        gboolean started = FALSE;
        for (int i = 0; i < width; i++) {
            double hz = min_hz + (max_hz - min_hz) * (double)i / MAX(1, width - 1);
            if (hz < reading->sweep_min_hz || hz > reading->sweep_max_hz) {
                started = FALSE;
                continue;
            }

            guint64 pixel = (guint64)floor((hz - reading->sweep_min_hz) / reading->pixel_hz);
            pixel = MIN(pixel, reading->total_pixels - 1);
            double db;
            if (!get_reading_average_db(reading, pixel, app->average_window, &db)) {
                started = FALSE;
                continue;
            }

            double x = margin_left + plot_w * (double)i / MAX(1, width - 1);
            double y = graph_y(db, margin_top, plot_h, db_min, db_max);
            if (!started) {
                cairo_move_to(cr, x, y);
                started = TRUE;
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_stroke(cr);
    }

    if (with_axes) {
        double legend_x = margin_left + 14.0;
        double legend_y = margin_top + 18.0;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);

        for (guint r = 0; r < app->readings->len; r++) {
            Reading *reading = g_ptr_array_index(app->readings, r);
            double y = legend_y + r * 18.0;
            char *legend = g_strdup_printf("%s (%s: %.3f MHz - %.3f MHz, %.2f MS/s, %.1f dB, %.3f s)",
                                           reading->name,
                                           reading->device_args && reading->device_args[0] ? reading->device_args : "device",
                                           reading->sweep_min_hz / 1000000.0,
                                           reading->sweep_max_hz / 1000000.0,
                                           reading->sample_rate / 1000000.0,
                                           reading->gain_db,
                                           reading->sweep_seconds);
            cairo_text_extents_t extents;
            cairo_text_extents(cr, legend, &extents);

            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
            cairo_rectangle(cr, legend_x - 6, y - 12, extents.width + 50, 17);
            cairo_fill(cr);

            cairo_set_source_rgba(cr, reading->color.red, reading->color.green, reading->color.blue, 1.0);
            cairo_set_line_width(cr, 3.0);
            cairo_move_to(cr, legend_x, y - 4);
            cairo_line_to(cr, legend_x + 28, y - 4);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 0.92, 0.95, 0.98, 0.95);
            cairo_move_to(cr, legend_x + 36, y);
            cairo_show_text(cr, legend);
            g_free(legend);
        }
    }
}

static void render_graph(App *app, cairo_t *cr, int width, int height)
{
    guint64 preview_pixels = app->total_pixels > 0 ?
        app->total_pixels :
        (guint64)MAX(1, width);
    render_spectrum(app, cr, width, height, 0, preview_pixels, TRUE);
}

static gboolean draw_cb(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    render_graph(user_data, cr, alloc.width, alloc.height);
    return FALSE;
}

static gboolean motion_cb(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    App *app = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    double min_hz;
    double max_hz;
    get_view_range(app, &min_hz, &max_hz);

    double left = 72.0;
    double right = 22.0;
    double plot_w = MAX(10.0, alloc.width - left - right);
    double x = fmax(0.0, fmin(plot_w, event->x - left));
    double hz = min_hz + (max_hz - min_hz) * x / plot_w;

    GString *text = g_string_new(NULL);
    g_string_append_printf(text, "Cursor %.6f MHz", hz / 1000000.0);
    for (guint i = 0; app->readings && i < app->readings->len; i++) {
        Reading *reading = g_ptr_array_index(app->readings, i);
        if (hz < reading->sweep_min_hz || hz > reading->sweep_max_hz) {
            continue;
        }
        guint64 pixel = (guint64)floor((hz - reading->sweep_min_hz) / reading->pixel_hz);
        pixel = MIN(pixel, reading->total_pixels - 1);
        double db;
        if (get_reading_average_db(reading, pixel, app->average_window, &db)) {
            g_string_append_printf(text, " | %s %.2f dB", reading->name, db);
        }
    }
    set_status(app, text->str);
    g_string_free(text, TRUE);
    return TRUE;
}

static void start_scan(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    g_mutex_lock(&app->state_lock);
    gboolean already_scanning = app->scanning;
    g_mutex_unlock(&app->state_lock);
    if (already_scanning) {
        return;
    }

    double min_mhz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->min_spin));
    double max_mhz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->max_spin));
    double sample_rate_msps = gtk_range_get_value(GTK_RANGE(app->sample_rate_spin));
    double gain_db = gtk_range_get_value(GTK_RANGE(app->gain_spin));
    double pixel_hz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->pixel_hz_spin));
    const char *args = gtk_entry_get_text(GTK_ENTRY(app->args_entry));

    if (max_mhz <= min_mhz) {
        set_status(app, "A frequencia final precisa ser maior que a inicial.");
        return;
    }
    if (!args || args[0] == '\0') {
        set_status(app, "Informe os argumentos SoapySDR, exemplo: driver=rtlsdr.");
        return;
    }

    app->sweep_min_hz = min_mhz * 1000000.0;
    app->sweep_max_hz = max_mhz * 1000000.0;
    app->capture_sample_rate = sample_rate_msps * 1000000.0;
    app->capture_gain_db = gain_db;
    app->capture_sweep_seconds = 0.0;
    app->pixel_hz = pixel_hz;
    reset_trace(app);

    ScanParams *params = g_new0(ScanParams, 1);
    params->app = app;
    params->args = g_strdup(args);
    params->min_hz = app->sweep_min_hz;
    params->max_hz = app->sweep_max_hz;
    params->sample_rate = sample_rate_msps * 1000000.0;
    params->gain_db = gain_db;

    g_mutex_lock(&app->state_lock);
    app->scanning = TRUE;
    app->stop_requested = FALSE;
    app->scan_thread = g_thread_new("soapy-scan", scan_thread_main, params);
    g_mutex_unlock(&app->state_lock);

    gtk_widget_set_sensitive(app->scan_button, FALSE);
    gtk_widget_set_sensitive(app->stop_button, TRUE);
    set_status(app, "Iniciando captura SoapySDR...");
}

static void stop_scan(App *app)
{
    g_mutex_lock(&app->state_lock);
    if (app->scanning) {
        app->stop_requested = TRUE;
    }
    g_mutex_unlock(&app->state_lock);
    set_status(app, "Parando scan...");
}

static void stop_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    stop_scan(user_data);
}

static void G_GNUC_UNUSED append_chunk_levels(App *app, GString *out, guint64 start_pixel, int width)
{
    char number[64];

    g_string_append_c(out, '[');
    g_mutex_lock(&app->state_lock);
    for (int i = 0; i < width; i++) {
        guint64 pixel = start_pixel + (guint64)i;
        double db;
        if (i > 0) {
            g_string_append_c(out, ',');
        }
        if (get_moving_average_db_locked(app, pixel, app->average_window, &db)) {
            g_ascii_formatd(number, sizeof(number), "%.2f", db);
            g_string_append(out, number);
        } else {
            g_string_append(out, "null");
        }
    }
    g_mutex_unlock(&app->state_lock);
    g_string_append_c(out, ']');
}

static gboolean G_GNUC_UNUSED write_viewer_files(App *app,
                                                 const char *dir,
                                                 GString *chunks_json,
                                                 double db_min,
                                                 double db_max,
                                                 GError **error)
{
    char *metadata_path = g_build_filename(dir, "metadata.js", NULL);
    char *html_path = g_build_filename(dir, "index.html", NULL);
    char *css_path = g_build_filename(dir, "style.css", NULL);
    char *js_path = g_build_filename(dir, "viewer.js", NULL);
    char min_hz_text[64];
    char max_hz_text[64];
    char pixel_hz_text[64];
    char db_min_text[64];
    char db_max_text[64];

    g_ascii_formatd(min_hz_text, sizeof(min_hz_text), "%.0f", app->sweep_min_hz);
    g_ascii_formatd(max_hz_text, sizeof(max_hz_text), "%.0f", app->sweep_max_hz);
    g_ascii_formatd(pixel_hz_text, sizeof(pixel_hz_text), "%.0f", app->pixel_hz);
    g_ascii_formatd(db_min_text, sizeof(db_min_text), "%.6f", db_min);
    g_ascii_formatd(db_max_text, sizeof(db_max_text), "%.6f", db_max);

    char *metadata = g_strdup_printf(
        "window.RF_SWEEP_METADATA={minHz:%s,maxHz:%s,pixelHz:%s,totalPixels:%" G_GUINT64_FORMAT ",height:%d,dbMin:%s,dbMax:%s,averageWindow:%d,chunks:%s};\n",
        min_hz_text,
        max_hz_text,
        pixel_hz_text,
        app->total_pixels,
        GRAPH_HEIGHT,
        db_min_text,
        db_max_text,
        app->average_window,
        chunks_json->str);

    const char *html =
        "<!doctype html>\n"
        "<html lang=\"pt-BR\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "  <title>RF Sweep Viewer</title>\n"
        "  <link rel=\"stylesheet\" href=\"style.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <header><strong>RF Sweep</strong><span id=\"readout\">Mova o mouse sobre o espectro</span></header>\n"
        "  <main id=\"viewport\"><div id=\"strip\"></div><div id=\"cursor\"></div></main>\n"
        "  <script src=\"metadata.js\"></script>\n"
        "  <script src=\"viewer.js\"></script>\n"
        "</body>\n"
        "</html>\n";

    const char *css =
        "html,body{margin:0;height:100%;background:#0b0d12;color:#e7edf3;font:14px system-ui,sans-serif;overflow:hidden}\n"
        "header{height:42px;display:flex;align-items:center;gap:24px;padding:0 14px;background:#151922;border-bottom:1px solid #2a3140;white-space:nowrap}\n"
        "#readout{color:#b9c4d0;font-variant-numeric:tabular-nums}\n"
        "#viewport{position:relative;height:calc(100% - 43px);overflow:auto;background:#080a0e;cursor:crosshair}\n"
        "#strip{display:flex;align-items:flex-start;min-height:720px}\n"
        "#strip img{display:block;width:auto;height:720px;flex:0 0 auto;image-rendering:auto}\n"
        "#cursor{position:absolute;top:0;bottom:0;width:1px;background:#f4d35e;pointer-events:none;display:none}\n";

    const char *js =
        "const meta=window.RF_SWEEP_METADATA;\n"
        "const strip=document.getElementById('strip');\n"
        "const viewport=document.getElementById('viewport');\n"
        "const readout=document.getElementById('readout');\n"
        "const cursor=document.getElementById('cursor');\n"
        "for(const c of meta.chunks){const img=document.createElement('img');img.src=c.file;img.width=c.width;img.height=meta.height;strip.appendChild(img);}\n"
        "function fmtHz(hz){if(hz>=1e9)return (hz/1e9).toFixed(6)+' GHz';if(hz>=1e6)return (hz/1e6).toFixed(6)+' MHz';if(hz>=1e3)return (hz/1e3).toFixed(3)+' kHz';return hz.toFixed(0)+' Hz';}\n"
        "function chunkAt(px){let lo=0,hi=meta.chunks.length-1;while(lo<=hi){const mid=(lo+hi)>>1,c=meta.chunks[mid];if(px<c.startPixel)hi=mid-1;else if(px>=c.startPixel+c.width)lo=mid+1;else return c;}return null;}\n"
        "viewport.addEventListener('mousemove',ev=>{const r=viewport.getBoundingClientRect();const x=Math.max(0,Math.floor(ev.clientX-r.left+viewport.scrollLeft));const y=Math.max(0,Math.floor(ev.clientY-r.top+viewport.scrollTop));const px=Math.min(meta.totalPixels-1,x);const hz=meta.minHz+px*meta.pixelHz;const c=chunkAt(px);let db=null;if(c)db=c.levels[px-c.startPixel];cursor.style.display='block';cursor.style.left=x+'px';readout.textContent=`Freq: ${fmtHz(hz)} | Sinal: ${db==null?'sem dado':db.toFixed(2)+' dB'} | X: ${px}px | Y: ${y}px`;});\n"
        "viewport.addEventListener('mouseleave',()=>{cursor.style.display='none';});\n";

    gboolean ok =
        g_file_set_contents(metadata_path, metadata, -1, error) &&
        g_file_set_contents(html_path, html, -1, error) &&
        g_file_set_contents(css_path, css, -1, error) &&
        g_file_set_contents(js_path, js, -1, error);

    g_free(metadata);
    g_free(metadata_path);
    g_free(html_path);
    g_free(css_path);
    g_free(js_path);
    return ok;
}

static void save_png(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Exportar leituras",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancelar",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Exportar",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "rf-sweep.rfsweep");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GString *out = g_string_new("RF_SWEEP_V2\n");
        GError *error = NULL;

        if (!app->last_scan_reading) {
            set_status(app, "Nada para exportar: faca um scan primeiro.");
            g_string_free(out, TRUE);
            g_free(filename);
            gtk_widget_destroy(dialog);
            return;
        }
        append_reading_to_file(out, app->last_scan_reading);

        if (g_file_set_contents(filename, out->str, -1, &error)) {
            char *message = g_strdup_printf("Exportado ultimo scan: %s", filename);
            set_status(app, message);
            g_free(message);
        } else {
            char *message = g_strdup_printf("Erro ao exportar: %s",
                                            error ? error->message : "erro desconhecido");
            set_status(app, message);
            g_free(message);
            g_clear_error(&error);
        }

        g_string_free(out, TRUE);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

static void export_view_png(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Exportar PNG da visualizacao",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancelar",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Salvar PNG",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "rf-sweep-view.png");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *filename = g_str_has_suffix(chosen, ".png") ? g_strdup(chosen) : g_strdup_printf("%s.png", chosen);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, PNG_EXPORT_WIDTH, GRAPH_HEIGHT);
        cairo_status_t status = cairo_surface_status(surface);

        if (status == CAIRO_STATUS_SUCCESS) {
            cairo_t *cr = cairo_create(surface);
            render_spectrum(app, cr, PNG_EXPORT_WIDTH, GRAPH_HEIGHT, 0, app->total_pixels, TRUE);
            cairo_destroy(cr);
            status = cairo_surface_write_to_png(surface, filename);
        }

        cairo_surface_destroy(surface);
        if (status == CAIRO_STATUS_SUCCESS) {
            char *message = g_strdup_printf("PNG exportado: %s (%d x %d px)",
                                            filename,
                                            PNG_EXPORT_WIDTH,
                                            GRAPH_HEIGHT);
            set_status(app, message);
            g_free(message);
        } else {
            char *message = g_strdup_printf("Erro ao exportar PNG: %s", cairo_status_to_string(status));
            set_status(app, message);
            g_free(message);
        }
        g_free(chosen);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

static void import_readings(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Importar leituras",
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancelar",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Importar",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *contents = NULL;
        gsize length = 0;
        GError *error = NULL;

        if (!g_file_get_contents(filename, &contents, &length, &error)) {
            char *message = g_strdup_printf("Erro ao importar: %s", error->message);
            set_status(app, message);
            g_free(message);
            g_clear_error(&error);
            g_free(filename);
            gtk_widget_destroy(dialog);
            return;
        }

        char **lines = g_strsplit(contents, "\n", -1);
        Reading *reading = NULL;
        gboolean in_data = FALSE;
        guint imported = 0;

        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (line[0] == '\0' || g_strcmp0(line, "RF_SWEEP_V2") == 0) {
                continue;
            }
            if (g_strcmp0(line, "READING") == 0) {
                reading = g_new0(Reading, 1);
                reading->bins = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_free);
                reading->name = g_strdup("Importada");
                reading->device_args = g_strdup("");
                reading->color = auto_color_for_index(app->readings->len + imported);
                in_data = FALSE;
                continue;
            }
            if (!reading) {
                continue;
            }
            if (g_strcmp0(line, "DATA") == 0) {
                in_data = TRUE;
                continue;
            }
            if (g_strcmp0(line, "END") == 0) {
                g_ptr_array_add(app->readings, reading);
                reading = NULL;
                imported++;
                in_data = FALSE;
                continue;
            }

            if (in_data) {
                guint64 pixel = 0;
                double sum_db = 0.0;
                guint count = 0;
                if (sscanf(line, "%" SCNu64 " %lf %u", &pixel, &sum_db, &count) == 3) {
                    guint64 *key = g_new(guint64, 1);
                    TraceBin *bin = g_new0(TraceBin, 1);
                    *key = pixel;
                    bin->sum_db = sum_db;
                    bin->count = count;
                    g_hash_table_insert(reading->bins, key, bin);
                }
                continue;
            }

            char **kv = g_strsplit(line, "=", 2);
            if (kv[0] && kv[1]) {
                if (g_strcmp0(kv[0], "name") == 0) {
                    g_free(reading->name);
                    reading->name = g_strdup(kv[1]);
                } else if (g_strcmp0(kv[0], "device_args") == 0) {
                    g_free(reading->device_args);
                    reading->device_args = g_strdup(kv[1]);
                } else if (g_strcmp0(kv[0], "min_hz") == 0) {
                    reading->sweep_min_hz = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "max_hz") == 0) {
                    reading->sweep_max_hz = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "sample_rate") == 0) {
                    reading->sample_rate = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "gain_db") == 0) {
                    reading->gain_db = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "sweep_seconds") == 0) {
                    reading->sweep_seconds = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "pixel_hz") == 0) {
                    reading->pixel_hz = g_ascii_strtod(kv[1], NULL);
                } else if (g_strcmp0(kv[0], "total_pixels") == 0) {
                    reading->total_pixels = g_ascii_strtoull(kv[1], NULL, 10);
                } else if (g_strcmp0(kv[0], "color") == 0) {
                    GdkRGBA parsed;
                    if (gdk_rgba_parse(&parsed, kv[1])) {
                        reading->color = parsed;
                    }
                }
            }
            g_strfreev(kv);
        }

        if (reading) {
            reading_free(reading);
        }

        char *message = g_strdup_printf("Importado: %u leituras de %s", imported, filename);
        set_status(app, message);
        g_free(message);
        g_strfreev(lines);
        g_free(contents);
        g_free(filename);
        gtk_widget_queue_draw(app->drawing_area);
    }

    gtk_widget_destroy(dialog);
}

static void clear_readings(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    if (app->readings) {
        g_ptr_array_set_size(app->readings, 0);
    }
    app->last_scan_reading = NULL;
    gtk_entry_set_text(GTK_ENTRY(app->name_entry), "Leitura 1");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    set_status(app, "Leituras limpas. Use Importar para carregar leituras salvas.");
    gtk_widget_queue_draw(app->drawing_area);
}

static void preset_changed(GtkComboBox *combo, gpointer user_data)
{
    App *app = user_data;
    int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index >= (int)PRESETS_LEN) {
        return;
    }

    const DevicePreset *preset = &PRESETS[index];
    gtk_entry_set_text(GTK_ENTRY(app->args_entry), preset->args);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->min_spin), preset->min_mhz);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->max_spin), preset->max_mhz);
    gtk_range_set_range(GTK_RANGE(app->sample_rate_spin),
                        preset->sample_rate_min_msps,
                        preset->sample_rate_max_msps);
    gtk_range_set_increments(GTK_RANGE(app->sample_rate_spin), 0.1, 1.0);
    gtk_range_set_value(GTK_RANGE(app->sample_rate_spin), preset->sample_rate_msps);
    gtk_range_set_range(GTK_RANGE(app->gain_spin), preset->gain_min_db, preset->gain_max_db);
    gtk_range_set_increments(GTK_RANGE(app->gain_spin), 1.0, 5.0);
    gtk_range_set_value(GTK_RANGE(app->gain_spin), preset->gain_db);
}

static void average_changed(GtkComboBox *combo, gpointer user_data)
{
    App *app = user_data;
    int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index >= (int)AVERAGE_WINDOWS_LEN) {
        index = 2;
    }

    app->average_window = AVERAGE_WINDOWS[index];
    gtk_widget_queue_draw(app->drawing_area);
}

static GtkWidget *labeled(GtkWidget *child, const char *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), child, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *make_spin(double min, double max, double step, int digits)
{
    GtkAdjustment *adj = gtk_adjustment_new(min, min, max, step, step * 10.0, 0.0);
    GtkWidget *spin = gtk_spin_button_new(adj, step, digits);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), 9);
    return spin;
}

static GtkWidget *make_scale(double min, double max, double step, int digits)
{
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_digits(GTK_SCALE(scale), digits);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_TOP);
    gtk_widget_set_size_request(scale, 150, -1);
    return scale;
}

static void destroy_cb(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    App *app = user_data;

    g_mutex_lock(&app->state_lock);
    app->stop_requested = TRUE;
    GThread *thread = app->scan_thread;
    app->scan_thread = NULL;
    g_mutex_unlock(&app->state_lock);

    if (thread) {
        g_thread_join(thread);
    }

    if (app->trace_bins) {
        g_hash_table_destroy(app->trace_bins);
    }
    if (app->readings) {
        g_ptr_array_free(app->readings, TRUE);
    }
    if (app->redraw_timer_id != 0) {
        g_source_remove(app->redraw_timer_id);
    }
    g_mutex_clear(&app->state_lock);
    gtk_main_quit();
}

static void activate(App *app)
{
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "RF Sweep SoapySDR");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1200, 820);
    g_signal_connect(app->window, "destroy", G_CALLBACK(destroy_cb), app);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);
    gtk_container_add(GTK_CONTAINER(app->window), root);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);

    app->device_combo = gtk_combo_box_text_new();
    for (guint i = 0; i < PRESETS_LEN; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->device_combo), PRESETS[i].name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->device_combo, "Dispositivo"), FALSE, FALSE, 0);
    g_signal_connect(app->device_combo, "changed", G_CALLBACK(preset_changed), app);

    app->min_spin = make_spin(0.01, 8000.0, 1.0, 2);
    app->max_spin = make_spin(0.01, 8000.0, 1.0, 2);
    app->sample_rate_spin = make_scale(0.25, 20.0, 0.1, 2);
    app->gain_spin = make_scale(0.0, 80.0, 1.0, 0);
    app->pixel_hz_spin = make_spin(500.0, 2500.0, 100.0, 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->pixel_hz_spin), DEFAULT_PIXEL_HZ);
    app->average_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "Off");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "MOV 3");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "MOV 5");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "MOV 9");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "MOV 15");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->average_combo), "MOV 31");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->average_combo), 2);
    app->name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(app->name_entry), "Leitura 1");
    gtk_entry_set_width_chars(GTK_ENTRY(app->name_entry), 12);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->min_spin, "Inicio MHz"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->max_spin, "Fim MHz"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->sample_rate_spin, "Sample MS/s"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->gain_spin, "Ganho dB"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->pixel_hz_spin, "Hz/pixel"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->average_combo, "Averaging"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->name_entry, "Nome"), FALSE, FALSE, 0);

    app->scan_button = gtk_button_new_with_label("Scan");
    app->stop_button = gtk_button_new_with_label("Parar");
    app->save_button = gtk_button_new_with_label("Exportar");
    app->png_button = gtk_button_new_with_label("Exportar PNG");
    app->import_button = gtk_button_new_with_label("Importar");
    app->clear_button = gtk_button_new_with_label("Limpar");
    gtk_widget_set_sensitive(app->stop_button, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), app->scan_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->import_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->clear_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->png_button, FALSE, FALSE, 0);
    g_signal_connect(app->scan_button, "clicked", G_CALLBACK(start_scan), app);
    g_signal_connect(app->stop_button, "clicked", G_CALLBACK(stop_clicked), app);
    g_signal_connect(app->import_button, "clicked", G_CALLBACK(import_readings), app);
    g_signal_connect(app->clear_button, "clicked", G_CALLBACK(clear_readings), app);
    g_signal_connect(app->save_button, "clicked", G_CALLBACK(save_png), app);
    g_signal_connect(app->png_button, "clicked", G_CALLBACK(export_view_png), app);

    app->args_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(root), labeled(app->args_entry, "Argumentos SoapySDR"), FALSE, FALSE, 0);

    app->progress_bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(root), app->progress_bar, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_ALWAYS, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->drawing_area, PREVIEW_MAX_WIDTH, GRAPH_HEIGHT);
    gtk_widget_add_events(app->drawing_area, GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(scroll), app->drawing_area);
    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(draw_cb), app);
    g_signal_connect(app->drawing_area, "motion-notify-event", G_CALLBACK(motion_cb), app);

    app->status_label = gtk_label_new("Pronto.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), app->status_label, FALSE, FALSE, 0);

    g_mutex_init(&app->state_lock);
    app->readings = g_ptr_array_new_with_free_func(reading_free);
    app->pixel_hz = DEFAULT_PIXEL_HZ;
    app->average_window = 5;
    app->redraw_timer_id = g_timeout_add(300, redraw_timer_cb, app);
    reset_trace(app);
    preset_changed(GTK_COMBO_BOX(app->device_combo), app);
    g_signal_connect(app->average_combo, "changed", G_CALLBACK(average_changed), app);
    gtk_widget_show_all(app->window);
}

int app_run(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    gtk_disable_setlocale();
    gtk_init(&argc, &argv);
    setlocale(LC_NUMERIC, "C");

    App app;
    memset(&app, 0, sizeof(app));
    activate(&app);

    gtk_main();
    return 0;
}
