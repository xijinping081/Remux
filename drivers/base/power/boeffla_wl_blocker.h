/*
 * Boeffla Wakelock Blocker v2.0.0
 */

#ifndef _BOEFFLA_WL_BLOCKER_H
#define _BOEFFLA_WL_BLOCKER_H

#include <linux/types.h>

#define BOEFFLA_WL_BLOCKER_VERSION "2.1.0-Optimized"

// Default wakelocks to block. Semicolon separated.
#define LIST_WL_DEFAULT "qcom_rx_wakelock;wlan;NETLINK;netmgr_wl;wcnss_filter_lock;smd_channel_loop;ipa_power"

// Max length for the user-configurable list
#define LENGTH_LIST_WL 255
// Max length for the default list
#define LENGTH_LIST_WL_DEFAULT 125

// Forward declaration of the main blocker function
struct wakeup_source;
bool check_for_block(struct wakeup_source *ws);

#endif // _BOEFFLA_WL_BLOCKER_H