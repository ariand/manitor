/*
 * manitor -- Display system information on the desktop.
 * See LICENSE for copyright.
 */
#include <glib.h>
#include <gio/gunixmounts.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/statvfs.h>

#include "info.h"

#define MAX_CPUS 16

struct Cpu {
    int n; // Number of CPUs.

    double usage[MAX_CPUS];     // Usage as a fraction (0..1).
    guint64 used[MAX_CPUS];     // }-- These two are used to calculate
    guint64 total[MAX_CPUS];    // }   the usage.
};

struct Net {
    char *iface;        // The network interface to monitor.
    double rxspeed;     // Receive speed (bytes/s).
    double txspeed;     // Transmit speed (bytes/s).

    // These are used to calculate the speeds.
    guint64 rx;         // Received bytes.
    guint64 tx;         // Transmitted bytes.
    gint64 rx_time;     // The last time rx was updated (<0: rx is invalid).
    gint64 tx_time;     // The last time tx was updated (<0: tx is invalid).
};

struct Info {
    GDateTime *time;    // The current time.
    guint64 uptime;     // Uptime, in seconds.
    struct Cpu cpu;     // CPU usage.
    double mem;         // Memory used, as a fraction.
    double swap;        // Swap used, as a fraction.
    struct Net net;     // Network interface speeds.

    GPtrArray *mounts;      // An array of unix mounts.
    guint64 mounts_time;    // Mount timestamp.
};

static char *
skip_line(const char *s)
{
    while (*s && *s != '\n') s++;   // Skip to the end of line.
    return (char *) ((*s == '\n') ? s + 1 : s);
}

static char *
skip_token(const char *s)
{
    while (*s && *s != ' ' && *s != '\t' && *s != '\a' && *s != '\v') s++;
    return (char *) s;
}

static char *
skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\a' || *s == '\v') s++;
    return (char *) s;
}

static char *
read_file(const char *filename)
{
    char *buf = NULL;
    g_file_get_contents(filename, &buf, NULL, NULL);
    return buf;
}

Info *
info_new(const char *iface)
{
    Info *info = g_new0(Info, 1);
    info->net.iface = g_strdup(iface);
    info->net.rx_time = -1; // -1 to indicate that we don't have valid values.
    info->net.tx_time = -1; //
    return info;
}

void
info_free(Info *info)
{
    if (info) {
        if (info->time) {
            info->time = (g_date_time_unref(info->time), NULL);
        }
        if (info->mounts) {
            info->mounts = (g_ptr_array_free(info->mounts, TRUE), NULL);
        }
        info->net.iface = (g_free(info->net.iface), NULL);
        g_free(info);
    }
}

// Parses a CPU line in /proc/stat.
// lineptr is the address of a pointer to the current line, and will be updated
// on success.
// n is the CPU number.
// Returns TRUE on success (filling cpu), FALSE otherwise.
static inline gboolean
parse_cpu_line(struct Cpu *cpu, char **lineptr, int n)
{
    char *s = *lineptr;

    // Expect "cpuN".
    if (G_UNLIKELY(s[0] != 'c' || s[1] != 'p' || s[2] != 'u' || !g_ascii_isdigit(s[3])))
        return FALSE;

    s += 3; // Skip "cpu".

    // Look for a matching N in "cpuN".
    {
        char *end = NULL;
        guint64 i = g_ascii_strtoull(s, &end, 10);
        if (n != i || s == end || errno == ERANGE || errno == EINVAL) { 
            return FALSE;
        }
        s = end;
    }

    // strtoull eats leading whitespace, including '\n'!
    // We don't want to cross lines, so we skip whitespace ourselves.
    guint64 user = (s = skip_space(s), g_ascii_strtoull(s, &s, 10));
    guint64 nice = (s = skip_space(s), g_ascii_strtoull(s, &s, 10));
    guint64 sys = (s = skip_space(s), g_ascii_strtoull(s, &s, 10));
    guint64 idle = (s = skip_space(s), g_ascii_strtoull(s, &s, 10));
    guint64 iowait = (s = skip_space(s), g_ascii_strtoull(s, &s, 10));

    // Skip the rest of the line.
    s = skip_line(s);

    guint64 total = user + nice + sys + idle + iowait;
    guint64 used = total - idle - iowait;
    double usage = 0;

    // Have we seen this CPU? Then we have valid stats.
    if (cpu->n > n) {
        // Handle overflow
        if (used < cpu->used[n])
            cpu->used[n] = used;
        if (total < cpu->total[n])
            cpu->total[n] = total;

        guint64 diff_total = total - cpu->total[n];
        if (diff_total > 0) {
            guint64 diff_used = used - cpu->used[n];
            usage = (double) diff_used / (double) diff_total;
        }
    }

    cpu->usage[n] = usage;
    cpu->total[n] = total;
    cpu->used[n] = used;

    *lineptr = s;
    return TRUE;
}

