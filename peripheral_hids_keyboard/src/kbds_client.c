/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

//#include <bluetooth/services/kbds_client.h>
#include "kbds.h"
#include "kbds_client.h"

#include <zephyr/logging/log.h>
//LOG_MODULE_REGISTER(kbds_client, CONFIG_BT_KBDS_CLIENT_LOG_LEVEL);

/**
 * @brief Process battery level value notification
 *
 * Internal function to process report notification and pass it further.
 *
 * @param conn   Connection handler.
 * @param params Notification parameters structure - the pointer
 *               to the structure provided to subscribe function.
 * @param data   Pointer to the data buffer.
 * @param length The size of the received data.
 *
 * @retval BT_GATT_ITER_STOP     Stop notification
 * @retval BT_GATT_ITER_CONTINUE Continue notification
 */
static uint8_t notify_process(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	struct bt_kbds_client *kbds;
	uint32_t keystates;
	const uint32_t *bdata = data;

	kbds = CONTAINER_OF(params, struct bt_kbds_client, notify_params);
    /*
	if (!data || !length) {
		printk("Notifications disabled.\n");
		if (kbds->notify_cb) {
			kbds->notify_cb(kbds, BT_KBDS_VAL_INVALID);
		}
		return BT_GATT_ITER_STOP;
	}
	if (length != 1) {
		printk("Unexpected notification value size.\n");
		if (kbds->notify_cb) {
			kbds->notify_cb(kbds, BT_KBDS_VAL_INVALID);
		}
		return BT_GATT_ITER_STOP;
	}
    printk("length %d\n",length);//4
    printk("bdata[0]: %x\n",bdata[0]);//LSB
    printk("bdata[1]: %x\n",bdata[1]);
    printk("bdata[2]: %x\n",bdata[2]);
    printk("bdata[3]: %x\n",bdata[3]);//MSB
    printk("\n");
    */

	keystates = bdata[0];
    /*

	if (keystates > BT_KBDS_VAL_MAX) {
		printk("Unexpected notification value.\n");
		if (kbds->notify_cb) {
			kbds->notify_cb(kbds, BT_KBDS_VAL_INVALID);
		}
		return BT_GATT_ITER_STOP;
	}
    */
	kbds->keystates = keystates;
	if (kbds->notify_cb) {
		kbds->notify_cb(kbds, keystates);
	}

	return BT_GATT_ITER_CONTINUE;
}

/**
 * @brief Process battery level value read
 *
 * Internal function to process report read and pass it further.
 *
 * @param conn   Connection handler.
 * @param err    Read ATT error code.
 * @param params Notification parameters structure - the pointer
 *               to the structure provided to read function.
 * @param data   Pointer to the data buffer.
 * @param length The size of the received data.
 *
 * @retval BT_GATT_ITER_STOP     Stop notification
 * @retval BT_GATT_ITER_CONTINUE Continue notification
 */
static uint8_t read_process(struct bt_conn *conn, uint8_t err,
			     struct bt_gatt_read_params *params,
			     const void *data, uint16_t length)
{
	struct bt_kbds_client *kbds;
	uint32_t keystates = BT_KBDS_VAL_INVALID;
	const uint8_t *bdata = data;

	kbds = CONTAINER_OF(params, struct bt_kbds_client, read_params);

	if (!kbds->read_cb) {
		printk("No read callback present");
	} else  if (err) {
		printk("Read value error: %d", err);
		kbds->read_cb(kbds,  keystates, err);
	} else if (!data || length != 1) {
		kbds->read_cb(kbds,  keystates, -EMSGSIZE);
	} else {
		keystates = bdata[0];
		if (keystates > BT_KBDS_VAL_MAX) {
			printk("Unexpected read value.\n");
			kbds->read_cb(kbds, BT_KBDS_VAL_INVALID, err);
		} else {
			kbds->keystates = keystates;
			kbds->read_cb(kbds, keystates, err);
		}
	}

	kbds->read_cb = NULL;

	return BT_GATT_ITER_STOP;
}

/**
 * @brief Process periodic battery level value read
 *
 * Internal function to process report read and pass it further.
 * And the end the new read request is triggered.
 *
 * @param conn   Connection handler.
 * @param err    Read ATT error code.
 * @param params Notification parameters structure - the pointer
 *               to the structure provided to read function.
 * @param data   Pointer to the data buffer.
 * @param length The size of the received data.
 *
 * @retval BT_GATT_ITER_STOP     Stop notification
 * @retval BT_GATT_ITER_CONTINUE Continue notification
 */
