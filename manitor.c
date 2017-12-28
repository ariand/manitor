/*
 * manitor -- Display system information on the desktop.
 * See LICENSE for copyright.
 */
#include <gtk/gtk.h>
#include <gio/gunixmounts.h>
#include <cairo.h>
#include <math.h>
#include <string.h>

#include "conf.h"
#include "info.h"

#define RAD(deg) ((deg) * G_PI / 180.0)
#define TAU (2 * G_PI)

#define FORMAT_BIG_BEGIN "<span size='xx-large' weight='light'>"
#define FORMAT_BIG_END "</span>"
#define FORMAT_BIG(strliteral) FORMAT_BIG_BEGIN strliteral FORMAT_BIG_END

typedef struct {
    /* Configuration */
    int monitor;                // Monitor number we want to appear on.
    int margin;                 // Margin to leave around the window on all sides.
    char *iface;                // Network interface to monitor.
    PangoFontDescription *font; // The font for most labels.
    GdkRGBA *color;             // Foreground color.
    GdkRGBA *alarm_color;       // Alarm color (used when CPU usage etc. is high).
    GdkRGBA *shade_color;       // Should be used as as background color.
    int interval;               // Update interval in seconds.

    /* The rest */
    GtkWidget *window;  // Yes, the window.
    Info *info;         // The monitored values.
} Manitor;

static Manitor *
manitor_new(void)
{
    Manitor *self = g_new0(Manitor, 1);
    self->monitor = CONF_MONITOR;
    self->margin = CONF_MARGIN;
    self->iface = g_strdup(CONF_IFACE);
    self->interval = MAX(1, CONF_INTERVAL);
    self->font = pango_font_description_from_string(CONF_FONT);
    self->color = g_new0(GdkRGBA, 1);
    self->shade_color = g_new0(GdkRGBA, 1);
    self->alarm_color = g_new0(GdkRGBA, 1);
    gdk_rgba_parse(self->color, CONF_COLOR);
    gdk_rgba_parse(self->shade_color, CONF_SHADE_COLOR);
    gdk_rgba_parse(self->alarm_color, CONF_ALARM_COLOR);

    self->info = info_new(CONF_IFACE);

    return self;
}

static void
manitor_place_window(Manitor *self)
{
    GdkScreen *scr = gtk_window_get_screen(GTK_WINDOW(self->window));
    GdkDisplay *dpy = gdk_screen_get_display(scr);

    // gdk_display_get_monitor(dpy, self->monitor) just segfaults for invalid
    // monitor numbers. API documentation says it is supposed to return NULL.
    // Can't sanitize your own input, GDK?
    int lastmon = gdk_display_get_n_monitors(dpy) - 1;
    if (lastmon < 0) {
            g_warning("Could not find any monitors");
            return;
    }

    int n = CLAMP(self->monitor, 0, lastmon);
    GdkMonitor *mon = gdk_display_get_monitor(dpy, n);
    if (!mon) {
        // Try the primary, then fall back to monitor 0 if we haven't tried it
        // already.
        mon = gdk_display_get_primary_monitor(dpy);
        if (!mon && n != 0) {
            mon = gdk_display_get_monitor(dpy, 0);
        }
        if (!mon) {
            g_warning("Could not find any monitors");
            return;
        }
    }

    GdkRectangle area;
    gdk_monitor_get_workarea(mon, &area);
    int w = area.width - 2 * self->margin;
    int h = area.height - 2 * self->margin;
    if (w < 0) w = area.width;
    if (h < 0) h = area.height;
    int x = area.x + (area.width - w) / 2;
    int y = area.y + (area.height - h) / 2;

    gtk_window_move(GTK_WINDOW(self->window), x, y);
    gtk_window_set_default_size(GTK_WINDOW(self->window), w, h);
    gtk_window_resize(GTK_WINDOW(self->window), w, h);
}

// Returns the baseline of line n (0 = first line).
// n can be negative (-1 = last line, -2 = last before, etc.).
// Returns 0 if there is no such line.
static int
get_baseline(PangoLayout *layout, int n)
{
    int retval = 0;
    if (n < 0) {
        n += pango_layout_get_line_count(layout);
        if (n < 0) return retval;
    }

    // Quick case -- we want the first line.
    if (n == 0) {
        retval = pango_layout_get_baseline(layout);
        return PANGO_PIXELS(retval);
    }

    // Find line n
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    while (n > 0 && pango_layout_iter_next_line(iter)) { n--; }

    if (n == 0) {
        retval = pango_layout_iter_get_baseline(iter);
        retval = PANGO_PIXELS(retval);
    }
    pango_layout_iter_free(iter);
    return retval;
}

