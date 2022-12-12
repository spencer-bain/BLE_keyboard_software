/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief LED Button Service (KBDS) sample
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

//#include <bluetooth/services/kbds.h>
#include "kbds.h"

#include <zephyr/logging/log.h>
#define CONFIG_BT_KBDS_POLL_BUTTON
//LOG_MODULE_REGISTER(bt_kbds, CONFIG_BT_KBDS_LOG_LEVEL);

static uint32_t                   notify_enabled;
static uint32_t                   keystate;
static struct bt_kbds_cb       kbds_cb;

static void kbdslc_ccc_cfg_changed(const struct bt_gatt_attr *attr,
				  uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}


#ifdef CONFIG_BT_KBDS_POLL_BUTTON
static ssize_t read_button(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf,
			  uint16_t len,
			  uint16_t offset)
{
	const char *value = attr->user_data;

	//LOG_DBG("Attribute read, handle: %u, conn: %p", attr->handle,
		//(void *)conn);

	if (kbds_cb.button_cb) {
		keystate = kbds_cb.button_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
					 sizeof(*value));
	}

	return 0;
}
#endif

/* LED Button Service Declaration */
BT_GATT_SERVICE_DEFINE(kbds_svc,
BT_GATT_PRIMARY_SERVICE(BT_UUID_KBDS),
#ifdef CONFIG_BT_KBDS_POLL_BUTTON
	BT_GATT_CHARACTERISTIC(BT_UUID_KBDS_BUTTON,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_button, NULL,
			       &keystate),
#else
	BT_GATT_CHARACTERISTIC(BT_UUID_KBDS_BUTTON,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, NULL, NULL, NULL),
#endif
	BT_GATT_CCC(kbdslc_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int bt_kbds_init(struct bt_kbds_cb *callbacks)
{
	if (callbacks) {
		kbds_cb.button_cb = callbacks->button_cb;
	}

	return 0;
}

int bt_kbds_send_keystate(uint32_t keystate)
{
	if (!notify_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &kbds_svc.attrs[2],
			      &keystate,
			      sizeof(keystate));
}
