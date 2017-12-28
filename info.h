/*
 * manitor -- Display system information on the desktop.
 * See LICENSE for copyright.
 */
#ifndef MANITOR_INFO_H
#define MANITOR_INFO_H

typedef struct Info Info;

// Creates a new Info.
// iface is the network interface to monitor.
Info * info_new(const char *iface);

// Frees the Info structure.
void info_free(Info *info);

// Updates the data gathered by info.
void info_update(Info *info);

// Returns the time at the last update.
GDateTime * info_get_time(Info *info);

// Returns the uptime, in seconds.
guint64 info_get_uptime(Info *info);

// Returns the number of CPUs monitored.
int info_get_cpu_count(Info *info);

// Returns the CPU usage (as a fraction in the range [0, 1]) for CPU n
// (0 = first CPU).
double info_get_cpu_usage(Info *info, int n);

// Returns the number of free bytes for mount point 'path'.
guint64 info_get_fs_free(Info *info, const char *path);

// Returns the memory usage, as a fraction.
double info_get_mem(Info *info);

// Returns an array of mount entries of interest.
// Each element is a pointer to a GUnixMountEntry.
// Do NOT change the returned data!
GPtrArray * info_get_mounts(Info *info);

// Returns the swap usage, as a fraction.
double info_get_swap(Info *info);

// Returns the receive speed (bytes/s) for the monitored network interface.
double info_get_net_rxspeed(Info *info);

// Returns the transmit speed (bytes/s) for the monitored network interface.
double info_get_net_txspeed(Info *info);

#endif // #ifndef MANITOR_INFO_H