static uint8_t periodic_read_process(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_read_params *params,
				  const void *data, uint16_t length)
{
	int32_t interval;
	struct bt_kbds_client *kbds;
	uint32_t keystates = BT_KBDS_VAL_INVALID;
	const uint8_t *bdata = data;

	kbds = CONTAINER_OF(params, struct bt_kbds_client,
			periodic_read.params);

	if (!kbds->notify_cb) {
		printk("No notification callback present");
	} else  if (err) {
		printk("Read value error: %d", err);
	} else if (!data || length != 1) {
		printk("Unexpected read value size.\n");
	} else {
		keystates = bdata[0];
		if (keystates > BT_KBDS_VAL_MAX) {
			printk("Unexpected read value.\n");
		} else if (kbds->keystates != keystates) {
			kbds->keystates = keystates;
			kbds->notify_cb(kbds, keystates);
		} else {
			/* Do nothing. */
		}
	}

	interval = atomic_get(&kbds->periodic_read.interval);
	if (interval) {
		k_work_schedule(&kbds->periodic_read.read_work,
				K_MSEC(interval));
	}
	return BT_GATT_ITER_STOP;
}


/**
 * @brief Periodic read workqueue handler.
 *
 * @param work Work instance.
 */
static void kbds_read_value_handler(struct k_work *work)
{
	int err;
	struct bt_kbds_client *kbds;

	kbds = CONTAINER_OF(work, struct bt_kbds_client,
			     periodic_read.read_work);

	if (!atomic_get(&kbds->periodic_read.interval)) {
		/* disabled */
		return;
	}

	if (!kbds->conn) {
		printk("No connection object.\n");
		return;
	}

	kbds->periodic_read.params.func = periodic_read_process;
	kbds->periodic_read.params.handle_count  = 1;
	kbds->periodic_read.params.single.handle = kbds->val_handle;
	kbds->periodic_read.params.single.offset = 0;

	err = bt_gatt_read(kbds->conn, &kbds->periodic_read.params);

	/* Do not treats reading after disconnection as error.
	 * Periodic read process is stopped after disconnection.
	 */
	if (err) {
		printk("Periodic Battery Level characteristic read error: %d",
			err);
	}
}


/**
 * @brief Reinitialize the KBDS Client.
 *
 * @param kbds KBDS Client object.
 */
static void kbds_reinit(struct bt_kbds_client *kbds)
{
	kbds->ccc_handle = 0;
	kbds->val_handle = 0;
	kbds->keystates = BT_KBDS_VAL_INVALID;
	kbds->conn = NULL;
	kbds->notify_cb = NULL;
	kbds->read_cb = NULL;
	kbds->notify = false;
}


void bt_kbds_client_init(struct bt_kbds_client *kbds)
{
	memset(kbds, 0, sizeof(*kbds));
	kbds->keystates = BT_KBDS_VAL_INVALID;

	k_work_init_delayable(&kbds->periodic_read.read_work,
			      kbds_read_value_handler);
}


int bt_kbds_handles_assign(struct bt_gatt_dm *dm,
				 struct bt_kbds_client *kbds)
{
	const struct bt_gatt_dm_attr *gatt_service_attr =
			bt_gatt_dm_service_get(dm);
	const struct bt_gatt_service_val *gatt_service =
			bt_gatt_dm_attr_service_val(gatt_service_attr);
	const struct bt_gatt_dm_attr *gatt_chrc;
	const struct bt_gatt_dm_attr *gatt_desc;
	const struct bt_gatt_chrc *chrc_val;

	if (bt_uuid_cmp(gatt_service->uuid, BT_UUID_KBDS)) {
		return -ENOTSUP;
	}
	printk("Getting handles from battery service.\n");

	/* If connection is established again, cancel previous read request. */
	k_work_cancel_delayable(&kbds->periodic_read.read_work);
	/* When workqueue is used its instance cannont be cleared. */
	kbds_reinit(kbds);