// Gets the pixel extents of line n in the layout.
// n can be negative to count from the end.
/*
static gboolean
get_line_pixel_extents(PangoLayout *layout, int n, PangoRectangle *ink, PangoRectangle *logical)
{
    if (n < 0) {
        n += pango_layout_get_line_count(layout);
    }

    PangoLayoutLine *line = pango_layout_get_line_readonly(layout, n);
    if (line) {
        pango_layout_line_get_pixel_extents(line, ink, logical);
        return TRUE;
    }

    return FALSE;
}
*/

// Show a layout such that its alignment point (selected by ha and va)
// is at (x, y).
//
// ha: Horizontal alignment from 0 (left) to 1 (right).
// va: Vertical alignment from 0 (top) to 1 (bottom).
//     Negative integers select the baseline of line |va|.
static void
show_layout(cairo_t *cr, PangoLayout *layout, double x, double y, double ha, double va)
{
    ha = CLAMP(ha, 0, 1);
    if (va > 1) va = 1; else if (va < 0) va = floor(va);

    PangoRectangle ex;
    pango_layout_get_pixel_extents(layout, NULL, &ex);

    x += trunc(ex.x - ha * ex.width);
    if (va >= 0) {
        y += trunc(ex.y - va * ex.height);
    } else {
        // Align to baseline. get_baseline() is 0-based, hence the -1.
        y -= get_baseline(layout, ((int) -va) - 1);
    }

    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
}

