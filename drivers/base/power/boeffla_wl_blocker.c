/*
 * Boeffla Wakelock Blocker
 *
 * Author: andip71, 01.09.2017
 * Refactored by: dain09, 29.12.2025
 *
 * Version 2.1.0
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "power.h" // For wakeup_source definition
#include "boeffla_wl_blocker.h"

/*****************************************/
// Configuration & Globals
/*****************************************/

#define MAX_WL_ITEMS 64  // Maximum number of blocked items allowed

// Raw strings for Sysfs display (Clean semicolon separated)
static char user_wl_list_raw[LENGTH_LIST_WL];
static char default_wl_list_raw[LENGTH_LIST_WL_DEFAULT];

// Working buffers (Modified by strsep, contains \0)
static char user_wl_list_work[LENGTH_LIST_WL];
static char default_wl_list_work[LENGTH_LIST_WL_DEFAULT];

// Array of pointers for fast lookup
static char *user_wl_ptrs[MAX_WL_ITEMS];
static char *default_wl_ptrs[MAX_WL_ITEMS];

// Counters
static int user_wl_count = 0;
static int default_wl_count = 0;

static bool blocker_active = false;
static bool blocker_debug = false;

// Spinlock to protect list updates vs reads
static DEFINE_SPINLOCK(blocker_lock);

// We need the function prototype from wakeup.c
void wakeup_source_deactivate(struct wakeup_source *ws);

/*****************************************/
// Helper Functions (Optimization Logic)
/*****************************************/

/**
 * rebuild_wl_array - Parses a semicolon list into a pointer array
 * @raw_source: The clean input string (from sysfs or default)
 * @work_buffer: The buffer to be chopped up (must be same size as raw)
 * @ptrs_array: The array to hold string pointers
 * @count_out: Pointer to integer to store the item count
 *
 * NOTE: Must be called with lock held!
 */
static void rebuild_wl_array(const char *raw_source, char *work_buffer, 
                             char **ptrs_array, int *count_out)
{
    char *p, *token;
    int c = 0;

    // Reset count
    *count_out = 0;

    if (!raw_source || raw_source[0] == '\0')
        return;

    // Copy raw to work buffer because strsep modifies it
    strlcpy(work_buffer, raw_source, LENGTH_LIST_WL); // Assuming sizes are similar enough for safety check

    p = work_buffer;
    while ((token = strsep(&p, ";")) != NULL) {
        if (*token == '\0') 
            continue; // Skip empty tokens
        
        // Strip newlines if any
        if (token[strlen(token)-1] == '\n')
            token[strlen(token)-1] = '\0';

        if (c < MAX_WL_ITEMS) {
            ptrs_array[c++] = token;
        } else {
            pr_warn("BWB: List full, ignoring extra items\n");
            break;
        }
    }
    *count_out = c;
}

static void update_blocker_state(void)
{
    unsigned long flags;
    spin_lock_irqsave(&blocker_lock, flags);
    
    // Check if we have any items in either list
    if (user_wl_count > 0 || default_wl_count > 0)
        blocker_active = true;
    else
        blocker_active = false;
        
    spin_unlock_irqrestore(&blocker_lock, flags);

    if (blocker_debug)
        pr_info("BWB: Blocker is now %s (User: %d, Default: %d)\n", 
                blocker_active ? "ACTIVE" : "INACTIVE", user_wl_count, default_wl_count);
}

/*****************************************/
// Core Blocker Logic (Fast Path)
/*****************************************/

/**
 * check_for_block - The main entry point called from wakeup.c
 * 
 * Optimized to use array lookup instead of string parsing.
 * Uses spinlock to ensure thread safety during updates.
 */
bool check_for_block(struct wakeup_source *ws)
{
    unsigned long flags;
    int i;
    bool blocked = false;

    // Fast exit if disabled or invalid
    if (!blocker_active || !ws || !ws->name)
        return false;

    // We use spin_trylock to avoid stalling the wakeup path if sysfs is updating.
    // If we can't get the lock immediately, we let this one pass.
    if (!spin_trylock_irqsave(&blocker_lock, flags))
        return false;

    // Check User List
    for (i = 0; i < user_wl_count; i++) {
        if (user_wl_ptrs[i] && strcmp(user_wl_ptrs[i], ws->name) == 0) {
            blocked = true;
            goto out;
        }
    }

    // Check Default List
    for (i = 0; i < default_wl_count; i++) {
        if (default_wl_ptrs[i] && strcmp(default_wl_ptrs[i], ws->name) == 0) {
            blocked = true;
            goto out;
        }
    }

out:
    spin_unlock_irqrestore(&blocker_lock, flags);

    if (blocked) {
        if (blocker_debug)
            pr_info("BWB: Blocking wakelock '%s'\n", ws->name);

        if (ws->active) {
            wakeup_source_deactivate(ws);
            if (blocker_debug)
                pr_info("BWB: Killed active wakelock '%s'\n", ws->name);
        }
        return true;
    }

    if (blocker_debug)
        pr_info("BWB: Allowed wakelock '%s'\n", ws->name);

    return false;
}
EXPORT_SYMBOL(check_for_block);