static void
info_update_cpu(Info *info)
{
    char *buf = read_file("/proc/stat");
    char *s = buf ? buf : "";

    // Skip the global "cpu" line.
    if (g_str_has_prefix(s, "cpu") && g_ascii_isspace(s[3])) {
        s = skip_line(s);
    }

    // The number of CPUs handled during this update.
    int cpu_n = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        if (parse_cpu_line(&info->cpu, &s, i)) {
            cpu_n++;
        } else {
            break;
        }
    }

    g_free(buf);
    info->cpu.n = cpu_n;
}

static void
update_iface_speed(const char *iface, const char *filename,
                   double *speed, guint64 *bytes, gint64 *time)
{
    char *name = g_strdup_printf("/sys/class/net/%s/statistics/%s", iface, filename);
    char *buf = read_file(name);
    gint64 now = g_get_monotonic_time();

    if (buf) {
        guint64 value = g_ascii_strtoull(buf, NULL, 10);
        if (*time >= 0) { // Previous byte count is valid.
            double delta_seconds = ((double) now - (double) *time) / 1e6;
            // Make sure some time has elapsed.
            if (delta_seconds > 1e-3) {
                // Handle overflow.
                if (value < *bytes) {
                    *bytes = value;
                }
                *speed = (value - *bytes) / delta_seconds;
                *bytes = value;
                *time = now;
            } else { // No time has elapsed
                *speed = 0;
                *bytes = value;
                *time = now;
            }
        } else { // Previous byte count is NOT valid.
            *speed = 0;
            *bytes = value;
            *time = now;
        }
    } else { // Coult not read the byte count.
        *speed = 0;
        *time = -1;
        *bytes = 0;
    }

    g_free(buf);
    g_free(name);
}

static inline guint64
parse_meminfo_value(const char *meminfo, const char *name)
{
    guint64 val = 0;
    const char *start = meminfo;
    char *s = NULL;

    // Find name at the beginning of a line.
    // It would be wrong, for example, to find "SwapCached:" for "Cached:".
    while (TRUE) {
        s = strstr(start, name);
        if (G_UNLIKELY(!s)) {
            return val;
        }
        // Is it at the beginning of the line?
        if (s == start || s[-1] == '\n') {
            break; // Got it!
        }
        // Keep searching until the buffer is processed.
        start = skip_line(s);
        if (!*start) {
            return val;
        }
    }

    s = skip_token(s); // Skip the "Name:".
    s = skip_space(s); // Skip to the number.
    val = g_ascii_strtoull(s, &s, 10);
    s = skip_space(s); // Skip to the unit.
    switch (*s) {
    case 'k': val *= 1024; break;
    case 'M': val *= 1024 * 1024; break;
    }

    return val;
}

static void
info_update_mem_swap(Info *info)
{
    info->mem = 0;
    info->swap = 0;

    char *buf = read_file("/proc/meminfo");
    if (G_UNLIKELY(!buf)) {
        return;
    }

    guint64 memtotal = parse_meminfo_value(buf, "MemTotal:");
    guint64 memfree = parse_meminfo_value(buf, "MemFree:");
    guint64 shmem = parse_meminfo_value(buf, "Shmem:");
    guint64 srec = parse_meminfo_value(buf, "SReclaimable:");
    guint64 buffers = parse_meminfo_value(buf, "Buffers:");
    guint64 cached = parse_meminfo_value(buf, "Cached:");

    // Do what Conky does (memused - membuf = really used memory).
    // https://github.com/brndnmtthws/conky/blob/v1.10.3/src/linux.cc#L166
    guint64 memused = memtotal - memfree;
    guint64 membuf = (cached - shmem) + buffers + srec;

    if (memtotal && memused >= membuf) {
        info->mem = ((double) (memused - membuf)) / ((double) memtotal);
    }

    guint64 swaptotal = parse_meminfo_value(buf, "SwapTotal:");
    guint64 swapfree = parse_meminfo_value(buf, "SwapFree:");
    guint64 swapused = swaptotal - swapfree;
    if (swaptotal) {
        info->swap = (double) swapused / (double) swaptotal;
    }

    g_free(buf);
}