	/* Battery level characteristic */
	gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_KBDS_BUTTON);
	if (!gatt_chrc) {
		printk("No battery level characteristic found.\n");
		return -EINVAL;
	}
	chrc_val = bt_gatt_dm_attr_chrc_val(gatt_chrc);
	__ASSERT_NO_MSG(chrc_val); /* This is internal function and it has to
				    * be called with characteristic attribute
				    */
	kbds->properties = chrc_val->properties;
	gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc,
					    BT_UUID_KBDS_BUTTON);
	if (!gatt_desc) {
		printk("No battery level characteristic value found.\n");
		return -EINVAL;
	}
	kbds->val_handle = gatt_desc->handle;

	gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_GATT_CCC);
	if (!gatt_desc) {
		printk("No battery CCC descriptor found. Battery service do not supported notification.\n");
	} else {
		kbds->notify = true;
		kbds->ccc_handle = gatt_desc->handle;
	}

	/* Finally - save connection object */
	kbds->conn = bt_gatt_dm_conn_get(dm);
	return 0;
}

int bt_kbds_subscribe_keystates(struct bt_kbds_client *kbds,
				   bt_kbds_notify_cb func)
{
	int err;

	if (!kbds || !func) {
		return -EINVAL;
	}
	if (!kbds->conn) {
		return -EINVAL;
	}
	if (!(kbds->properties & BT_GATT_CHRC_NOTIFY)) {
		return -ENOTSUP;
	}
	if (kbds->notify_cb) {
		return -EALREADY;
	}

	kbds->notify_cb = func;

	kbds->notify_params.notify = notify_process;
	kbds->notify_params.value = BT_GATT_CCC_NOTIFY;
	kbds->notify_params.value_handle = kbds->val_handle;
	kbds->notify_params.ccc_handle = kbds->ccc_handle;
	atomic_set_bit(kbds->notify_params.flags,
		       BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

	printk("Subscribe: val: %u, ccc: %u \n",
		kbds->notify_params.value_handle,
		kbds->notify_params.ccc_handle);
	err = bt_gatt_subscribe(kbds->conn, &kbds->notify_params);
	if (err) {
		printk("Report notification subscribe error: %d.\n", err);
		kbds->notify_cb = NULL;
		return err;
	}
	printk("Report subscribed.\n");
	return err;
}


int bt_kbds_unsubscribe_keystates(struct bt_kbds_client *kbds)
{
	int err;

	if (!kbds) {
		return -EINVAL;
	}

	if (!kbds->notify_cb) {
		return -EFAULT;
	}

	err = bt_gatt_unsubscribe(kbds->conn, &kbds->notify_params);
	kbds->notify_cb = NULL;
	return err;
}


struct bt_conn *bt_kbds_conn(const struct bt_kbds_client *kbds)
{
	return kbds->conn;
}


int bt_kbds_read_keystates(struct bt_kbds_client *kbds, bt_kbds_read_cb func)
{
	int err;

	if (!kbds || !func) {
		return -EINVAL;
	}
	if (!kbds->conn) {
		return -EINVAL;
	}
	if (kbds->read_cb) {
		return -EBUSY;
	}
	kbds->read_cb = func;
	kbds->read_params.func = read_process;
	kbds->read_params.handle_count  = 1;
	kbds->read_params.single.handle = kbds->val_handle;
	kbds->read_params.single.offset = 0;

	err = bt_gatt_read(kbds->conn, &kbds->read_params);
	if (err) {
		kbds->read_cb = NULL;
		return err;
	}
	return 0;
}


int bt_kbds_get_last_keystates(struct bt_kbds_client *kbds)
{
	if (!kbds) {
		return -EINVAL;
	}

	return kbds->keystates;
}


int bt_kbds_start_per_read_keystates(struct bt_kbds_client *kbds,
					int32_t interval,
					bt_kbds_notify_cb func)
{
	if (!kbds || !func || !interval) {
		return -EINVAL;
	}

	if (bt_kbds_notify_supported(kbds)) {
		return -ENOTSUP;
	}

	kbds->notify_cb = func;
	atomic_set(&kbds->periodic_read.interval, interval);
	k_work_schedule(&kbds->periodic_read.read_work, K_MSEC(interval));

	return 0;
}


void bt_kbds_stop_per_read_keystates(struct bt_kbds_client *kbds)
{
	/* If read is proccesed now, prevent triggering new
	 * characteristic read.
	 */
	atomic_set(&kbds->periodic_read.interval, 0);

	/* If delayed workqueue pending, cancel it. If this fails, we'll exit
	 * early in the read handler due to the interval.
	 */
	k_work_cancel_delayable(&kbds->periodic_read.read_work);
}
