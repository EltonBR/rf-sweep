#include "app_internal.h"

static double graph_y(double db, double top, double plot_h, double db_min, double db_max)
{
    double clamped = fmax(db_min, fmin(db_max, db));
    double t = (clamped - db_min) / (db_max - db_min);
    return top + plot_h - (t * plot_h);
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


static gboolean get_reading_peak_db_in_range(Reading *reading,
                                             double hz1,
                                             double hz2,
                                             int average_window,
                                             double *db)
{
    if (hz2 < reading->sweep_min_hz || hz1 > reading->sweep_max_hz) {
        return FALSE;
    }

    hz1 = fmax(hz1, reading->sweep_min_hz);
    hz2 = fmin(hz2, reading->sweep_max_hz);
    guint64 p1 = (guint64)floor((hz1 - reading->sweep_min_hz) / reading->pixel_hz);
    guint64 p2 = (guint64)ceil((hz2 - reading->sweep_min_hz) / reading->pixel_hz);
    p1 = MIN(p1, reading->total_pixels - 1);
    p2 = MIN(MAX(p1 + 1, p2), reading->total_pixels);

    gboolean found = FALSE;
    double peak = 0.0;
    for (guint64 p = p1; p < p2; p++) {
        double value;
        if (!get_reading_average_db(reading, p, average_window, &value)) {
            continue;
        }
        if (!found || value > peak) {
            peak = value;
            found = TRUE;
        }
    }

    if (!found) {
        return FALSE;
    }

    *db = peak;
    return TRUE;
}


gboolean get_reading_db_at_hz(Reading *reading, double hz, int average_window, double *db)
{
    if (hz < reading->sweep_min_hz || hz > reading->sweep_max_hz || reading->pixel_hz <= 0.0) {
        return FALSE;
    }

    guint64 pixel = (guint64)floor((hz - reading->sweep_min_hz) / reading->pixel_hz);
    pixel = MIN(pixel, reading->total_pixels - 1);
    return get_reading_average_db(reading, pixel, average_window, db);
}


void render_spectrum(App *app,
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
            double hz1 = min_hz + (max_hz - min_hz) * (double)i / MAX(1, width);
            double hz2 = min_hz + (max_hz - min_hz) * (double)(i + 1) / MAX(1, width);
            double db;
            if (!get_reading_peak_db_in_range(reading, hz1, hz2, app->average_window, &db)) {
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


gboolean draw_cb(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    render_graph(user_data, cr, alloc.width, alloc.height);
    return FALSE;
}


gboolean motion_cb(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
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
