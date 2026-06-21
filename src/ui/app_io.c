#include "app_internal.h"

void save_png(GtkButton *button, gpointer user_data)
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


void export_view_png(GtkButton *button, gpointer user_data)
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


void import_readings(GtkButton *button, gpointer user_data)
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
        gboolean valid_header = FALSE;
        guint imported = 0;

        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (line[0] == '\0') {
                continue;
            }
            if (g_strcmp0(line, "RF_SWEEP_V2") == 0) {
                valid_header = TRUE;
                continue;
            }
            if (!valid_header) {
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
                if (reading->bins && g_hash_table_size(reading->bins) > 0) {
                    g_ptr_array_add(app->readings, reading);
                    imported++;
                } else {
                    reading_free(reading);
                }
                reading = NULL;
                in_data = FALSE;
                continue;
            }

            if (in_data) {
                guint64 pixel = 0;
                double sum_value = 0.0;
                double peak_value = 0.0;
                guint count = 0;
                int fields = sscanf(line, "%" SCNu64 " %lf %lf %u", &pixel, &sum_value, &peak_value, &count);
                if (fields == 4 && sum_value > 0.0 && peak_value > 0.0 && count > 0) {
                    guint64 *key = g_new(guint64, 1);
                    TraceBin *bin = g_new0(TraceBin, 1);
                    *key = pixel;
                    bin->sum_power = sum_value;
                    bin->peak_power = peak_value;
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

        char *message = valid_header
            ? g_strdup_printf("Importado: %u leituras de %s", imported, filename)
            : g_strdup_printf("Arquivo antigo ou invalido ignorado: %s", filename);
        set_status(app, message);
        g_free(message);
        g_strfreev(lines);
        g_free(contents);
        g_free(filename);
        gtk_widget_queue_draw(app->drawing_area);
    }

    gtk_widget_destroy(dialog);
}


void subtract_readings(GtkButton *button, gpointer user_data)
{
    (void)button;
    App *app = user_data;

    if (!app->readings || app->readings->len < 2) {
        set_status(app, "Subtracao precisa de pelo menos duas leituras exibidas.");
        return;
    }

    Reading *base = g_ptr_array_index(app->readings, 0);
    double min_hz = base->sweep_min_hz;
    double max_hz = base->sweep_max_hz;
    double pixel_hz = base->pixel_hz;

    for (guint i = 1; i < app->readings->len; i++) {
        Reading *reading = g_ptr_array_index(app->readings, i);
        min_hz = fmax(min_hz, reading->sweep_min_hz);
        max_hz = fmin(max_hz, reading->sweep_max_hz);
        pixel_hz = fmin(pixel_hz, reading->pixel_hz);
    }

    if (max_hz <= min_hz || pixel_hz <= 0.0) {
        set_status(app, "As leituras nao possuem faixa de frequencia comum para subtrair.");
        return;
    }

    Reading *result = g_new0(Reading, 1);
    result->name = g_strdup_printf("Subtracao %u", app->readings->len + 1);
    result->device_args = g_strdup("subtracao");
    result->sweep_min_hz = min_hz;
    result->sweep_max_hz = max_hz;
    result->sample_rate = base->sample_rate;
    result->gain_db = 0.0;
    result->sweep_seconds = 0.0;
    result->pixel_hz = pixel_hz;
    result->total_pixels = compute_total_pixels(min_hz, max_hz, pixel_hz);
    result->color = auto_color_for_index(app->readings->len);
    result->bins = g_hash_table_new_full(uint64_hash, uint64_equal, g_free, g_free);

    guint64 written = 0;
    for (guint64 p = 0; p < result->total_pixels; p++) {
        double hz = min_hz + ((double)p + 0.5) * pixel_hz;
        if (hz > max_hz) {
            hz = max_hz;
        }

        double db;
        if (!get_reading_db_at_hz(base, hz, app->average_window, &db)) {
            continue;
        }

        gboolean valid = TRUE;
        for (guint i = 1; i < app->readings->len; i++) {
            Reading *reading = g_ptr_array_index(app->readings, i);
            double other_db;
            if (!get_reading_db_at_hz(reading, hz, app->average_window, &other_db)) {
                valid = FALSE;
                break;
            }
            db -= other_db;
        }
        if (!valid) {
            continue;
        }

        guint64 *key = g_new(guint64, 1);
        TraceBin *bin = g_new0(TraceBin, 1);
        double power = pow(10.0, db / 10.0);
        *key = p;
        bin->sum_power = power;
        bin->peak_power = power;
        bin->count = 1;
        g_hash_table_insert(result->bins, key, bin);
        written++;
    }

    if (written == 0) {
        reading_free(result);
        set_status(app, "Nao ha bins validos suficientes para gerar a subtracao.");
        return;
    }

    g_ptr_array_add(app->readings, result);
    char *message = g_strdup_printf("Subtracao criada: %s = primeira leitura - demais (%" G_GUINT64_FORMAT " bins).",
                                    result->name,
                                    written);
    set_status(app, message);
    g_free(message);
    gtk_widget_queue_draw(app->drawing_area);
}


void clear_readings(GtkButton *button, gpointer user_data)
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
