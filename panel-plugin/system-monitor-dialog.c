/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Quang Trung Ta
 *
 * system-monitor-dialog.c - The properties dialog for the Xfce panel
 * system monitor plugin.
 *
 * See LICENSE for the full licence text.
 */

#include <glib/gi18n-lib.h>
#include <libxfce4panel/libxfce4panel.h>

#include "system-monitor.h"

static const gchar *const METRIC_LABELS[] = {
    N_("CPU"), N_("Memory"), N_("Swap"), N_("GPU Memory"), N_("Network Download"), N_("Network Upload")
};

/* Adding a metric to MetricType must add a row to this table. Sized by
 * N_METRICS it would silently gain a NULL slot instead, which only shows up
 * as a crash when the plugin loads. */
G_STATIC_ASSERT (G_N_ELEMENTS (METRIC_LABELS) == N_METRICS);

/* References to the editable widgets, so "Reset to defaults" can refresh
 * them. Allocated per dialog and freed when the dialog is destroyed. */
typedef struct
{
    SystemMonitor *sm;
    GtkWidget   *interval_spin;
    GtkWidget   *command_entry;
    GtkWidget   *border_check;
    GtkWidget   *mem_notify_check;
    GtkWidget   *enable_check[N_METRICS];
    GtkWidget   *color_btn[N_METRICS];
    gboolean     forget;   /* Reset was clicked: drop the saved config file */
} DialogWidgets;

static void
interval_changed (GtkSpinButton *spin, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    dw->forget = FALSE;
    sm->update_interval = (guint) gtk_spin_button_get_value_as_int (spin);
    restart_timer (sm);
    save_config (sm->plugin, sm);
}

/* Updates sm->command on every keystroke (so the click action is live
 * immediately) but does not save; saving happens on focus-out below, so
 * typing a command does not write the config file on every character. */
static void
command_changed (GtkEntry *entry, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    dw->forget = FALSE;
    g_free (sm->command);
    sm->command = g_strdup (gtk_entry_get_text (entry));
}

static gboolean
command_focus_out (GtkWidget *widget, GdkEventFocus *event, DialogWidgets *dw)
{
    (void) widget;
    (void) event;
    save_config (dw->sm->plugin, dw->sm);
    return FALSE;
}

static void
border_toggled (GtkToggleButton *btn, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    dw->forget = FALSE;
    sm->show_border = gtk_toggle_button_get_active (btn);
    gtk_widget_queue_draw (sm->area);
    save_config (sm->plugin, sm);
}

static void
mem_notify_toggled (GtkToggleButton *btn, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    dw->forget = FALSE;
    sm->mem_notify = gtk_toggle_button_get_active (btn);
    save_config (sm->plugin, sm);
}

static void
enabled_toggled (GtkToggleButton *btn, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    MetricType m = (MetricType) GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (btn), "metric"));
    dw->forget = FALSE;
    sm->enabled[m] = gtk_toggle_button_get_active (btn);
    relayout (sm);
    save_config (sm->plugin, sm);
}

static void
color_set (GtkColorButton *btn, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    MetricType m = (MetricType) GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (btn), "metric"));
    dw->forget = FALSE;
    gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (btn), &sm->color[m]);
    gtk_widget_queue_draw (sm->area);
    save_config (sm->plugin, sm);
}

static void
reset_defaults_clicked (GtkButton *btn, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;
    MetricType m;

    (void) btn;

    apply_defaults (sm);

    /* Push the defaults back into the widgets. Setting the spin and the
     * toggles re-emits their signals (which update sm and relayout); colour
     * buttons do not emit on programmatic change, so sm->color is already set
     * by apply_defaults above. */
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (dw->interval_spin),
                               sm->update_interval);
    gtk_entry_set_text (GTK_ENTRY (dw->command_entry),
                        sm->command ? sm->command : "");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dw->border_check),
                                  sm->show_border);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dw->mem_notify_check),
                                  sm->mem_notify);
    for (m = 0; m < N_METRICS; m++) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dw->enable_check[m]),
                                      sm->enabled[m]);
        gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (dw->color_btn[m]),
                                    &sm->color[m]);
    }

    /* Drop the saved preference file now. Set the flag last, because the
     * widget updates above re-emit change signals that clear it. Any edit
     * made afterwards clears the flag again and re-enables saving. */
    remove_config_file (sm);
    dw->forget = TRUE;

    restart_timer (sm);
    relayout (sm);
}

static void
dialog_response (GtkWidget *dialog, gint response, DialogWidgets *dw)
{
    SystemMonitor *sm = dw->sm;

    (void) response;

    xfce_panel_plugin_unblock_menu (sm->plugin);
    if (dw->forget)
        remove_config_file (sm);   /* reset: leave no saved preference */
    else
        save_config (sm->plugin, sm);
    sm->config_dialog = NULL;
    gtk_widget_destroy (dialog);
}

