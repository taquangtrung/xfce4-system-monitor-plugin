/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor-fs.c - /proc and /sys readers for the Xfce panel
 * system monitor plugin: CPU, memory, swap, GPU memory, network and
 * temperature figures.
 *
 * See LICENSE for the full licence text.
 */

#include <stdio.h>
#include <string.h>

#include <gio/gio.h>

#include "system-monitor.h"

/* Consecutive failed nvidia-smi probes after which probing stops for good.
 * A machine with no NVIDIA GPU (or without nvidia-smi installed) must not
 * keep spawning a doomed subprocess for the life of the panel session. */
#define MAX_NVIDIA_PROBE_FAILURES 3

/* ------------------------------------------------------------------ */
/* /proc readers                                                       */
/* ------------------------------------------------------------------ */

/* Compute the CPU busy fraction since the previous sample from /proc/stat. */
gboolean
read_cpu (SystemMonitor *sm, gdouble *out)
{
    FILE *f = fopen ("/proc/stat", "r");
    char line[256];
    unsigned long long user = 0, nice = 0, system = 0, idle = 0,
                       iowait = 0, irq = 0, softirq = 0, steal = 0;
    int n;

    *out = 0.0;
    if (!f)
        return FALSE;
    if (!fgets (line, sizeof line, f)) {
        fclose (f);
        return FALSE;
    }
    fclose (f);

    n = sscanf (line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4)
        return FALSE;

    {
        guint64 idle_all = (guint64) idle + iowait;
        guint64 total = (guint64) user + nice + system + idle
                        + iowait + irq + softirq + steal;
        gdouble usage = 0.0;

        if (sm->cpu_have_prev && total > sm->cpu_prev_total) {
            guint64 dtotal = total - sm->cpu_prev_total;
            guint64 didle = (idle_all > sm->cpu_prev_idle)
                            ? idle_all - sm->cpu_prev_idle : 0;
            usage = (gdouble) (dtotal - didle) / (gdouble) dtotal;
        }

        sm->cpu_prev_total = total;
        sm->cpu_prev_idle = idle_all;
        sm->cpu_have_prev = TRUE;
        *out = CLAMP (usage, 0.0, 1.0);
    }
    return TRUE;
}

/* Average current CPU frequency in MHz across all cores, or 0 if unavailable. */
gdouble
read_cpu_freq (void)
{
    GDir *dir = g_dir_open ("/sys/devices/system/cpu", 0, NULL);
    const gchar *name;
    gdouble sum = 0.0;
    guint n = 0;

    if (!dir)
        return 0.0;
    while ((name = g_dir_read_name (dir)) != NULL) {
        gchar *path, *data = NULL;
        if (strncmp (name, "cpu", 3) != 0 || !g_ascii_isdigit (name[3]))
            continue;
        path = g_strdup_printf ("/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", name);
        if (g_file_get_contents (path, &data, NULL, NULL)) {
            sum += (gdouble) g_ascii_strtoull (data, NULL, 10);   /* kHz */
            n++;
            g_free (data);
        }
        g_free (path);
    }
    g_dir_close (dir);
    return (n > 0) ? (sum / n) / 1000.0 : 0.0;   /* kHz -> MHz */
}

/* Read millidegree temperature from a sysfs file into °C; TRUE on success. */
static gboolean
read_temp_file (const gchar *path, gdouble *out)
{
    gchar *data = NULL;
    if (!g_file_get_contents (path, &data, NULL, NULL))
        return FALSE;
    *out = g_ascii_strtoll (data, NULL, 10) / 1000.0;
    g_free (data);
    return (*out > 0.0);
}

/* CPU package temperature in °C from /sys/class/thermal, or -1 if no zone
 * names the CPU. read_hwmon_temps() below covers the hwmon fallback. */
gdouble
read_thermal_zone_cpu_temp (void)
{
    GDir *dir = g_dir_open ("/sys/class/thermal", 0, NULL);
    const gchar *name;
    gdouble temp = -1.0;

    if (!dir)
        return -1.0;
    while ((name = g_dir_read_name (dir)) != NULL) {
        gchar *tpath, *type = NULL;
        gboolean match = FALSE;

        if (strncmp (name, "thermal_zone", 12) != 0)
            continue;
        tpath = g_strdup_printf ("/sys/class/thermal/%s/type", name);
        if (g_file_get_contents (tpath, &type, NULL, NULL)) {
            g_strchomp (type);
            if (g_strcmp0 (type, "x86_pkg_temp") == 0
                || g_str_has_prefix (type, "coretemp")
                || strstr (type, "cpu") != NULL)
                match = TRUE;
            g_free (type);
        }
        g_free (tpath);
        if (match) {
            gchar *vpath = g_strdup_printf ("/sys/class/thermal/%s/temp", name);
            gboolean ok = read_temp_file (vpath, &temp);
            g_free (vpath);
            if (ok)
                break;
        }
    }
    g_dir_close (dir);
    return temp;
}

