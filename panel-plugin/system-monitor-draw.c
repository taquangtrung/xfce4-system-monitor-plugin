/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor-draw.c - Rendering, the periodic sampling timer and the
 * on-demand tooltip for the Xfce panel system monitor plugin.
 *
 * See LICENSE for the full licence text.
 */

#include <math.h>

#include <glib/gi18n-lib.h>
#include <libnotify/notify.h>
#include <libxfce4panel/libxfce4panel.h>

#include "system-monitor.h"

#define GRAPH_PAD_TOP    2     /* px: headroom above the curve's peak */
#define GRAPH_PAD_BOTTOM 3     /* px: gap between the curve's baseline and the square's bottom edge */

/* Memory-usage percentages at which a desktop notification is fired, in
 * ascending order. Re-arms once usage drops MEM_NOTIFY_HYSTERESIS points
 * below the lowest one, so a value hovering near a boundary doesn't spam. */
static const gint MEM_NOTIFY_THRESHOLDS[] = { 75, 80, 85, 90, 95 };
#define MEM_NOTIFY_HYSTERESIS 5  /* percentage points */
#define MEM_NOTIFY_URGENT_PCT 90 /* threshold at/above which urgency is critical */

/* Metrics whose history holds a raw rate (scaled at draw time), not a ratio. */
#define IS_RATE_METRIC(m) ((m) == METRIC_NET_DOWN || (m) == METRIC_NET_UP)

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static void
history_push (SystemMonitor *sm)
{
    guint m;
    for (m = 0; m < N_METRICS; m++)
        sm->history[m][sm->history_head] = sm->current[m];
    sm->history_head = (sm->history_head + 1) % HISTORY_SIZE;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Value `back` columns behind the newest sample in an arbitrary ring buffer. */
static gdouble
ring_value (const gdouble *ring, guint head, guint back)
{
    gint idx = (gint) head - 1 - (gint) back;
    idx %= HISTORY_SIZE;
    if (idx < 0)
        idx += HISTORY_SIZE;
    return ring[idx];
}

/* Map a raw network rate (bytes/sec) onto a fixed logarithmic axis from
 * 1 KiB/s (bottom) to 1 GiB/s (top). A fixed axis needs no rescaling, so the
 * network graphs never jump as traffic rises and falls. */
static gdouble
net_log_frac (gdouble rate)
{
    static const gdouble lo = 1024.0;                    /* 1 KiB/s -> 0.0 */
    static const gdouble hi = 1024.0 * 1024.0 * 1024.0;  /* 1 GiB/s -> 1.0 */
    gdouble v;

    if (rate <= lo)
        return 0.0;
    v = log2 (rate / lo) / log2 (hi / lo);
    v = CLAMP (v, 0.0, 1.0);
    return pow (v, 1.5);   /* gamma > 1: bias small values shorter */
}

/* Normalised height (0..1) of one sample: ratio metrics are already in
 * [0, 1]; network samples use the fixed log axis. */
static gdouble
sample_frac (gdouble raw, gboolean logscale)
{
    return logscale ? net_log_frac (raw) : CLAMP (raw, 0.0, 1.0);
}

/* Draw one history curve into the current (already translated + clipped)
 * width x height area: an optional filled area plus a 1px top line. */
static void
draw_curve (cairo_t *cr, const gdouble *ring, guint head, gint width, gint height,
            gboolean logscale, const GdkRGBA *c, gboolean fill)
{
    gint x;

    if (fill) {
        cairo_new_path (cr);
        cairo_move_to (cr, 0, height);
        for (x = 0; x < width; x++) {
            gdouble v = sample_frac (ring_value (ring, head, (guint) (width - 1 - x)), logscale);
            cairo_line_to (cr, x, height - v * height);
        }
        cairo_line_to (cr, width - 1, height);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha * 0.40);
        cairo_fill (cr);
    }

    cairo_new_path (cr);
    for (x = 0; x < width; x++) {
        gdouble v = sample_frac (ring_value (ring, head, (guint) (width - 1 - x)), logscale);
        gdouble y = height - v * height;
        if (x == 0)
            cairo_move_to (cr, x + 0.5, y);
        else
            cairo_line_to (cr, x + 0.5, y);
    }
    cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
}

