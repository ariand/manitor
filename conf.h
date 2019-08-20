/*
 * manitor -- Display system information on the desktop.
 * See LICENSE for copyright.
 */
#ifndef MANITOR_CONF_H
#define MANITOR_CONF_H

// The monitor number the window should appear on.
#define CONF_MONITOR 1

// The margin to leave around the window (in pixels).
#define CONF_MARGIN 10

// The name of the network interface to monitor.
#define CONF_IFACE "enp8s0"

// The default font to use (a Pango font specification).
#define CONF_FONT "Roboto Condensed, 16px"

// How to format the time? See g_date_time_format(), which uses a format
// similar to strftime(3). The string must be a Pango marked-up text.
#define CONF_CLOCK_FORMAT "<span font='Roboto ultralight 120px'>%-H:%M</span>"

// The default foreground color.
#define CONF_COLOR "rgb(75%, 75%, 75%)"

// Alarm color -- see Alarms below for an explanation.
#define CONF_ALARM_COLOR "#dc322f"

// Background color -- only used by the clock right now.
#define CONF_SHADE_COLOR "rgba(0, 0, 0, 0.25)"

// Alarms -- draw a ring in the alarm color if the value a ring displays
// is greater than or equal to the alarm limit. Use 0 to disable an alarm.
// Each value is in the range [0, 1].
#define CONF_CPU_ALARM 0.75
#define CONF_MEM_ALARM 0.67
#define CONF_SWAP_ALARM 0.05

// The update interval, in integer seconds.
#define CONF_INTERVAL 1

#endif // #ifndef MANITOR_CONF_H
