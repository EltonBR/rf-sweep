#include "app_internal.h"
#include "app.h"
#include <locale.h>

static void format_msps(double value, char *buf, size_t len);
static const DevicePreset *active_preset(App *app);
static double selected_sample_rate_msps(App *app);

void set_status(App *app, const char *message)
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


void post_ui(App *app, const char *message, double progress, gboolean done, gboolean queue_draw)
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
    double sample_rate_msps = selected_sample_rate_msps(app);
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
    if (sample_rate_msps <= 0.0) {
        set_status(app, "Selecione um sample rate suportado pelo preset do SDR.");
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
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->sample_rate_combo));
    int selected_rate = 0;
    double best_delta = G_MAXDOUBLE;
    for (guint i = 0; i < preset->sample_rates_len; i++) {
        char value[32];
        char label[48];
        format_msps(preset->sample_rates_msps[i], value, sizeof(value));
        g_snprintf(label, sizeof(label), "%s MS/s", value);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->sample_rate_combo), label);

        double delta = fabs(preset->sample_rates_msps[i] - preset->sample_rate_msps);
        if (delta < best_delta) {
            best_delta = delta;
            selected_rate = (int)i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->sample_rate_combo), selected_rate);
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


static void fit_changed(GtkToggleButton *button, gpointer user_data)
{
    App *app = user_data;
    app->fit_to_window = gtk_toggle_button_get_active(button);
    gtk_widget_set_size_request(app->drawing_area,
                                app->fit_to_window ? 1 : app->preview_width,
                                GRAPH_HEIGHT);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->scroll_window),
                                   app->fit_to_window ? GTK_POLICY_NEVER : GTK_POLICY_ALWAYS,
                                   GTK_POLICY_AUTOMATIC);
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


static void format_msps(double value, char *buf, size_t len)
{
    g_ascii_formatd(buf, len, "%.3f", value);
    char *dot = strchr(buf, '.');
    if (!dot) {
        return;
    }
    char *end = buf + strlen(buf) - 1;
    while (end > dot && *end == '0') {
        *end-- = '\0';
    }
    if (end == dot) {
        *end = '\0';
    }
}

static const DevicePreset *active_preset(App *app)
{
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(app->device_combo));
    if (index < 0 || index >= (int)PRESETS_LEN) {
        return NULL;
    }
    return &PRESETS[index];
}

static double selected_sample_rate_msps(App *app)
{
    const DevicePreset *preset = active_preset(app);
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(app->sample_rate_combo));
    if (!preset || index < 0 || index >= (int)preset->sample_rates_len) {
        return 0.0;
    }
    return preset->sample_rates_msps[index];
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
    app->sample_rate_combo = gtk_combo_box_text_new();
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
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->sample_rate_combo, "Sample MS/s"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->gain_spin, "Ganho dB"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->pixel_hz_spin, "Hz/pixel"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->average_combo, "Averaging"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), labeled(app->name_entry, "Nome"), FALSE, FALSE, 0);
    app->fit_check = gtk_check_button_new_with_label("Ajustar janela");
    gtk_box_pack_start(GTK_BOX(toolbar), app->fit_check, FALSE, FALSE, 0);

    app->scan_button = gtk_button_new_with_label("Scan");
    app->stop_button = gtk_button_new_with_label("Parar");
    app->save_button = gtk_button_new_with_label("Exportar");
    app->png_button = gtk_button_new_with_label("Exportar PNG");
    app->import_button = gtk_button_new_with_label("Importar");
    app->clear_button = gtk_button_new_with_label("Limpar");
    app->subtract_button = gtk_button_new_with_label("Subtrair");
    gtk_widget_set_sensitive(app->stop_button, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), app->scan_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->import_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->clear_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->subtract_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->png_button, FALSE, FALSE, 0);
    g_signal_connect(app->scan_button, "clicked", G_CALLBACK(start_scan), app);
    g_signal_connect(app->stop_button, "clicked", G_CALLBACK(stop_clicked), app);
    g_signal_connect(app->import_button, "clicked", G_CALLBACK(import_readings), app);
    g_signal_connect(app->clear_button, "clicked", G_CALLBACK(clear_readings), app);
    g_signal_connect(app->subtract_button, "clicked", G_CALLBACK(subtract_readings), app);
    g_signal_connect(app->save_button, "clicked", G_CALLBACK(save_png), app);
    g_signal_connect(app->png_button, "clicked", G_CALLBACK(export_view_png), app);
    g_signal_connect(app->fit_check, "toggled", G_CALLBACK(fit_changed), app);

    app->args_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(root), labeled(app->args_entry, "Argumentos SoapySDR"), FALSE, FALSE, 0);

    app->progress_bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(root), app->progress_bar, FALSE, FALSE, 0);

    app->scroll_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->scroll_window), GTK_POLICY_ALWAYS, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(root), app->scroll_window, TRUE, TRUE, 0);

    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app->drawing_area, TRUE);
    gtk_widget_set_size_request(app->drawing_area, PREVIEW_MAX_WIDTH, GRAPH_HEIGHT);
    gtk_widget_add_events(app->drawing_area, GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(app->scroll_window), app->drawing_area);
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