// x, y: Coordinates of the center.
// radius: Yep.
// value: The value to display (a fraction in the range [0, 1]).
// angle1: Start angle (degrees).
// angle2: End angle (degrees).
// alarm: Draw using the alarm color if value >= alarm. 0 to disable alarms.
static void
draw_ring(Manitor *self, cairo_t *cr, double value, double x, double y,
          double radius, double angle1, double angle2, double alarm)
{
    value = CLAMP(value, 0, 1);
    alarm = CLAMP(alarm, 0, 1);

    double a1 = RAD(angle1);
    double a2 = RAD(angle2);
    double a = a1 + value * (a2 - a1); // value angle

    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, (alarm == 0 || value < alarm) ? self->color : self->alarm_color);

    if (value > 0) {
        cairo_set_line_width(cr, 7);
        cairo_arc(cr, x + 0.5, y + 0.5, radius, MIN(a1, a), MAX(a1, a));
        cairo_stroke(cr);
    }

    if (value < 1) {
        cairo_set_line_width(cr, 1);
        cairo_arc(cr, x + 0.5, y + 0.5, radius, MIN(a, a2), MAX(a, a2));
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

static char *
format_uptime(guint64 uptime)
{
    double h = uptime / 3600;
    double m = (uptime % 3600) / 60;

    if (h == 0) {
        return g_strdup_printf(FORMAT_BIG("%.0f") " min", m);
    } else if (m == 0) {
        return g_strdup_printf(FORMAT_BIG("%.0f") " hr", h);
    }

    return g_strdup_printf(FORMAT_BIG("%.0f") " hr "
                           FORMAT_BIG("%.0f") " min", h, m);
}

static char *
format_netspeed(double speed)
{
    speed /= 1000;
    if (speed >= 10 || speed == 0) {
        return g_strdup_printf(FORMAT_BIG("%.0f") "<span fgalpha='1'>.0</span>", speed);
    } else {
        char *s = g_strdup_printf("%.1f", speed);
        char *dot = strchr(s, '.');
        *dot = '\0';
        char *retval = g_strdup_printf(FORMAT_BIG("%s") ".%s", s, dot + 1);
        g_free(s);
        return retval;
    }
}

static char *
format_size(double size)
{
    static const char *units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB"};
    static int units_max_index = G_N_ELEMENTS(units) - 1;

    int i = trunc(trunc(log10(fabs(size))) / 3);
    i = MIN(i, units_max_index);
    const char *unit = units[i];
    if (i > 0) {
        size /= pow(10, 3 * i);
    }

    if (size >= 10 || size == 0) {
        return g_strdup_printf(FORMAT_BIG("%.0f") " %s", size, unit);
    }

    char *s = g_strdup_printf("%.1f", size);
    char *dot = strchr(s, '.');
    *dot = '\0';
    char *retval = g_strdup_printf(FORMAT_BIG("%s") ".%s %s", s, dot + 1, unit);
    g_free(s);
    return retval;
}

// Gets called when the number, size or position of the monitors attached to
// the screen change.
static void
on_monitors_changed(GdkScreen *screen, Manitor *self)
{
    manitor_place_window(self);
}

static gboolean
on_tick(Manitor *self)
{
    info_update(self->info);
    gtk_widget_queue_draw(self->window);
    return TRUE;
}

static void
draw_clock(Manitor *self, cairo_t *cr, PangoLayout *layout, int window_width, int window_height)
{
    static int radius = 0;
    static int seconds_radius = 0;

    if (G_UNLIKELY(radius == 0)) {
        GDateTime *tm = g_date_time_new_local(2000, 1, 1, 20, 0, 59);
        char *tmstr = g_date_time_format(tm, CONF_CLOCK_FORMAT);
        pango_layout_set_markup(layout, tmstr, -1);
        g_free(tmstr);
        g_date_time_unref(tm);

        int w, h;
        pango_layout_get_pixel_size(layout, &w, &h);
        radius = MAX(w, h) / 2;

        pango_layout_set_markup(layout, "59", -1);
        pango_layout_get_pixel_size(layout, &w, &h);
        radius += 2 * MAX(w, h);
        seconds_radius = radius - MAX(w, h);
    }

    int cx = window_width / 2;
    int cy = window_height / 2;

    GDateTime *tm = info_get_time(self->info);
    if (G_UNLIKELY(!tm)) {
        pango_layout_set_markup(layout, "H:MM \360\237\230\222", -1);
        show_layout(cr, layout, cx, cy, 0.5, 0.5);
    }

    // Draw the background
    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, self->shade_color);
    cairo_arc(cr, cx, cy, radius, 0, TAU);
    cairo_fill(cr);
    cairo_restore(cr);

    // Show the time
    char *s = g_date_time_format(tm, CONF_CLOCK_FORMAT);
    pango_layout_set_markup(layout, s, -1);
    g_free(s);
    show_layout(cr, layout, cx, cy, 0.5, 0.5);

    // Show the seconds circling around
    char buf[20];
    int sec = g_date_time_get_second(tm);
    g_snprintf(buf, sizeof(buf), "%02d", sec);
    double fraction = ((double) sec) / 60.0;
    pango_layout_set_markup(layout, buf, -1);
    show_layout(cr, layout,
                cx + seconds_radius * cos(RAD(-90) + fraction * TAU),
                cy + seconds_radius * sin(RAD(-90) + fraction * TAU),
                0.5, 0.5);
}

static void
draw_mounts(Manitor *self, cairo_t *cr, PangoLayout *layout, int window_width, int window_height)
{
    GString *str = g_string_sized_new(1024);

    GPtrArray *mounts = info_get_mounts(self->info);
    for (guint i = 0; i < mounts->len; i++) {
        GUnixMountEntry *entry = mounts->pdata[i];
        const char *path = g_unix_mount_get_mount_path(entry);

        char *size = format_size(info_get_fs_free(self->info, path));
        g_string_append(str, size);
        g_string_append(str, " free\n");
        g_free(size);

        if (strcmp(path, "/") == 0) {
            g_string_append(str, "root");
        } else {
            char *name = g_unix_mount_guess_name(entry);
            g_string_append(str, name);
            g_free(name);
        }
        g_string_append(str, "\n\n");
    }

    PangoAlignment align = pango_layout_get_alignment(layout);
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    pango_layout_set_markup(layout, str->str, -1);
    show_layout(cr, layout, window_width - 1, 0, 1.0, 0.0);
    pango_layout_set_alignment(layout, align);

    g_string_free(str, TRUE);
}