static void
info_update_mounts(Info *info)
{
    if (info->mounts) {
        // Looks like I can't rely on this. What a surprise.
        /*
        if (!g_unix_mounts_changed_since(info->mounts_time)) {
            //g_print("Mounts have NOT CHANGED since %lu\n", info->mounts_time);
            return info->mounts;
        }
        */
        g_ptr_array_set_size(info->mounts, 0);
    } else {
        info->mounts = g_ptr_array_new_full(10, (GDestroyNotify) g_unix_mount_free);
    }

    GList *mounts = g_unix_mounts_get(&info->mounts_time);
 
    for (GList *m = mounts; m != NULL; m = m->next) {
        GUnixMountEntry *entry = m->data;

        // Only interested in devices...
        const char *dev = g_unix_mount_get_device_path(entry);
        if (strncmp(dev, "/dev/", 5) != 0) {
            continue;
        }

        // ...and certain filesystems.
        const char *type = g_unix_mount_get_fs_type(entry);
        if ((strcmp(type, "ext2") != 0) &&
            (strcmp(type, "ext3") != 0) &&
            (strcmp(type, "ext4") != 0) &&
            (strcmp(type, "vfat") != 0) &&
            (strcmp(type, "ntfs") != 0) &&
            (strcmp(type, "ntfs-3g") != 0) &&
            (strcmp(type, "reiserfs") != 0)
        ) {
            continue;
        }

        g_ptr_array_add(info->mounts, g_unix_mount_copy(entry));
    }

    g_list_free_full(mounts, (GDestroyNotify) g_unix_mount_free);
}

static void
info_update_net(Info *info)
{
    update_iface_speed(info->net.iface, "rx_bytes",
                       &info->net.rxspeed,
                       &info->net.rx,
                       &info->net.rx_time);
    update_iface_speed(info->net.iface, "tx_bytes",
                       &info->net.txspeed,
                       &info->net.tx,
                       &info->net.tx_time);
}

static void
info_update_time(Info *info)
{
    if (info->time) {
        g_date_time_unref(info->time);
    }
    // This may be NULL...
    info->time = g_date_time_new_now_local();
}

static void
info_update_uptime(Info *info)
{
    char *buf = read_file("/proc/uptime");
    if (G_UNLIKELY(!buf)) {
        info->uptime = 0;
        return;
    }

    // Uptime has fractional seconds, but we are not interested in that.
    info->uptime = g_ascii_strtoull(buf, NULL, 10);
    g_free(buf);
}

void
info_update(Info *info)
{
    info_update_cpu(info);
    info_update_mem_swap(info);
    info_update_mounts(info);
    info_update_net(info);
    info_update_time(info);
    info_update_uptime(info);
}

int
info_get_cpu_count(Info *info)
{
    return info->cpu.n;
}

double
info_get_cpu_usage(Info *info, int n)
{
    return (0 <= n && n < info->cpu.n) ? info->cpu.usage[n] : 0;
}

guint64
info_get_fs_free(Info *info, const char *path)
{
    struct statvfs st;

    if (statvfs(path, &st) == 0) {
        // NOTE: f_bavail = number of free blocks for unpriviliged users
        //       f_bfree  = number of free blocks
        //return st.f_bfree * st.f_frsize;
        return st.f_bavail * st.f_frsize;
    }

    return 0;
}

double
info_get_mem(Info *info)
{
    return info->mem;
}

GPtrArray *
info_get_mounts(Info *info)
{
    return info->mounts;
}

double
info_get_swap(Info *info)
{
    return info->swap;
}

GDateTime *
info_get_time(Info *info)
{
    return info->time;
}

guint64
info_get_uptime(Info *info)
{
    return info->uptime;
}

double
info_get_net_rxspeed(Info *info)
{
    return info->net.rxspeed;
}

double
info_get_net_txspeed(Info *info)
{
    return info->net.txspeed;
}