void
configure_cb (XfcePanelPlugin *plugin, SystemMonitor *sm)
{
    GtkWidget *dialog, *content, *grid, *w;
    DialogWidgets *dw;
    MetricType m;
    gint row = 0;

    xfce_panel_plugin_block_menu (plugin);

    dw = g_new0 (DialogWidgets, 1);
    dw->sm = sm;

    dialog = gtk_dialog_new_with_buttons (
        _("System Monitor"),
        GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        _("_Close"), GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_icon_name (GTK_WINDOW (dialog), "utilities-system-monitor");
    g_object_set_data_full (G_OBJECT (dialog), "dw", dw, g_free);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
    gtk_box_pack_start (GTK_BOX (content), grid, TRUE, TRUE, 0);

    /* Update interval. */
    w = gtk_label_new (_("Update interval (ms):"));
    gtk_widget_set_halign (w, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid), w, 0, row, 1, 1);
    w = gtk_spin_button_new_with_range (100, 10000, 100);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (w), sm->update_interval);
    g_signal_connect (w, "value-changed", G_CALLBACK (interval_changed), dw);
    gtk_grid_attach (GTK_GRID (grid), w, 1, row++, 1, 1);
    dw->interval_spin = w;

    /* Command launched on left-click. */
    w = gtk_label_new (_("Command on click:"));
    gtk_widget_set_halign (w, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid), w, 0, row, 1, 1);
    w = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (w), sm->command ? sm->command : "");
    gtk_widget_set_hexpand (w, TRUE);
    g_signal_connect (w, "changed", G_CALLBACK (command_changed), dw);
    g_signal_connect (w, "focus-out-event", G_CALLBACK (command_focus_out), dw);
    gtk_grid_attach (GTK_GRID (grid), w, 1, row++, 1, 1);
    dw->command_entry = w;

    /* Section header. */
    w = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (w), _("<b>Graphs to display</b>"));
    gtk_widget_set_halign (w, GTK_ALIGN_START);
    gtk_widget_set_margin_top (w, 6);
    gtk_grid_attach (GTK_GRID (grid), w, 0, row++, 2, 1);

    /* One row per metric: an enable checkbox plus its colour. The label sits
     * beside the check box as a separate widget, so clicking the text does
     * not toggle the box. */
    for (m = 0; m < N_METRICS; m++) {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *check = gtk_check_button_new ();
        GtkWidget *label = gtk_label_new (_(METRIC_LABELS[m]));

        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), sm->enabled[m]);
        g_object_set_data (G_OBJECT (check), "metric", GINT_TO_POINTER (m));
        g_signal_connect (check, "toggled", G_CALLBACK (enabled_toggled), dw);
        gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_widget_set_halign (box, GTK_ALIGN_START);
        gtk_grid_attach (GTK_GRID (grid), box, 0, row, 1, 1);
        dw->enable_check[m] = check;

        w = gtk_color_button_new_with_rgba (&sm->color[m]);
        gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (w), TRUE);
        g_object_set_data (G_OBJECT (w), "metric", GINT_TO_POINTER (m));
        g_signal_connect (w, "color-set", G_CALLBACK (color_set), dw);
        gtk_grid_attach (GTK_GRID (grid), w, 1, row++, 1, 1);
        dw->color_btn[m] = w;
    }

    /* Border toggle. The label is a separate widget (not the button's own
     * label) so that only the check box itself toggles, not the text. */
    {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *check = gtk_check_button_new ();
        GtkWidget *label = gtk_label_new (_("Draw a frame around each graph"));

        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), sm->show_border);
        g_signal_connect (check, "toggled", G_CALLBACK (border_toggled), dw);
        gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_grid_attach (GTK_GRID (grid), box, 0, row++, 2, 1);
        dw->border_check = check;
    }

    /* Memory notification toggle, same layout as the border toggle above. */
    {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *check = gtk_check_button_new ();
        GtkWidget *label = gtk_label_new (_("Notify when memory usage is high"));

        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), sm->mem_notify);
        g_signal_connect (check, "toggled", G_CALLBACK (mem_notify_toggled), dw);
        gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
        gtk_grid_attach (GTK_GRID (grid), box, 0, row++, 2, 1);
        dw->mem_notify_check = check;
    }

    /* Reset to defaults. */
    w = gtk_button_new_with_label (_("Reset to Defaults"));
    gtk_widget_set_halign (w, GTK_ALIGN_END);
    gtk_widget_set_margin_top (w, 6);
    g_signal_connect (w, "clicked", G_CALLBACK (reset_defaults_clicked), dw);
    gtk_grid_attach (GTK_GRID (grid), w, 0, row++, 2, 1);

    g_signal_connect (dialog, "response", G_CALLBACK (dialog_response), dw);
    sm->config_dialog = dialog;
    gtk_widget_show_all (dialog);
}