/* Fill @out from a single pass over /sys/class/hwmon. One pass matters: the
 * tooltip rebuilds on every sample tick while it is on screen, and reading
 * each category separately meant re-walking the directory once per category. */
void
read_hwmon_temps (HwmonTemps *out)
{
    GDir *dir;
    const gchar *name;

    out->cpu = out->disk = out->gpu = -1.0;

    dir = g_dir_open ("/sys/class/hwmon", 0, NULL);
    if (!dir)
        return;

    while ((name = g_dir_read_name (dir)) != NULL) {
        gchar *npath = g_strdup_printf ("/sys/class/hwmon/%s/name", name);
        gchar *hname = NULL;
        gdouble *slot = NULL;   /* borrowed: points into *out, never freed */

        if (g_file_get_contents (npath, &hname, NULL, NULL)) {
            g_strchomp (hname);
            if (g_strcmp0 (hname, "coretemp") == 0
                || g_strcmp0 (hname, "k10temp") == 0
                || g_strcmp0 (hname, "zenpower") == 0
                || g_strcmp0 (hname, "cpu_thermal") == 0)
                slot = &out->cpu;
            else if (g_strcmp0 (hname, "nvme") == 0
                     || g_strcmp0 (hname, "drivetemp") == 0)
                slot = &out->disk;
            else if (g_strcmp0 (hname, "amdgpu") == 0
                     || g_strcmp0 (hname, "nouveau") == 0)
                slot = &out->gpu;   /* the proprietary NVIDIA driver and most
                                     * Intel iGPUs expose no hwmon sensor */
            g_free (hname);
        }
        g_free (npath);

        if (slot != NULL) {
            gchar *vpath = g_strdup_printf ("/sys/class/hwmon/%s/temp1_input", name);
            gdouble t;
            if (read_temp_file (vpath, &t) && t > *slot)
                *slot = t;
            g_free (vpath);
        }
    }
    g_dir_close (dir);
}

/* Read AMD GPU VRAM used/total (kB) from the amdgpu sysfs DRM card, if any.
 * TRUE on success. */
static gboolean
read_amdgpu_vram (guint64 *used_out, guint64 *total_out)
{
    GDir *dir = g_dir_open ("/sys/class/drm", 0, NULL);
    const gchar *name;
    gboolean found = FALSE;

    if (!dir)
        return FALSE;
    while (!found && (name = g_dir_read_name (dir)) != NULL) {
        gchar *driver_path, *driver_link;
        gchar *used_path, *total_path, *used_data = NULL, *total_data = NULL;

        if (strncmp (name, "card", 4) != 0 || !g_ascii_isdigit (name[4]))
            continue;

        driver_path = g_strdup_printf ("/sys/class/drm/%s/device/driver", name);
        driver_link = g_file_read_link (driver_path, NULL);
        g_free (driver_path);
        if (!driver_link)
            continue;
        if (!g_str_has_suffix (driver_link, "amdgpu")) {
            g_free (driver_link);
            continue;
        }
        g_free (driver_link);

        used_path = g_strdup_printf ("/sys/class/drm/%s/device/mem_info_vram_used", name);
        total_path = g_strdup_printf ("/sys/class/drm/%s/device/mem_info_vram_total", name);
        if (g_file_get_contents (used_path, &used_data, NULL, NULL)
            && g_file_get_contents (total_path, &total_data, NULL, NULL)) {
            *used_out = g_ascii_strtoull (used_data, NULL, 10) / 1024;   /* bytes -> kB */
            *total_out = g_ascii_strtoull (total_data, NULL, 10) / 1024;
            found = TRUE;
        }
        g_free (used_path);
        g_free (total_path);
        g_free (used_data);
        g_free (total_data);
    }
    g_dir_close (dir);
    return found;
}