/*****************************************/
// sysfs interface functions
/*****************************************/

static ssize_t wakelock_blocker_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    // Show the raw string
    return scnprintf(buf, PAGE_SIZE, "%s\n", user_wl_list_raw);
}

static ssize_t wakelock_blocker_store(struct device * dev, struct device_attribute *attr, const char * buf, size_t count)
{
    unsigned long flags;

    if (count >= LENGTH_LIST_WL)
        return -EINVAL;

    spin_lock_irqsave(&blocker_lock, flags);
    
    // 1. Update Raw String
    strlcpy(user_wl_list_raw, buf, sizeof(user_wl_list_raw));
    // Remove trailing newline
    if (count > 0 && user_wl_list_raw[count - 1] == '\n')
        user_wl_list_raw[count - 1] = '\0';
    
    // 2. Rebuild Optimized Array
    rebuild_wl_array(user_wl_list_raw, user_wl_list_work, user_wl_ptrs, &user_wl_count);
    
    spin_unlock_irqrestore(&blocker_lock, flags);

    update_blocker_state();
    return count;
}

static ssize_t wakelock_blocker_default_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", default_wl_list_raw);
}

static ssize_t wakelock_blocker_default_store(struct device * dev, struct device_attribute *attr, const char * buf, size_t count)
{
    unsigned long flags;

    if (count >= LENGTH_LIST_WL_DEFAULT)
        return -EINVAL;

    spin_lock_irqsave(&blocker_lock, flags);

    strlcpy(default_wl_list_raw, buf, sizeof(default_wl_list_raw));
    if (count > 0 && default_wl_list_raw[count - 1] == '\n')
        default_wl_list_raw[count - 1] = '\0';

    rebuild_wl_array(default_wl_list_raw, default_wl_list_work, default_wl_ptrs, &default_wl_count);

    spin_unlock_irqrestore(&blocker_lock, flags);

    update_blocker_state();
    return count;
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "Active: %d\nDebug: %d\nUser Count: %d\nDefault Count: %d\n",
                    blocker_active, blocker_debug, user_wl_count, default_wl_count);
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int val;
    if (kstrtoint(buf, 10, &val))
        return -EINVAL;
    blocker_debug = !!val;
    return count;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", BOEFFLA_WL_BLOCKER_VERSION);
}

/*****************************************/
// Initialize sysfs objects
/*****************************************/

static DEVICE_ATTR(wakelock_blocker, 0664, wakelock_blocker_show, wakelock_blocker_store);
static DEVICE_ATTR(wakelock_blocker_default, 0664, wakelock_blocker_default_show, wakelock_blocker_default_store);
static DEVICE_ATTR(debug, 0664, debug_show, debug_store);
static DEVICE_ATTR(version, 0444, version_show, NULL);

static struct attribute *boeffla_wl_blocker_attrs[] = {
    &dev_attr_wakelock_blocker.attr,
    &dev_attr_wakelock_blocker_default.attr,
    &dev_attr_debug.attr,
    &dev_attr_version.attr,
    NULL
};

static const struct attribute_group boeffla_wl_blocker_group = {
    .attrs = boeffla_wl_blocker_attrs,
};

static const struct attribute_group *boeffla_wl_blocker_groups[] = {
    &boeffla_wl_blocker_group,
    NULL
};

static struct miscdevice boeffla_wl_blocker_control_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "boeffla_wakelock_blocker",
    .groups = boeffla_wl_blocker_groups,
};

/*****************************************/
// Driver init and exit functions
/*****************************************/

static int __init boeffla_wl_blocker_init(void)
{
    int ret;
    unsigned long flags;

    ret = misc_register(&boeffla_wl_blocker_control_device);
    if (ret) {
        pr_err("BWB: failed to register misc device.\n");
        return ret;
    }

    // Initialize Default List
    spin_lock_irqsave(&blocker_lock, flags);
    
    // Load default string from header
    strlcpy(default_wl_list_raw, LIST_WL_DEFAULT, sizeof(default_wl_list_raw));
    
    // Parse it immediately into the optimized array
    rebuild_wl_array(default_wl_list_raw, default_wl_list_work, default_wl_ptrs, &default_wl_count);
    
    spin_unlock_irqrestore(&blocker_lock, flags);

    update_blocker_state();

    pr_info("BWB: driver version %s loaded (Optimized).\n", BOEFFLA_WL_BLOCKER_VERSION);
    return 0;
}

static void __exit boeffla_wl_blocker_exit(void)
{
    misc_deregister(&boeffla_wl_blocker_control_device);
    pr_info("BWB: driver unloaded.\n");
}

module_init(boeffla_wl_blocker_init);
module_exit(boeffla_wl_blocker_exit);

MODULE_AUTHOR("andip71, Refactored by dain09");
MODULE_DESCRIPTION("Boeffla Wakelock Blocker v2.1.0");
MODULE_LICENSE("GPL v2");