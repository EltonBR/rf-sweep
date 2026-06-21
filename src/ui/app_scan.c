#include "app_internal.h"

gboolean should_stop(App *app)
{
    gboolean stop;
    g_mutex_lock(&app->state_lock);
    stop = app->stop_requested;
    g_mutex_unlock(&app->state_lock);
    return stop;
}


void reset_trace(App *app)
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
    gtk_widget_set_size_request(app->drawing_area,
                                app->fit_to_window ? 1 : app->preview_width,
                                GRAPH_HEIGHT);
    gtk_widget_queue_draw(app->drawing_area);
}


Reading *create_reading_from_capture(App *app)
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


static void add_power_band(App *app, double freq_hz, double bandwidth_hz, double power)
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
        bin->sum_power += power;
        if (bin->count == 0 || power > bin->peak_power) {
            bin->peak_power = power;
        }
        bin->count++;
    }
    app->has_trace = TRUE;
    g_mutex_unlock(&app->state_lock);
}


static void process_fft(App *app, const float *samples, double center_hz, double sample_rate)
{
    double offsets_hz[FFT_SIZE];
    double powers[FFT_SIZE];
    gboolean usable[FFT_SIZE];

    spectrum_power_from_cf32(samples, FFT_SIZE, sample_rate, offsets_hz, powers, usable);
    for (int i = 0; i < FFT_SIZE; i++) {
        if (!usable[i]) {
            continue;
        }
        add_power_band(app, center_hz + offsets_hz[i], sample_rate / FFT_SIZE, powers[i]);
    }
}


static gboolean read_fft_samples(SoapySDRDevice *device,
                                 SoapySDRStream *stream,
                                 float *samples,
                                 App *app,
                                 char **message)
{
    size_t got = 0;
    while (got < FFT_SIZE && !should_stop(app)) {
        void *buffs[] = {samples + got * 2};
        int flags = 0;
        long long time_ns = 0;
        int rc = SoapySDRDevice_readStream(device, stream, buffs, FFT_SIZE - got,
                                           &flags, &time_ns, 300000);
        if (rc == SOAPY_SDR_TIMEOUT || rc == SOAPY_SDR_OVERFLOW) {
            continue;
        }
        if (rc < 0) {
            *message = g_strdup_printf("Erro lendo amostras: %s", SoapySDRDevice_lastError());
            return FALSE;
        }
        got += (size_t)rc;
    }

    return got == FFT_SIZE;
}


static gboolean discard_samples_after_tune(SoapySDRDevice *device,
                                           SoapySDRStream *stream,
                                           float *samples,
                                           double sample_rate,
                                           App *app,
                                           char **message)
{
    size_t discard_total = MAX((size_t)FFT_SIZE,
                               (size_t)ceil(sample_rate * DISCARD_SECONDS_AFTER_TUNE));
    size_t discarded = 0;
    while (discarded < discard_total && !should_stop(app)) {
        size_t want = MIN((size_t)FFT_SIZE, discard_total - discarded);
        void *buffs[] = {samples};
        int flags = 0;
        long long time_ns = 0;
        int rc = SoapySDRDevice_readStream(device, stream, buffs, want,
                                           &flags, &time_ns, 300000);
        if (rc == SOAPY_SDR_TIMEOUT || rc == SOAPY_SDR_OVERFLOW) {
            continue;
        }
        if (rc < 0) {
            *message = g_strdup_printf("Erro descartando amostras antigas: %s",
                                       SoapySDRDevice_lastError());
            return FALSE;
        }
        discarded += (size_t)rc;
    }

    return !should_stop(app);
}


