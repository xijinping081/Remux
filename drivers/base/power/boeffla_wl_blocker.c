/*
 * Boeffla Wakelock Blocker
 *
 * Author: andip71, 01.09.2017
 * Refactored by: dain09, 24.12.2025
 *
 * Version 2.0.0
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
#include "power.h" // For wakeup_source definition
#include "boeffla_wl_blocker.h"

/*****************************************/
// Variables
/*****************************************/

static char user_wl_list[LENGTH_LIST_WL];
static char default_wl_list[LENGTH_LIST_WL_DEFAULT];
static bool blocker_active;
static bool blocker_debug;

// We need the function prototype from wakeup.c
void wakeup_source_deactivate(struct wakeup_source *ws);

/*****************************************/
// Core Blocker Logic (Refactored)
/*****************************************/

/**
 * is_wakelock_blocked - Checks if a given wakelock name is in a blocked list.
 * @list: The semicolon-separated list of wakelocks to check against.
 * @name: The name of the wakelock to check.
 *
 * This function iterates through a list of wakelocks separated by semicolons
 * and checks for an exact match with the provided name. It uses strsep to
 * tokenize the list safely.
 *
 * Returns: true if the wakelock is found in the list, false otherwise.
 */
static bool is_wakelock_blocked(const char *list, const char *name)
{
	char *list_copy, *list_ptr, *token;
	bool found = false;

	if (!list || !name || list[0] == '\0' || name[0] == '\0')
		return false;

	// We need a mutable copy because strsep modifies the string
	list_copy = kstrdup(list, GFP_ATOMIC);
	if (!list_copy)
		return false;

	list_ptr = list_copy;
	while ((token = strsep(&list_ptr, ";")) != NULL) {
		if (strcmp(token, name) == 0) {
			found = true;
			break;
		}
	}

	kfree(list_copy);
	return found;
}

/**
 * check_for_block - The main entry point called from wakeup.c
 * @ws: The wakeup_source struct to be checked.
 *
 * This is the hook that determines if a wakelock activation should be blocked.
 * It now checks both the user and default lists.
 *
 * Returns: true to block the wakelock, false to allow it.
 */
bool check_for_block(struct wakeup_source *ws)
{
	// Exit early if the blocker is inactive or the source is invalid
	if (!blocker_active || !ws || !ws->name)
		return false;

	if (blocker_debug)
		pr_info("BWB: Checking wakelock '%s'\n", ws->name);

	if (is_wakelock_blocked(user_wl_list, ws->name) ||
	    is_wakelock_blocked(default_wl_list, ws->name)) {
		
		if (blocker_debug)
			pr_info("BWB: Blocking wakelock '%s'\n", ws->name);

		// If it's currently active, deactivate it immediately
		if (ws->active) {
			wakeup_source_deactivate(ws);
			if (blocker_debug)
				pr_info("BWB: Killed active wakelock '%s'\n", ws->name);
		}

		return true; // Block it
	}

	return false; // Do not block
}
EXPORT_SYMBOL(check_for_block);

static void update_blocker_state(void)
{
	if (user_wl_list[0] != '\0' || default_wl_list[0] != '\0')
		blocker_active = true;
	else
		blocker_active = false;
	
	if (blocker_debug)
		pr_info("BWB: Blocker is now %s\n", blocker_active ? "ACTIVE" : "INACTIVE");
}

/*****************************************/
// sysfs interface functions
/*****************************************/

static ssize_t wakelock_blocker_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", user_wl_list);
}

static ssize_t wakelock_blocker_store(struct device * dev, struct device_attribute *attr, const char * buf, size_t count)
{
	if (count >= LENGTH_LIST_WL)
		return -EINVAL;

	strlcpy(user_wl_list, buf, sizeof(user_wl_list));
	// Remove trailing newline if present
	if (count > 0 && user_wl_list[count - 1] == '\n')
		user_wl_list[count - 1] = '\0';
	
	update_blocker_state();
	return count;
}

static ssize_t wakelock_blocker_default_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", default_wl_list);
}

static ssize_t wakelock_blocker_default_store(struct device * dev, struct device_attribute *attr, const char * buf, size_t count)
{
	if (count >= LENGTH_LIST_WL_DEFAULT)
		return -EINVAL;

	strlcpy(default_wl_list, buf, sizeof(default_wl_list));
	// Remove trailing newline if present
	if (count > 0 && default_wl_list[count - 1] == '\n')
		default_wl_list[count - 1] = '\0';

	update_blocker_state();
	return count;
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "Active: %d\nUser list: %s\nDefault list: %s\n",
					blocker_active, user_wl_list, default_wl_list);
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
// Initialize sysfs objects (FIXED)
/*****************************************/

static DEVICE_ATTR(wakelock_blocker, 0664, wakelock_blocker_show, wakelock_blocker_store);
static DEVICE_ATTR(wakelock_blocker_default, 0664, wakelock_blocker_default_show, wakelock_blocker_default_store);
static DEVICE_ATTR(debug, 0664, debug_show, debug_store);
static DEVICE_ATTR(version, 0444, version_show, NULL);

// 1. Define the array of attributes (renamed to _attrs to be standard)
static struct attribute *boeffla_wl_blocker_attrs[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_wakelock_blocker_default.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL
};

// 2. Manually define the attribute group
static const struct attribute_group boeffla_wl_blocker_group = {
	.attrs = boeffla_wl_blocker_attrs,
};

// 3. Define the groups list (needed for miscdevice)
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

	ret = misc_register(&boeffla_wl_blocker_control_device);
	if (ret) {
		pr_err("BWB: failed to register misc device.\n");
		return ret;
	}

	strlcpy(default_wl_list, LIST_WL_DEFAULT, sizeof(default_wl_list));
	update_blocker_state();

	pr_info("BWB: driver version %s loaded.\n", BOEFFLA_WL_BLOCKER_VERSION);
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
MODULE_DESCRIPTION("Boeffla Wakelock Blocker v2.0.0");
MODULE_LICENSE("GPL v2");