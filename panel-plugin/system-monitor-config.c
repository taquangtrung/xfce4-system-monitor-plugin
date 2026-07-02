/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor-config.c - Default values and persisted-config load/save
 * for the Xfce panel system monitor plugin.
 *
 * See LICENSE for the full licence text.
 */

#include <glib/gstdio.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>

#include "system-monitor.h"

#define DEFAULT_INTERVAL 2000  /* ms */
#define DEFAULT_COMMAND  "xfce4-taskmanager"

static const gchar *const DEFAULT_COLORS[] = {
    "#5fce4f",  /* CPU:      green                 */
    "#7fc0ff",  /* memory:   light sky blue        */
    "#ffb84d",  /* swap:     light amber           */
    "#c792ea",  /* GPU mem:  light violet          */
    "#a9b4ff",  /* download: light periwinkle blue */
    "#ff9e6b"   /* upload:   light coral orange    */
};

/* Stable short names used as config-key suffixes (color_cpu, enabled_cpu, ...). */
static const gchar *const METRIC_KEYS[] = {
    "cpu", "mem", "swap", "gpu_mem", "net_down", "net_up"
};

/* Adding a metric to MetricType must add a row to each table above. Sized by
 * N_METRICS these would silently gain a NULL slot instead, which only shows up
 * as a crash in gdk_rgba_parse() when the plugin loads. */
G_STATIC_ASSERT (G_N_ELEMENTS (DEFAULT_COLORS) == N_METRICS);
G_STATIC_ASSERT (G_N_ELEMENTS (METRIC_KEYS) == N_METRICS);

/* Pick the click command: prefer gnome-system-monitor, then fall back to
 * whatever common monitor is installed, and finally to a fixed default. */
static gchar *
default_monitor_command (void)
{
    static const gchar *const candidates[] = {
        "gnome-system-monitor",
        "xfce4-taskmanager",
        "mate-system-monitor",
        "plasma-systemmonitor",
        "ksysguard",
        "lxtask",
        NULL
    };
    guint i;

    for (i = 0; candidates[i] != NULL; i++) {
        gchar *path = g_find_program_in_path (candidates[i]);
        if (path != NULL) {
            g_free (path);
            return g_strdup (candidates[i]);
        }
    }
    return g_strdup (DEFAULT_COMMAND);
}

void
apply_defaults (SystemMonitor *sm)
{
    MetricType m;

    sm->update_interval = DEFAULT_INTERVAL;
    sm->show_border = FALSE;
    sm->mem_notify = TRUE;
    g_free (sm->command);
    sm->command = default_monitor_command ();
    for (m = 0; m < N_METRICS; m++) {
        gdk_rgba_parse (&sm->color[m], DEFAULT_COLORS[m]);
        sm->enabled[m] = TRUE;
    }
}

void
load_config (SystemMonitor *sm)
{
    gchar *file;
    XfceRc *rc = NULL;
    MetricType m;

    apply_defaults (sm);

    file = xfce_panel_plugin_lookup_rc_file (sm->plugin);
    if (file) {
        rc = xfce_rc_simple_open (file, TRUE);
        g_free (file);
    }
    if (!rc)
        return;

    sm->update_interval = xfce_rc_read_int_entry (rc, "interval", DEFAULT_INTERVAL);
    if (sm->update_interval < 100)
        sm->update_interval = 100;
    sm->show_border = xfce_rc_read_bool_entry (rc, "border", FALSE);
    sm->mem_notify = xfce_rc_read_bool_entry (rc, "mem_notify", TRUE);

    {
        /* Only override the auto-detected default if a value was saved. */
        const gchar *cmd = xfce_rc_read_entry (rc, "command", NULL);
        if (cmd != NULL) {
            g_free (sm->command);
            sm->command = g_strdup (cmd);
        }
    }

    for (m = 0; m < N_METRICS; m++) {
        gchar key[24];
        const gchar *s;

        g_snprintf (key, sizeof key, "color_%s", METRIC_KEYS[m]);
        s = xfce_rc_read_entry (rc, key, NULL);
        if (s)
            gdk_rgba_parse (&sm->color[m], s);

        g_snprintf (key, sizeof key, "enabled_%s", METRIC_KEYS[m]);
        sm->enabled[m] = xfce_rc_read_bool_entry (rc, key, TRUE);
    }

    xfce_rc_close (rc);
}

void
save_config (XfcePanelPlugin *plugin, SystemMonitor *sm)
{
    gchar *file;
    XfceRc *rc;
    MetricType m;

    file = xfce_panel_plugin_save_location (plugin, TRUE);
    if (!file)
        return;
    rc = xfce_rc_simple_open (file, FALSE);
    g_free (file);
    if (!rc)
        return;

    xfce_rc_write_int_entry (rc, "interval", (gint) sm->update_interval);
    xfce_rc_write_bool_entry (rc, "border", sm->show_border);
    xfce_rc_write_bool_entry (rc, "mem_notify", sm->mem_notify);
    xfce_rc_write_entry (rc, "command", sm->command ? sm->command : "");
    for (m = 0; m < N_METRICS; m++) {
        gchar key[24];
        gchar *s = gdk_rgba_to_string (&sm->color[m]);

        g_snprintf (key, sizeof key, "color_%s", METRIC_KEYS[m]);
        xfce_rc_write_entry (rc, key, s);
        g_free (s);

        g_snprintf (key, sizeof key, "enabled_%s", METRIC_KEYS[m]);
        xfce_rc_write_bool_entry (rc, key, sm->enabled[m]);
    }

    xfce_rc_close (rc);
}

/* Delete this plugin instance's saved settings file so the plugin falls back
 * to the compiled-in defaults. Both the writable save location and any file
 * found via the XDG search path are removed, to be safe. */
void
remove_config_file (SystemMonitor *sm)
{
    gchar *file;

    file = xfce_panel_plugin_save_location (sm->plugin, FALSE);
    if (file != NULL) {
        g_unlink (file);
        g_free (file);
    }

    file = xfce_panel_plugin_lookup_rc_file (sm->plugin);
    if (file != NULL) {
        g_unlink (file);
        g_free (file);
    }
}