static gboolean
on_draw(GtkWidget *widget, cairo_t *cr, Manitor *self)
{
    char buf[256];

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, self->font);
    gdk_cairo_set_source_rgba(cr, self->color);

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    int cx = width / 2;
    //int cy = height / 2;

    draw_clock(self, cr, layout, width, height);
    draw_mounts(self, cr, layout, width, height);

    // CPU
    double x = cx;
    double y = height - 1;
    double radius = 35;
    double gap = 15;
    {
        int ncpu = info_get_cpu_count(self->info);
        for (int i = 0; i < ncpu; i++) {
            int r = radius + i * gap;
            int cpu = ncpu - i - 1;
            draw_ring(self, cr, info_get_cpu_usage(self->info, cpu),
                      x, y, r, 180, 360, CONF_CPU_ALARM);
        }
        pango_layout_set_markup(layout, "CPU", -1);
        show_layout(cr, layout, x, y, 0.5, -1);
    }

    // Memory
    {
        double mem = info_get_mem(self->info);
        x = cx - (2 * radius + 3 * gap);
        draw_ring(self, cr, mem, x, y, radius, 180, 360, CONF_MEM_ALARM);
        pango_layout_set_markup(layout, "MEM", -1);
        show_layout(cr, layout, x, y, 0.5, -1);

        g_snprintf(buf, sizeof(buf), "%.0f%%", trunc(100 * mem));
        pango_layout_set_markup(layout, buf, -1);
        show_layout(cr, layout, x - radius - gap, y, 1, -1);
    }

    // Swap
    {
        double swp = info_get_swap(self->info);
        x = cx + (2 * radius + 3 * gap);
        draw_ring(self, cr, swp, x, y, radius, 180, 360, CONF_SWAP_ALARM);
        pango_layout_set_markup(layout, "SWAP", -1);
        show_layout(cr, layout, x, y, 0.5, -1);

        g_snprintf(buf, sizeof(buf), "%.0f%%", trunc(100 * swp));
        pango_layout_set_markup(layout, buf, -1);
        show_layout(cr, layout, x + radius + gap, y, 0, -1);
    }

    // Uptime
    {
        int x = 0;
        int y = height - 1;
        char *s = format_uptime(info_get_uptime(self->info));
        pango_layout_set_markup(layout, s, -1);
        g_free(s);
        show_layout(cr, layout, x, y, 0, -1);
    }

    // Net
    {
        int x = width - 1;
        int y = height - 1;
        char *up = format_netspeed(info_get_net_txspeed(self->info));
        char *dn = format_netspeed(info_get_net_rxspeed(self->info));
        char *s = g_strdup_printf("%s kB/s \360\237\240\211\n"
                                  "%s kB/s \360\237\240\213", up, dn);
        pango_layout_set_markup(layout, s, -1);
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
        show_layout(cr, layout, x, y, 1, -2);

        g_free(s);
        g_free(up);
        g_free(dn);
    }

    g_object_unref(layout);
    return TRUE;
}

int
main(int argc, char** argv)
{
    gtk_init(&argc, &argv);
    
    Manitor *self = manitor_new();
    self->window = (GtkWidget *) g_object_new(
        GTK_TYPE_WINDOW,
        "app-paintable", TRUE,
        "decorated", FALSE,
        "resizable", FALSE,
        "skip-pager-hint", TRUE,
        "skip-taskbar-hint", TRUE,
        "title", "Manitor",
        "type", GTK_WINDOW_TOPLEVEL,
        "type-hint", GDK_WINDOW_TYPE_HINT_DESKTOP,
        NULL);
    gtk_window_stick(GTK_WINDOW(self->window));
    gtk_window_set_keep_below(GTK_WINDOW(self->window), TRUE);

    {
        GdkScreen *scr = gtk_window_get_screen(GTK_WINDOW(self->window));
        GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
        if (vis) {
            gtk_widget_set_visual(GTK_WIDGET(self->window), vis);
        }
        g_signal_connect(G_OBJECT(scr), "monitors-changed", G_CALLBACK(on_monitors_changed), self);
    }

    // Clear the input shape to make mouse clicks go through the window.
    cairo_region_t *region = cairo_region_create();
    gtk_widget_input_shape_combine_region(GTK_WIDGET(self->window), region);
    cairo_region_destroy(region);

    g_signal_connect(G_OBJECT(self->window), "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(G_OBJECT(self->window), "draw", G_CALLBACK(on_draw), self);
    g_timeout_add(self->interval * 1000, (GSourceFunc) on_tick, self);

    info_update(self->info);
    manitor_place_window(self);
    gtk_widget_show(self->window);
    gtk_main();

    return 0;
}
