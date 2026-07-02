/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor.c - Xfce panel plugin showing CPU, memory, swap, GPU memory
 * and network usage as separate square history graphs drawn on a transparent
 * background.
 *
 * This is an independent implementation; no code is derived from any other
 * panel plugin. See LICENSE for the full licence text.
 *
 * Plugin construction and teardown, left-click handling and panel geometry
 * live here. Sensor reading is in system-monitor-fs.c, rendering and
 * the sampling timer in system-monitor-draw.c, config persistence in
 * system-monitor-config.c, and the properties dialog in
 * system-monitor-dialog.c.
 */

#include <gio/gio.h>
#include <libnotify/notify.h>
#include <libxfce4util/libxfce4util.h>
#include <libxfce4panel/libxfce4panel.h>

#include "system-monitor.h"

#define MIN_SIDE 8     /* px */

/* ------------------------------------------------------------------ */
/* Left-click action                                                   */
/* ------------------------------------------------------------------ */

static void
launch_monitor (SystemMonitor *sm)
{
    GError *error = NULL;

    if (!sm->command || sm->command[0] == '\0')
        return;
    if (!g_spawn_command_line_async (sm->command, &error)) {
        g_warning ("system-monitor: could not launch \"%s\": %s",
                   sm->command, error->message);
        g_error_free (error);
    }
}