gpointer scan_thread_main(gpointer data)
{
    ScanParams *params = data;
    App *app = params->app;
    SoapySDRDevice *device = NULL;
    SoapySDRStream *stream = NULL;
    gboolean stream_active = FALSE;
    float *samples = NULL;
    char *message = NULL;
    gint64 sweep_start_us = g_get_monotonic_time();
    double actual_sample_rate = params->sample_rate;

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
    double reported_sample_rate = SoapySDRDevice_getSampleRate(device, SOAPY_SDR_RX, 0);
    if (reported_sample_rate > 0.0) {
        actual_sample_rate = reported_sample_rate;
    }
    SoapySDRDevice_setGainMode(device, SOAPY_SDR_RX, 0, false);
    SoapySDRDevice_setGain(device, SOAPY_SDR_RX, 0, params->gain_db);

    samples = g_new0(float, FFT_SIZE * 2);
    double step_hz = actual_sample_rate * FFT_STEP_FRACTION;
    int total_steps = MAX(1, (int)ceil((params->max_hz - params->min_hz) / step_hz));
    gint64 last_ui_update = 0;

    for (int step = 0; step < total_steps && !should_stop(app); step++) {
        double center_hz;
        if ((params->max_hz - params->min_hz) <= actual_sample_rate) {
            center_hz = (params->min_hz + params->max_hz) * 0.5;
        } else {
            center_hz = params->min_hz + step_hz * step + actual_sample_rate * 0.5;
            center_hz = MIN(center_hz, params->max_hz - actual_sample_rate * 0.5);
            center_hz = MAX(center_hz, params->min_hz + actual_sample_rate * 0.5);
        }

        rc = SoapySDRDevice_setFrequency(device, SOAPY_SDR_RX, 0, center_hz, NULL);
        if (rc != 0) {
            message = g_strdup_printf("Falha ao sintonizar %.3f MHz: %s",
                                      center_hz / 1000000.0,
                                      SoapySDRDevice_lastError());
            goto done;
        }

        double actual_center_hz = SoapySDRDevice_getFrequency(device, SOAPY_SDR_RX, 0);
        if (actual_center_hz <= 0.0) {
            actual_center_hz = center_hz;
        }

        size_t channel = 0;
        stream = SoapySDRDevice_setupStream(device, SOAPY_SDR_RX, SOAPY_SDR_CF32, &channel, 1, NULL);
        if (!stream) {
            message = g_strdup_printf("Falha no stream SoapySDR em %.3f MHz: %s",
                                      center_hz / 1000000.0,
                                      SoapySDRDevice_lastError());
            goto done;
        }

        rc = SoapySDRDevice_activateStream(device, stream, 0, 0, 0);
        if (rc != 0) {
            message = g_strdup_printf("Falha ao ativar stream em %.3f MHz: %s",
                                      center_hz / 1000000.0,
                                      SoapySDRDevice_lastError());
            goto done;
        }
        stream_active = TRUE;

        usleep(TUNE_SETTLE_US);

        if (!discard_samples_after_tune(device, stream, samples, actual_sample_rate, app, &message)) {
            goto done;
        }

        for (int frame = 0; frame < FFTS_PER_STEP && !should_stop(app); frame++) {
            if (!read_fft_samples(device, stream, samples, app, &message)) {
                goto done;
            }
            process_fft(app, samples, actual_center_hz, actual_sample_rate);
        }

        rc = SoapySDRDevice_deactivateStream(device, stream, 0, 0);
        stream_active = FALSE;
        if (rc != 0) {
            message = g_strdup_printf("Falha ao desativar stream em %.3f MHz: %s",
                                      center_hz / 1000000.0,
                                      SoapySDRDevice_lastError());
            goto done;
        }
        SoapySDRDevice_closeStream(device, stream);
        stream = NULL;

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
        if (stream_active) {
            SoapySDRDevice_deactivateStream(device, stream, 0, 0);
        }
        SoapySDRDevice_closeStream(device, stream);
    }
    if (device) {
        SoapySDRDevice_unmake(device);
    }
    g_free(samples);

    g_mutex_lock(&app->state_lock);
    app->capture_sweep_seconds = (g_get_monotonic_time() - sweep_start_us) / 1000000.0;
    app->capture_sample_rate = actual_sample_rate;
    g_mutex_unlock(&app->state_lock);

    post_ui(app, message, should_stop(app) ? -1.0 : 1.0, TRUE, TRUE);
    g_free(message);
    g_free(params->args);
    g_free(params);
    return NULL;
}
