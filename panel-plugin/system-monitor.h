/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor.h - Shared types and configuration state for the Xfce panel
 * system monitor plugin.
 *
 * See LICENSE for the full licence text.
 */

#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

/* The metrics tracked by the plugin, in draw order. */
typedef enum
{
    METRIC_CPU = 0,
    METRIC_MEM,
    METRIC_SWAP,
    METRIC_GPU_MEM,
    METRIC_NET_DOWN,
    METRIC_NET_UP,
    N_METRICS
} MetricType;

/* Length of the per-metric history ring buffer. Kept large enough that a
 * wide panel still has one sample per pixel column. */
#define HISTORY_SIZE 256

/* px: left/right margin around the whole graph row. Shared between the
 * layout code (system-monitor.c) and the drawing code (system-monitor-
 * draw.c), which must agree on how much space the layout reserved. */
#define GRAPH_PAD_SIDE 3

/* Minimum time between nvidia-smi polls in read_gpu_mem() (system-monitor-
 * sensors.c), regardless of update_interval: it spawns a subprocess, so a
 * fast sample rate must not spawn it on every tick. Also used by
 * system_monitor_construct() to pre-charge the throttle so the first tick
 * polls immediately. */
#define MIN_NVIDIA_POLL_MS 5000

/* Highest temp1_input (°C) per category among the hwmon chips, or -1 for a
 * category with no matching chip. Filled by read_hwmon_temps() in
 * system-monitor-fs.c, consumed by the tooltip in system-monitor-
 * draw.c. */
typedef struct
{
    gdouble cpu;
    gdouble disk;
    gdouble gpu;
} HwmonTemps;

typedef struct
{
    /* Panel integration. */
    XfcePanelPlugin *plugin;
    GtkWidget       *ebox;   /* windowless event box (for transparency) */
    GtkWidget       *area;   /* single drawing area holding all squares  */

    /* Configuration (persisted). */
    guint    update_interval;      /* milliseconds between samples */
    GdkRGBA  color[N_METRICS];     /* fill colour per metric         */
    gboolean enabled[N_METRICS];   /* whether each square is shown   */
    gboolean show_border;          /* draw a 1px frame per square    */
    gchar   *command;              /* launched on left-click         */
    gboolean mem_notify;           /* fire desktop notifications on high memory use */

    /* Runtime state. */
    guint      timeout_id;
    GtkWidget *config_dialog;      /* borrowed: the open properties dialog,
                                    * or NULL. Destroyed in free_cb() so its
                                    * callbacks cannot outlive this struct. */
    gint    box_w;                 /* per-tile width in pixels        */
    gint    box_h;                 /* per-tile height in pixels       */

    /* Per-metric history ring buffers. CPU/memory/swap/GPU memory hold ratios
     * in [0, 1]; the two network metrics hold raw bytes/sec and are mapped at
     * draw time onto a fixed logarithmic axis (see net_log_frac()). */
    gdouble history[N_METRICS][HISTORY_SIZE];
    guint   history_head;          /* index of next slot to write    */
    gdouble current[N_METRICS];    /* latest value                   */

    /* CPU sampling accumulators from /proc/stat. */
    guint64 cpu_prev_total;
    guint64 cpu_prev_idle;
    gboolean cpu_have_prev;

    /* Latest absolute memory/swap figures (kB), for the tooltip. */
    guint64 mem_used_kb;
    guint64 mem_total_kb;
    guint64 mem_cache_kb;   /* buffers + cached */
    guint64 swap_used_kb;
    guint64 swap_total_kb;
    guint64 gpu_mem_used_kb;
    guint64 gpu_mem_total_kb;

    /* State for the nvidia-smi fallback in read_gpu_mem(). The proprietary
     * NVIDIA driver exposes no VRAM sysfs node, so the figures come from a
     * subprocess, spawned asynchronously so a slow nvidia-smi start cannot
     * stall the panel.
     *   gpu_poll_accum_ms  ms accumulated since the last poll, independent of
     *                      update_interval (see MIN_NVIDIA_POLL_MS)
     *   gpu_probe_failures consecutive failed probes; probing stops for good
     *                      at MAX_NVIDIA_PROBE_FAILURES (no NVIDIA GPU here)
     *   gpu_poll_pending   a probe is in flight; do not start a second one
     *   gpu_cancellable    owned; cancelled in free_cb() so an in-flight probe
     *                      cannot write into a freed plugin */
    guint         gpu_poll_accum_ms;
    guint         gpu_probe_failures;
    gboolean      gpu_poll_pending;
    GCancellable *gpu_cancellable;

    /* Highest memory-usage percentage threshold (see MEM_NOTIFY_THRESHOLDS)
     * already notified for the current high-usage episode; 0 = none yet. */
    gint    mem_notified_pct;

    /* Network sampling from /proc/net/dev. The per-second rates feed the
     * METRIC_NET_DOWN / METRIC_NET_UP history; these fields hold the previous
     * cumulative counters and the latest rates (for the tooltip). */
    guint64  net_prev_rx;
    guint64  net_prev_tx;
    gboolean net_have_prev;
    gdouble  net_down;    /* latest receive rate,  bytes/sec */
    gdouble  net_up;      /* latest transmit rate, bytes/sec */
} SystemMonitor;

/* system-monitor-fs.c */
gboolean read_cpu (SystemMonitor *sm, gdouble *out);
gdouble  read_cpu_freq (void);
gdouble  read_thermal_zone_cpu_temp (void);
void     read_hwmon_temps (HwmonTemps *out);
void     read_gpu_mem (SystemMonitor *sm, gdouble *out);
void     read_mem (SystemMonitor *sm, gdouble *mem_out, gdouble *swap_out);
gboolean read_net_totals (guint64 *rx_out, guint64 *tx_out);
gchar   *format_size_kb (guint64 kb);
gchar   *format_rate (gdouble bps);

/* system-monitor-draw.c */
gboolean draw_cb (GtkWidget *widget, cairo_t *cr, SystemMonitor *sm);
gboolean query_tooltip_cb (GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                            GtkTooltip *tooltip, SystemMonitor *sm);
gboolean update_cb (gpointer data);
void     restart_timer (SystemMonitor *sm);

/* system-monitor-config.c */
void apply_defaults (SystemMonitor *sm);
void load_config (SystemMonitor *sm);
void save_config (XfcePanelPlugin *plugin, SystemMonitor *sm);
void remove_config_file (SystemMonitor *sm);

/* system-monitor-dialog.c */
void configure_cb (XfcePanelPlugin *plugin, SystemMonitor *sm);

/* system-monitor.c */
void relayout (SystemMonitor *sm);

G_END_DECLS

#endif /* SYSTEM_MONITOR_H */