static void
draw_square (cairo_t *cr, SystemMonitor *sm, MetricType m,
             gint ox, gint oy, gint box_w, gint box_h)
{
    const GdkRGBA *c = &sm->color[m];
    gint curve_h = MAX (box_h - GRAPH_PAD_TOP - GRAPH_PAD_BOTTOM, 1);

    cairo_save (cr);
    cairo_translate (cr, ox, oy + GRAPH_PAD_TOP);
    cairo_rectangle (cr, 0, 0, box_w, curve_h);
    cairo_clip (cr);

    draw_curve (cr, sm->history[m], sm->history_head, box_w, curve_h, IS_RATE_METRIC (m), c, TRUE);

    cairo_restore (cr);

    /* Optional 1px frame so the squares read as distinct tiles. */
    if (sm->show_border) {
        cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha * 0.55);
        cairo_set_line_width (cr, 1.0);
        cairo_rectangle (cr, ox + 0.5, oy + 0.5, box_w - 1, box_h - 1);
        cairo_stroke (cr);
    }
}

gboolean
draw_cb (GtkWidget *widget, cairo_t *cr, SystemMonitor *sm)
{
    GtkOrientation o = xfce_panel_plugin_get_orientation (sm->plugin);
    gint box_w = sm->box_w;
    gint box_h = sm->box_h;
    MetricType m;
    gint slot = 0;

    (void) widget;

    /* No background is painted: the windowless event box keeps the panel
     * background (and thus its transparency) showing through. Only enabled
     * metrics are drawn, packed consecutively with no gaps. */
    for (m = 0; m < N_METRICS; m++) {
        gint ox, oy;
        if (!sm->enabled[m])
            continue;
        ox = (o == GTK_ORIENTATION_HORIZONTAL) ? GRAPH_PAD_SIDE + slot * box_w : GRAPH_PAD_SIDE;
        oy = (o == GTK_ORIENTATION_HORIZONTAL) ? 0 : slot * box_h;
        draw_square (cr, sm, m, ox, oy, box_w, box_h);
        slot++;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Memory notifications                                                */
/* ------------------------------------------------------------------ */

/* Fire a desktop notification announcing that memory usage has crossed
 * "pct" percent, using the plugin's latest mem_used_kb/mem_total_kb. */
static void
notify_mem_threshold (SystemMonitor *sm, gint pct)
{
    gchar *used = format_size_kb (sm->mem_used_kb);
    gchar *total = format_size_kb (sm->mem_total_kb);
    gchar *body = g_strdup_printf (_("%s of %s used (%d%%)"), used, total, pct);
    NotifyNotification *n = notify_notification_new (_("Memory usage high"), body, "dialog-warning");
    GError *error = NULL;

    notify_notification_set_urgency (n, (pct >= MEM_NOTIFY_URGENT_PCT)
                                         ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL);
    if (!notify_notification_show (n, &error)) {
        g_warning ("system-monitor: failed to show memory notification: %s", error->message);
        g_error_free (error);
    }

    g_object_unref (n);
    g_free (body);
    g_free (total);
    g_free (used);
}

/* Check the latest memory fraction against MEM_NOTIFY_THRESHOLDS and fire a
 * notification the first time each one is crossed. Re-arms (so a later
 * crossing notifies again) once usage falls comfortably below the lowest
 * threshold; see MEM_NOTIFY_HYSTERESIS. */
static void
check_mem_notify (SystemMonitor *sm, gdouble mem_frac)
{
    gint pct = (gint) (mem_frac * 100.0 + 0.5);
    gint reached = 0;
    guint i;

    for (i = 0; i < G_N_ELEMENTS (MEM_NOTIFY_THRESHOLDS); i++) {
        if (pct >= MEM_NOTIFY_THRESHOLDS[i])
            reached = MEM_NOTIFY_THRESHOLDS[i];
    }

    if (reached > sm->mem_notified_pct) {
        notify_mem_threshold (sm, reached);
        sm->mem_notified_pct = reached;
    }
    else if (pct < MEM_NOTIFY_THRESHOLDS[0] - MEM_NOTIFY_HYSTERESIS) {
        sm->mem_notified_pct = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Sampling timer                                                      */
/* ------------------------------------------------------------------ */

gboolean
update_cb (gpointer data)
{
    SystemMonitor *sm = data;
    gdouble cpu = 0.0, mem = 0.0, swap = 0.0, gpu_mem = 0.0;
    guint64 rx = 0, tx = 0;

    read_cpu (sm, &cpu);
    read_mem (sm, &mem, &swap);
    if (sm->mem_notify)
        check_mem_notify (sm, mem);
    if (sm->enabled[METRIC_GPU_MEM])
        read_gpu_mem (sm, &gpu_mem);

    /* Network: turn cumulative counters into download/upload rates (raw
     * bytes/sec). Each feeds its own metric, drawn against the fixed
     * logarithmic axis in net_log_frac(). A failed read leaves the previous
     * rates in place rather than pushing a spurious zero into the history,
     * which would also make the graph disagree with the tooltip. */
    if (read_net_totals (&rx, &tx)) {
        gdouble secs = sm->update_interval / 1000.0;
        if (secs <= 0.0)
            secs = 1.0;
        if (sm->net_have_prev) {
            sm->net_down = (rx >= sm->net_prev_rx)
                           ? (rx - sm->net_prev_rx) / secs : 0.0;
            sm->net_up   = (tx >= sm->net_prev_tx)
                           ? (tx - sm->net_prev_tx) / secs : 0.0;
        }
        sm->net_prev_rx = rx;
        sm->net_prev_tx = tx;
        sm->net_have_prev = TRUE;
    }

    sm->current[METRIC_CPU] = cpu;
    sm->current[METRIC_MEM] = mem;
    sm->current[METRIC_NET_DOWN] = sm->net_down;
    sm->current[METRIC_NET_UP] = sm->net_up;
    sm->current[METRIC_SWAP] = swap;
    sm->current[METRIC_GPU_MEM] = gpu_mem;
    history_push (sm);

    gtk_widget_queue_draw (sm->area);

    /* Refresh the tooltip only while it is actually on screen; this re-fires
     * query-tooltip (which reads freq/temps) just for a visible tooltip. */
    gtk_widget_trigger_tooltip_query (sm->ebox);
    return G_SOURCE_CONTINUE;
}

void
restart_timer (SystemMonitor *sm)
{
    if (sm->timeout_id != 0)
        g_source_remove (sm->timeout_id);
    sm->timeout_id = g_timeout_add (sm->update_interval, update_cb, sm);
}

/* ------------------------------------------------------------------ */
/* Tooltip (built on demand, only while it is visible)                 */
/* ------------------------------------------------------------------ */

/* Compose the tooltip from the values already sampled for the graphs, plus
 * tooltip-only extras (CPU frequency, temperatures) that are read here so they
 * are fetched only when the tooltip is actually shown. Caller must g_free()
 * the returned string. */
static gchar *
build_tooltip_text (SystemMonitor *sm)
{
    GString *tip = g_string_new (NULL);

    /* Usage lines, one per shown graph. */
    if (sm->enabled[METRIC_CPU]) {
        guint ncpu = (guint) g_get_num_processors ();
        gdouble freq = read_cpu_freq ();          /* MHz, 0 if unavailable */
        gdouble pct = sm->current[METRIC_CPU] * 100.0;

        /* One whole sentence per msgid. Assembling the line from fragments
         * leaves a translator with an unbalanced parenthesis and no way to
         * reorder the clauses, which many languages need. */
        if (freq > 0.0)
            g_string_append_printf (tip, _("CPU: %.0f%%  (%u cores @ %.2f GHz)\n"),
                                    pct, ncpu, freq / 1000.0);
        else
            g_string_append_printf (tip, _("CPU: %.0f%%  (%u cores)\n"), pct, ncpu);
    }
    if (sm->enabled[METRIC_NET_DOWN] || sm->enabled[METRIC_NET_UP]) {
        g_string_append (tip, _("Network\n"));
        if (sm->enabled[METRIC_NET_DOWN]) {
            gchar *d = format_rate (sm->net_down);
            g_string_append_printf (tip, _("  Download: %s\n"), d);
            g_free (d);
        }
        if (sm->enabled[METRIC_NET_UP]) {
            gchar *u = format_rate (sm->net_up);
            g_string_append_printf (tip, _("  Upload: %s\n"), u);
            g_free (u);
        }
    }

    /* Memory section: RAM, swap and GPU memory together. */
    if (sm->enabled[METRIC_MEM] || sm->enabled[METRIC_SWAP] || sm->enabled[METRIC_GPU_MEM]) {
        g_string_append (tip, _("Memory\n"));
        if (sm->enabled[METRIC_MEM]) {
            gchar *u = format_size_kb (sm->mem_used_kb);
            gchar *t = format_size_kb (sm->mem_total_kb);
            gchar *ca = format_size_kb (sm->mem_cache_kb);
            g_string_append_printf (tip,
                _("  RAM: %.0f%% (%s / %s, cache %s)\n"),
                sm->current[METRIC_MEM] * 100.0, u, t, ca);
            g_free (u); g_free (t); g_free (ca);
        }
        if (sm->enabled[METRIC_SWAP]) {
            gchar *u = format_size_kb (sm->swap_used_kb);
            gchar *t = format_size_kb (sm->swap_total_kb);
            g_string_append_printf (tip, _("  Swap: %.0f%% (%s / %s)\n"),
                                    sm->current[METRIC_SWAP] * 100.0, u, t);
            g_free (u); g_free (t);
        }
        if (sm->enabled[METRIC_GPU_MEM] && sm->gpu_mem_total_kb > 0) {
            gchar *u = format_size_kb (sm->gpu_mem_used_kb);
            gchar *t = format_size_kb (sm->gpu_mem_total_kb);
            g_string_append_printf (tip, _("  GPU: %.0f%% (%s / %s)\n"),
                                    sm->current[METRIC_GPU_MEM] * 100.0, u, t);
            g_free (u); g_free (t);
        }
    }

    /* Temperature section: independent of the graphs; shows what is present.
     * A thermal zone is the better CPU source when one names the package; the
     * single hwmon pass supplies the fallback and the disk/GPU figures. */
    {
        HwmonTemps hw;
        gdouble tc = read_thermal_zone_cpu_temp ();

        read_hwmon_temps (&hw);
        if (tc <= 0.0)
            tc = hw.cpu;
        if (tc > 0.0 || hw.disk > 0.0 || hw.gpu > 0.0) {
            g_string_append (tip, _("Temperature\n"));
            if (tc > 0.0)
                g_string_append_printf (tip, _("  CPU: %.0f\302\260C\n"), tc);
            if (hw.disk > 0.0)
                g_string_append_printf (tip, _("  Disk: %.0f\302\260C\n"), hw.disk);
            if (hw.gpu > 0.0)
                g_string_append_printf (tip, _("  GPU: %.0f\302\260C\n"), hw.gpu);
        }
    }

    if (tip->len > 0 && tip->str[tip->len - 1] == '\n')
        g_string_truncate (tip, tip->len - 1);
    return g_string_free (tip, FALSE);   /* hand ownership of the buffer back */
}

gboolean
query_tooltip_cb (GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                  GtkTooltip *tooltip, SystemMonitor *sm)
{
    gchar *text;
    gboolean show;

    (void) widget;
    (void) x;
    (void) y;
    (void) keyboard_mode;

    text = build_tooltip_text (sm);
    show = (text[0] != '\0');
    if (show)
        gtk_tooltip_set_text (tooltip, text);
    g_free (text);
    return show;
}