/* Parse the "<used>, <total>" MiB pair printed by the nvidia-smi query in
 * poll_nvidia_vram_async() into kB. TRUE on success. */
static gboolean
parse_nvidia_vram (const gchar *text, guint64 *used_out, guint64 *total_out)
{
    unsigned long long used_mib = 0, total_mib = 0;

    if (text == NULL || sscanf (text, "%llu, %llu", &used_mib, &total_mib) != 2)
        return FALSE;

    *used_out = used_mib * 1024;    /* MiB -> kB */
    *total_out = total_mib * 1024;
    return TRUE;
}

/* Completion of the probe started by poll_nvidia_vram_async(). Runs on the
 * main loop, so it may touch the plugin freely, with one exception: on
 * cancellation the plugin has already been freed by free_cb(), so that path
 * must return without dereferencing @data. Owns the GSubprocess. */
static void
nvidia_vram_ready (GObject *source, GAsyncResult *res, gpointer data)
{
    GSubprocess *proc = G_SUBPROCESS (source);
    SystemMonitor *sm = data;
    gchar *out = NULL;
    GError *error = NULL;
    guint64 used = 0, total = 0;
    gboolean ok;

    ok = g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, &error);
    if (!ok && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free (error);
        g_object_unref (proc);
        return;   /* the plugin is gone; sm must not be touched */
    }
    g_clear_error (&error);

    sm->gpu_poll_pending = FALSE;
    if (ok && g_subprocess_get_successful (proc)
        && parse_nvidia_vram (out, &used, &total) && total > 0) {
        sm->gpu_mem_used_kb = used;
        sm->gpu_mem_total_kb = total;
        sm->gpu_probe_failures = 0;
    }
    else {
        sm->gpu_probe_failures++;
    }

    g_free (out);
    g_object_unref (proc);
}

/* Start an nvidia-smi query for VRAM usage. The proprietary NVIDIA driver
 * exposes no VRAM sysfs node, so a subprocess is the only source. It is run
 * asynchronously because nvidia-smi can take hundreds of milliseconds to
 * start, and a synchronous spawn on the main loop would stall the whole
 * panel. Results land in sm->gpu_mem_*_kb via nvidia_vram_ready(). */
static void
poll_nvidia_vram_async (SystemMonitor *sm)
{
    static const gchar *const argv[] = {
        "nvidia-smi", "--query-gpu=memory.used,memory.total",
        "--format=csv,noheader,nounits", NULL
    };
    GSubprocess *proc;

    proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                    | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (proc == NULL) {   /* nvidia-smi is not installed */
        sm->gpu_probe_failures++;
        return;
    }

    sm->gpu_poll_pending = TRUE;
    g_subprocess_communicate_utf8_async (proc, NULL, sm->gpu_cancellable,
                                         nvidia_vram_ready, sm);
}

/* GPU memory usage fraction, and absolute used/total (kB) for the tooltip.
 * AMD is a cheap sysfs read and is always current. NVIDIA needs the
 * subprocess above, so it is refreshed in the background at most every
 * MIN_NVIDIA_POLL_MS and the last known figures are reported in between;
 * probing stops for good after MAX_NVIDIA_PROBE_FAILURES failures. */
void
read_gpu_mem (SystemMonitor *sm, gdouble *out)
{
    guint64 used = 0, total = 0;

    if (!read_amdgpu_vram (&used, &total)) {
        if (sm->gpu_probe_failures < MAX_NVIDIA_PROBE_FAILURES) {
            sm->gpu_poll_accum_ms += sm->update_interval;
            if (!sm->gpu_poll_pending
                && sm->gpu_poll_accum_ms >= MIN_NVIDIA_POLL_MS) {
                sm->gpu_poll_accum_ms = 0;
                poll_nvidia_vram_async (sm);
            }
        }
        used = sm->gpu_mem_used_kb;
        total = sm->gpu_mem_total_kb;
    }

    sm->gpu_mem_used_kb = used;
    sm->gpu_mem_total_kb = total;
    *out = (total > 0) ? CLAMP ((gdouble) used / (gdouble) total, 0.0, 1.0) : 0.0;
}

/* Read memory and swap usage fractions from /proc/meminfo, and record the
 * absolute used/total figures (kB) on the plugin for the tooltip. */
