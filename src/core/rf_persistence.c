#include "rf_persistence.h"

void append_reading_to_file(GString *out, Reading *reading)
{
    char color[32];
    g_snprintf(color, sizeof(color), "#%02x%02x%02x",
               (unsigned)(reading->color.red * 255.0),
               (unsigned)(reading->color.green * 255.0),
               (unsigned)(reading->color.blue * 255.0));
    g_string_append(out, "READING\n");
    g_string_append_printf(out, "name=%s\n", reading->name);
    g_string_append_printf(out, "device_args=%s\n", reading->device_args);
    g_string_append_printf(out, "min_hz=%.0f\n", reading->sweep_min_hz);
    g_string_append_printf(out, "max_hz=%.0f\n", reading->sweep_max_hz);
    g_string_append_printf(out, "sample_rate=%.0f\n", reading->sample_rate);
    g_string_append_printf(out, "gain_db=%.6f\n", reading->gain_db);
    g_string_append_printf(out, "sweep_seconds=%.3f\n", reading->sweep_seconds);
    g_string_append_printf(out, "pixel_hz=%.0f\n", reading->pixel_hz);
    g_string_append_printf(out, "total_pixels=%" G_GUINT64_FORMAT "\n", reading->total_pixels);
    g_string_append_printf(out, "color=%s\n", color);
    g_string_append(out, "DATA\n");

    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, reading->bins);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint64 pixel = *(guint64 *)key;
        TraceBin *bin = value;
        g_string_append_printf(out, "%" G_GUINT64_FORMAT " %.9f %u\n",
                               pixel,
                               bin->sum_db,
                               bin->count);
    }
    g_string_append(out, "END\n");
}