static gboolean
button_pressed (GtkWidget *widget, GdkEventButton *event, SystemMonitor *sm)
{
    (void) widget;

    /* Left-click launches the configured monitor; other buttons fall through
     * so the panel's own context menu (right-click) still works. */
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        launch_monitor (sm);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Layout / panel geometry                                             */
/* ------------------------------------------------------------------ */

static gint
count_enabled (const SystemMonitor *sm)
{
    gint n = 0;
    MetricType m;
    for (m = 0; m < N_METRICS; m++)
        if (sm->enabled[m])
            n++;
    return n;
}

static gboolean
size_changed (XfcePanelPlugin *plugin, gint size, SystemMonitor *sm)
{
    gint nrows = xfce_panel_plugin_get_nrows (plugin);
    /* "size" is the whole panel's thickness, so one row gets a 1/nrows share.
     * Both tile dimensions come from that share, which is what keeps the
     * graphs square; deriving only the thickness from it made every tile
     * nrows times too long on a multi-row panel. */
    gint side = (nrows > 0) ? size / nrows : size;
    gint thick = MAX (side, MIN_SIDE);       /* across the panel        */
    gint along = MAX (side - 2, MIN_SIDE);   /* along it, slightly inset */
    GtkOrientation o = xfce_panel_plugin_get_orientation (plugin);
    gint n = count_enabled (sm);

    if (o == GTK_ORIENTATION_HORIZONTAL) {
        sm->box_w = along;
        sm->box_h = thick;
    } else {
        sm->box_w = thick;
        sm->box_h = along;
    }

    if (n < 1)
        n = 1;   /* keep a minimal footprint when nothing is selected */

    if (o == GTK_ORIENTATION_HORIZONTAL)
        gtk_widget_set_size_request (sm->area, sm->box_w * n + GRAPH_PAD_SIDE * 2, sm->box_h);
    else
        gtk_widget_set_size_request (sm->area, sm->box_w + GRAPH_PAD_SIDE * 2, sm->box_h * n);

    return TRUE;
}

/* Recompute the widget geometry after the set of enabled metrics changes. */
void
relayout (SystemMonitor *sm)
{
    size_changed (sm->plugin, xfce_panel_plugin_get_size (sm->plugin), sm);
    gtk_widget_queue_draw (sm->area);
}

static void
mode_changed (XfcePanelPlugin *plugin, XfcePanelPluginMode mode, SystemMonitor *sm)
{
    (void) mode;
    size_changed (plugin, xfce_panel_plugin_get_size (plugin), sm);
    gtk_widget_queue_draw (sm->area);
}

/* ------------------------------------------------------------------ */
/* Construction / teardown                                             */
/* ------------------------------------------------------------------ */

static void
free_cb (XfcePanelPlugin *plugin, SystemMonitor *sm)
{
    (void) plugin;
    if (sm->timeout_id != 0)
        g_source_remove (sm->timeout_id);

    /* The properties dialog is a toplevel, so nothing else tears it down when
     * the plugin goes away (DESTROY_WITH_PARENT tracks the panel window, not
     * the plugin). Left alive, every one of its callbacks would dereference
     * this freed struct. gtk_widget_destroy() does not emit "response", so
     * dialog_response() will not run against freed memory either. */
    if (sm->config_dialog != NULL)
        gtk_widget_destroy (sm->config_dialog);

    /* Cancel before freeing: an in-flight nvidia-smi probe holds a pointer to
     * sm, and nvidia_vram_ready() returns early once the operation is
     * cancelled. The async call keeps its own ref on the cancellable. */
    g_cancellable_cancel (sm->gpu_cancellable);
    g_object_unref (sm->gpu_cancellable);

    g_free (sm->command);
    g_free (sm);
}

static void
system_monitor_construct (XfcePanelPlugin *plugin)
{
    SystemMonitor *sm = g_new0 (SystemMonitor, 1);

    xfce_textdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");

    sm->plugin = plugin;
    sm->gpu_cancellable = g_cancellable_new ();

    /* Pre-charge the throttle so the first tick polls immediately, rather than
     * leaving the GPU graph blank until MIN_NVIDIA_POLL_MS has elapsed. */
    sm->gpu_poll_accum_ms = MIN_NVIDIA_POLL_MS;
    load_config (sm);

    /* Guarded: xfce4-panel loads one instance per configured plugin slot, so
     * multiple SystemMonitor instances can share this process. libnotify is
     * a process-wide singleton; skip re-init (and never uninit it here,
     * since other instances may still be using it) once any instance has
     * connected. */
    if (!notify_is_initted ())
        notify_init (GETTEXT_PACKAGE);

    sm->ebox = gtk_event_box_new ();
    gtk_event_box_set_visible_window (GTK_EVENT_BOX (sm->ebox), FALSE);
    gtk_event_box_set_above_child (GTK_EVENT_BOX (sm->ebox), FALSE);
    gtk_widget_set_has_tooltip (sm->ebox, TRUE);
    g_signal_connect (sm->ebox, "query-tooltip", G_CALLBACK (query_tooltip_cb), sm);

    sm->area = gtk_drawing_area_new ();
    gtk_widget_set_app_paintable (sm->area, TRUE);
    gtk_container_add (GTK_CONTAINER (sm->ebox), sm->area);

    gtk_container_add (GTK_CONTAINER (plugin), sm->ebox);
    gtk_widget_show_all (sm->ebox);

    g_signal_connect (sm->area, "draw", G_CALLBACK (draw_cb), sm);

    /* The drawing area owns a real GdkWindow, so button events land on it;
     * register it (not the windowless event box) so a right-click pops up the
     * standard panel menu, which includes Move, Remove and Properties. */
    gtk_widget_add_events (sm->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    xfce_panel_plugin_add_action_widget (plugin, sm->area);
    g_signal_connect (sm->area, "button-press-event",
                      G_CALLBACK (button_pressed), sm);
    xfce_panel_plugin_menu_show_configure (plugin);

    g_signal_connect (plugin, "free-data", G_CALLBACK (free_cb), sm);
    g_signal_connect (plugin, "save", G_CALLBACK (save_config), sm);
    g_signal_connect (plugin, "size-changed", G_CALLBACK (size_changed), sm);
    g_signal_connect (plugin, "mode-changed", G_CALLBACK (mode_changed), sm);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (configure_cb), sm);

    size_changed (plugin, xfce_panel_plugin_get_size (plugin), sm);
    update_cb (sm);        /* prime CPU accumulators / first sample */
    restart_timer (sm);
}

XFCE_PANEL_PLUGIN_REGISTER (system_monitor_construct)