void
read_mem (SystemMonitor *sm, gdouble *mem_out, gdouble *swap_out)
{
    FILE *f = fopen ("/proc/meminfo", "r");
    char line[256];
    unsigned long long mem_total = 0, mem_avail = 0, mem_free = 0,
                       buffers = 0, cached = 0,
                       swap_total = 0, swap_free = 0;
    gboolean have_avail = FALSE;
    unsigned long long v;

    *mem_out = 0.0;
    *swap_out = 0.0;
    if (!f)
        return;

    while (fgets (line, sizeof line, f)) {
        if (sscanf (line, "MemTotal: %llu", &v) == 1)
            mem_total = v;
        else if (sscanf (line, "MemAvailable: %llu", &v) == 1) {
            mem_avail = v;
            have_avail = TRUE;
        }
        else if (sscanf (line, "MemFree: %llu", &v) == 1)
            mem_free = v;
        else if (sscanf (line, "Buffers: %llu", &v) == 1)
            buffers = v;
        else if (sscanf (line, "Cached: %llu", &v) == 1)
            cached = v;
        else if (sscanf (line, "SwapTotal: %llu", &v) == 1)
            swap_total = v;
        else if (sscanf (line, "SwapFree: %llu", &v) == 1)
            swap_free = v;
    }
    fclose (f);

    if (mem_total > 0) {
        /* Prefer MemAvailable (kernel's estimate); fall back for old kernels. */
        guint64 available = have_avail ? mem_avail : (mem_free + buffers + cached);
        guint64 used = (mem_total > available) ? mem_total - available : 0;
        sm->mem_used_kb = used;
        sm->mem_total_kb = mem_total;
        sm->mem_cache_kb = buffers + cached;
        *mem_out = CLAMP ((gdouble) used / (gdouble) mem_total, 0.0, 1.0);
    }
    if (swap_total > 0) {
        guint64 used = (swap_total > swap_free) ? swap_total - swap_free : 0;
        sm->swap_used_kb = used;
        sm->swap_total_kb = swap_total;
        *swap_out = CLAMP ((gdouble) used / (gdouble) swap_total, 0.0, 1.0);
    }
}

/* Format a size given in kibibytes into a compact human-readable string.
 * Caller must g_free() the result. */
gchar *
format_size_kb (guint64 kb)
{
    gdouble v = (gdouble) kb;
    const gchar *unit = "KiB";

    if (v >= 1024.0 * 1024.0) {
        v /= 1024.0 * 1024.0;
        unit = "GiB";
    }
    else if (v >= 1024.0) {
        v /= 1024.0;
        unit = "MiB";
    }
    return g_strdup_printf ("%.1f %s", v, unit);
}

/* Read cumulative receive/transmit byte counters from /proc/net/dev,
 * summed over every interface except the loopback. */
gboolean
read_net_totals (guint64 *rx_out, guint64 *tx_out)
{
    FILE *f = fopen ("/proc/net/dev", "r");
    char line[512];
    guint64 rx_sum = 0, tx_sum = 0;

    if (!f)
        return FALSE;

    /* Skip the two header lines. */
    if (!fgets (line, sizeof line, f) || !fgets (line, sizeof line, f)) {
        fclose (f);
        return FALSE;
    }

    while (fgets (line, sizeof line, f)) {
        char *colon = strchr (line, ':');
        char *name = line;
        unsigned long long rx = 0, tx = 0;

        if (!colon)
            continue;
        *colon = '\0';
        while (*name == ' ')
            name++;
        if (strcmp (name, "lo") == 0)
            continue;

        /* rx bytes is field 1, tx bytes is field 9 after the colon. */
        if (sscanf (colon + 1,
                    "%llu %*s %*s %*s %*s %*s %*s %*s %llu",
                    &rx, &tx) == 2) {
            rx_sum += rx;
            tx_sum += tx;
        }
    }
    fclose (f);

    *rx_out = rx_sum;
    *tx_out = tx_sum;
    return TRUE;
}

/* Format a byte/second rate into a compact human-readable string. Binary
 * (1024-based) units, to match format_size_kb(). Caller must g_free(). */
gchar *
format_rate (gdouble bps)
{
    const gchar *unit = "B/s";
    gdouble v = bps;

    if (v >= 1024.0 * 1024.0) {
        v /= 1024.0 * 1024.0;
        unit = "MiB/s";
    }
    else if (v >= 1024.0) {
        v /= 1024.0;
        unit = "KiB/s";
    }
    return g_strdup_printf ("%.1f %s", v, unit);
}
