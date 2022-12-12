/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#define dev_mode
//#define dongle

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <assert.h>
#include <zephyr/spinlock.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>

#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/dis.h>
#include <dk_buttons_and_leds.h>
#include "keys.h"

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION   0x0101

#define OUTPUT_REPORT_MAX_LEN            1
#define OUTPUT_REPORT_BIT_MASK_CAPS_LOCK 0x02
#define INPUT_REP_KEYS_REF_ID            1
#define OUTPUT_REP_KEYS_REF_ID           1
#define MODIFIER_KEY_POS                 0
#define SHIFT_KEY_CODE                   0x02
#define SCAN_CODE_POS                    2
#define KEYS_MAX_LEN                    (INPUT_REPORT_KEYS_MAX_LEN - \
					SCAN_CODE_POS)

#define ADV_LED_BLINK_INTERVAL  20

#ifndef dongle
#define ADV_STATUS_LED DK_LED1
#define CON_STATUS_LED DK_LED2
#define LED_CAPS_LOCK  DK_LED3
#define NFC_LED	       DK_LED4
#define KEY_TEXT_MASK  DK_BTN1_MSK
#define KEY_SHIFT_MASK DK_BTN2_MSK
#define KEY_ADV_MASK   DK_BTN4_MSK

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK
#else
#define NUM_OF_LED 3
#define ADV_STATUS_LED 0
#define CON_STATUS_LED 1
#define DEBUG_LED 	   2
const static struct gpio_dt_spec led[NUM_OF_LED] = {GPIO_DT_SPEC_GET(DT_ALIAS(led0 ),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(led1 ),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(led2 ),gpios),	
													};

#endif
#define BUT_ACCEPT_POS 0x08
#define BUT_REJECT_POS 0x16

/* HIDs queue elements. */
#define HIDS_QUEUE_SIZE 10

/* ********************* */
/* Buttons configuration */

/* Note: The configuration below is the same as BOOT mode configuration
 * This simplifies the code as the BOOT mode is the same as REPORT mode.
 * Changing this configuration would require separate implementation of
 * BOOT mode report generation.
 */
#define KEY_CTRL_CODE_MIN 224 /* Control key codes - required 8 of them */
#define KEY_CTRL_CODE_MAX 231 /* Control key codes - required 8 of them */
#define KEY_CODE_MIN      0   /* Normal key codes */
#define KEY_CODE_MAX      101 /* Normal key codes */
#define KEY_PRESS_MAX     6   /* Maximum number of non-control keys
			       * pressed simultaneously
			       */

/* Number of bytes in key report
 *
 * 1B - control keys
 * 1B - reserved
 * rest - non-control keys
 */
#define INPUT_REPORT_KEYS_MAX_LEN (1 + 1 + KEY_PRESS_MAX)

//#define single_change

#ifdef dev_mode
#define KBDS_READ_VALUE_INTERVAL (10 * MSEC_PER_SEC)
#include "kbds_client.h"
#include "kbds.h"

static struct bt_conn *default_conn;
static struct bt_kbds_client kbds;

uint32_t last_keystate_left = 0;
uint32_t last_keystate_right = 0;
uint32_t keystate_right = 0;
uint32_t right_keystate_change = 0;
uint8_t layer_selection = 0;

bool in_pairing_mode = true;

static void notify_keystates_cb(struct bt_kbds_client *kbds,
				    uint32_t keystates);

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));

	printk("Filters matched. Address: %s connectable: %s\n",
		addr, connectable ? "yes" : "no");
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	printk("Connecting failed\n");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	default_conn = bt_conn_ref(conn);
}

static void scan_filter_no_match(struct bt_scan_device_info *device_info,
				 bool connectable)
{
	int err;
	struct bt_conn *conn;
	char addr[BT_ADDR_LE_STR_LEN];

	if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_addr_le_to_str(device_info->recv_info->addr, addr,
				  sizeof(addr));
		printk("Direct advertising received from %s\n", addr);
		bt_scan_stop();

		err = bt_conn_le_create(device_info->recv_info->addr,
					BT_CONN_LE_CREATE_CONN,
					device_info->conn_param, &conn);

		if (!err) {
			default_conn = bt_conn_ref(conn);
			bt_conn_unref(conn);
		}
	}
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match,
		scan_connecting_error, scan_connecting);

static void read_keystates_cb(struct bt_kbds_client *kbds,
				  uint32_t keystates,
				  int err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(bt_kbds_conn(kbds)),
			  addr, sizeof(addr));
	if (err) {
		printk("[%s] Battery read ERROR: %d\n", addr, err);
		return;
	}

	last_keystate_left = keystates;

	printk("[%s] Battery read: %x\n", addr, keystates);
}

static void button_readval(void)
{
	int err;

	printk("Reading KBDS value:\n");
	err = bt_kbds_read_keystates(&kbds, read_keystates_cb);
	if (err) {
		printk("KBDS read call error: %d\n", err);
	}
}

static void discovery_completed_cb(struct bt_gatt_dm *dm,
				   void *context)
{
	int err;

	printk("The discovery procedure succeeded\n");

	bt_gatt_dm_data_print(dm);

	err = bt_kbds_handles_assign(dm, &kbds);
	if (err) {
		printk("Could not init KBDS client object, error: %d\n", err);
	}

	if (bt_kbds_notify_supported(&kbds)) {
		err = bt_kbds_subscribe_keystates(&kbds,
						     notify_keystates_cb);
		if (err) {
			printk("Cannot subscribe to KBDS value notification "
				"(err: %d)\n", err);
			/* Continue anyway */
		}
	} else {
		err = bt_kbds_start_per_read_keystates(
			&kbds, KBDS_READ_VALUE_INTERVAL, notify_keystates_cb);
		if (err) {
			printk("Could not start periodic read of KBDS value\n");
		}
	}

	button_readval();

	err = bt_gatt_dm_data_release(dm);
	if (err) {
		printk("Could not release the discovery data, error "
		       "code: %d\n", err);
	}
}

static void discovery_service_not_found_cb(struct bt_conn *conn,
					   void *context)
{
	printk("The service could not be found during the discovery\n");
}

static void discovery_error_found_cb(struct bt_conn *conn,
				     int err,
				     void *context)
{
	printk("The discovery procedure failed with %d\n", err);
}

static struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};


static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != default_conn) {
		return;
	}

	err = bt_gatt_dm_start(conn, BT_UUID_KBDS, &discovery_cb, NULL);
	if (err) {
		printk("Could not start the discovery procedure, error "
		       "code: %d\n", err);
	}
}

#endif


/* Current report map construction requires exactly 8 buttons */
BUILD_ASSERT((KEY_CTRL_CODE_MAX - KEY_CTRL_CODE_MIN) + 1 == 8);

/* OUT report internal indexes.
 *
 * This is a position in internal report table and is not related to
 * report ID.
 */
enum {
	OUTPUT_REP_KEYS_IDX = 0
};

/* INPUT report internal indexes.
 *
 * This is a position in internal report table and is not related to
 * report ID.
 */
enum {
	INPUT_REP_KEYS_IDX = 0
};

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    OUTPUT_REPORT_MAX_LEN,
	    INPUT_REPORT_KEYS_MAX_LEN);

static volatile bool is_adv;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  ),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static struct conn_mode {
	struct bt_conn *conn;
	bool in_boot_mode;
} conn_mode[CONFIG_BT_HIDS_MAX_CLIENT_COUNT];

static const uint8_t hello_world_str[] = {
	0x0b,	/* Key h */
	0x08,	/* Key e */
	0x0f,	/* Key l */
	0x0f,	/* Key l */
	0x12,	/* Key o */
	0x28,	/* Key Return */
};

static const uint8_t shift_key[] = { 225 };

/* Current report status
 */
static struct keyboard_state {
	uint8_t ctrl_keys_state; /* Current keys state */
	uint8_t keys_state[KEY_PRESS_MAX];
} hid_keyboard_state;

static struct k_work pairing_work;
struct pairing_data_mitm {
	struct bt_conn *conn;
	unsigned int passkey;
};

K_MSGQ_DEFINE(mitm_queue,
	      sizeof(struct pairing_data_mitm),
	      CONFIG_BT_HIDS_MAX_CLIENT_COUNT,
	      4);

static void advertising_start(void)
{
	int err;
	struct bt_le_adv_param *adv_param = BT_LE_ADV_PARAM(
						BT_LE_ADV_OPT_CONNECTABLE |
						BT_LE_ADV_OPT_ONE_TIME,
						BT_GAP_ADV_FAST_INT_MIN_2,
						BT_GAP_ADV_FAST_INT_MAX_2,
						NULL);

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err) {
		if (err == -EALREADY) {
			printk("Advertising continued\n");
		} else {
			printk("Advertising failed to start (err %d)\n", err);
		}

		return;
	}

	is_adv = true;
	printk("Advertising successfully started\n");
}


static void pairing_process(struct k_work *work)
{
	int err;
	struct pairing_data_mitm pairing_data;

	char addr[BT_ADDR_LE_STR_LEN];

	err = k_msgq_peek(&mitm_queue, &pairing_data);
	if (err) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(pairing_data.conn),
			  addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, pairing_data.passkey);
	printk("Press Button 1 to confirm, Button 2 to reject.\n");
}

//I need to edit connected, disconnected, and security_change
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

#ifdef dev_mode
	int bt_err;
	struct bt_conn_info info;
#endif

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s (%u)\n", addr, err);
#ifdef dev_mode
		if (conn == default_conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;

			/* This demo doesn't require active scan */
			bt_err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
			if (bt_err) {
				printk("Scanning failed to start (err %d)\n",
				       err);
			}
		}
#endif
		return;
	}

	printk("Connected %s\n", addr);
	#ifndef dongle
	dk_set_led_on(CON_STATUS_LED);
	#else
	//gpio_pin_set_dt(&led[CON_STATUS_LED],1);
	#endif

#ifdef dev_mode
	bt_conn_get_info(conn, &info);
	if(info.role == BT_CONN_ROLE_CENTRAL){
		//printk("we are connected and about to discover attributes on the connected gatt client!\n");
		printk("This is concidered a Central connection\n");
		bt_err = bt_conn_set_security(conn, BT_SECURITY_L1);
		if (err) {
			printk("Failed to set security: %d\n", bt_err);
			gatt_discover(conn);
		}
		gatt_discover(conn);
	}
	else{//info.role = BT_CONN_ROLE_PERIPHERAL

#endif

	err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about connection\n");
		return;
	}

	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (!conn_mode[i].conn) {
			conn_mode[i].conn = conn;
			conn_mode[i].in_boot_mode = false;
			break;
		}
	}

	is_adv = false;
#ifdef dev_mode
	}
#endif
}


static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;
	bool is_any_dev_connected = false;
	char addr[BT_ADDR_LE_STR_LEN];
#ifdef dev_mode
	struct bt_conn_info info;
#endif

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s (reason %u)\n", addr, reason);
#ifdef dev_mode
	bt_conn_get_info(conn, &info);
	if(info.role == BT_CONN_ROLE_CENTRAL){
		if (default_conn != conn) {
			printk("disconnected from a peripheral that wasn't the other kbd?\n");
			return;
		}
		printk("this is the kbds_peripheral\n");
		kbds.keystates = 0x00;
		bt_conn_unref(default_conn);
		default_conn = NULL;
		/* This demo doesn't require active scan */
		err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
		if (err) {
			printk("Scanning failed to start (err %d)\n", err);
		}
	}
	else{//info.role = BT_CONN_ROLE_PERIPHERAL
#endif

	err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about disconnection\n");
	}

	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			conn_mode[i].conn = NULL;
		} else {
			if (conn_mode[i].conn) {
				is_any_dev_connected = true;
			}
		}
	}

	if (!is_any_dev_connected) {
		in_pairing_mode = true;
#ifndef dongle
		dk_set_led_off(CON_STATUS_LED);
#else
		gpio_pin_set_dt(&led[CON_STATUS_LED],0);
#endif
	}


	advertising_start();
#ifdef dev_mode
	}//else
#endif
}


static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
		in_pairing_mode = false;
	} else {
		printk("Security failed: %s level %u err %d\n", addr, level,
			err);
	}
#ifdef dev_mode
	//gatt_discover(conn);
#endif
}


BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

#ifdef dev_mode
static void scan_init(void)
{
	int err;

	struct bt_scan_init_param scan_init = {
		.connect_if_match = 1,
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT
	};

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_KBDS);
	if (err) {
		printk("Scanning filters cannot be set (err %d)\n", err);

		return;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		printk("Filters cannot be turned on (err %d)\n", err);
	}
}

void create_report(bool down, bool l_or_r, uint32_t position);

void call_key_report(void){
	static uint32_t has_changed = 0;
	if(kbds.keystates != 0xff){
		if(kbds.keystates == 0xff){
			printk("ignored, illigal keystate: %x\n", kbds.keystates);
			return;
		}
	has_changed = kbds.keystates ^ last_keystate_left;	
	}
	//first need to deal with layer select
	layer_selection = 0;
	
	if(is_ith_bit_set(keystate_right,19) || is_ith_bit_set(last_keystate_right,19)){
		layer_selection = 2;
	}

	if(kbds.keystates != 0xff){
		if(is_ith_bit_set(kbds.keystates,22) && is_ith_bit_set(keystate_right,19) || 
		is_ith_bit_set(last_keystate_left,22) && is_ith_bit_set(last_keystate_right,19)){
			layer_selection = 3;
		}
		else if(is_ith_bit_set(kbds.keystates,22) || is_ith_bit_set(last_keystate_left,22)){
			layer_selection = 1;
		}
	}

	/*
	else if(!is_ith_bit_set(kbds.keystates,22) || !is_ith_bit_set(last_keystate_left,22)){
		layer_selection = 0;
	}
	*/

	if(layer_selection != 0){
		printk("layer_selection is %x\n", layer_selection);
	}

	if(kbds.keystates != 0xff){
		//checking left
		for(uint32_t i=0; i<25; i++){
			if(has_changed & (1 << (i-1) )){
				printk("we got a change, it's position is %d!\n",i-1);
				printk("This is the keystate %x\n", kbds.keystates);
				if(kbds.keystates & (1 << (i-1))){
					printk("press\n");
					create_report(true, true, i-1);
				}else{
					printk("release\n\n");
					create_report(false, true, i-1);
				}
			}
		}
		last_keystate_left = kbds.keystates;
	}

	//checking right
	for(uint32_t i=0; i<25; i++){
		if(right_keystate_change & (1 << (i-1) )){
			printk("we got a change, it's position is %d!\n",i-1);
			printk("This is the keystate %x\n", last_keystate_right);
			if(last_keystate_right & (1 << (i-1))){
				printk("press\n");
				create_report(true, false, i-1);
			}else{
				printk("release\n\n");
				create_report(false, false, i-1);
			}
		}
	}
	return;
}

static void notify_keystates_cb(struct bt_kbds_client *kbds,
				    uint32_t keystates)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;
	int j=0;
	uint32_t has_changed = 0;

	bt_addr_le_to_str(bt_conn_get_dst(bt_kbds_conn(kbds)),
			  addr, sizeof(addr));
	if (keystates == BT_KBDS_VAL_INVALID) {
		printk("[%s] Battery notification aborted\n", addr);
	} else {
		//printk("[%s] Battery notification: %"PRIu8"%%\n",
		//       addr, keystates);
		printk("[%s] Battery notification: %x \n",
		       addr, keystates);
		if(conn_mode[0].conn){
			printk("We are connected to a central and have recived a notification.\n");
			//printk("%x \n",key_map_left[0][0][0]);
		}
	}
}


#endif

//This function take a report given by the central of what led's should be turned on
//This function only turns on the capslock led
static void caps_lock_handler(const struct bt_hids_rep *rep)
{
	uint8_t report_val = ((*rep->data) & OUTPUT_REPORT_BIT_MASK_CAPS_LOCK) ?
			  1 : 0;
#ifndef dev_mode
	dk_set_led(LED_CAPS_LOCK, report_val);
#endif
}


static void hids_outp_rep_handler(struct bt_hids_rep *rep,
				  struct bt_conn *conn,
				  bool write)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!write) {
		printk("Output report read\n");
		return;
	};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Output report has been received %s\n", addr);
	caps_lock_handler(rep);
}


static void hids_boot_kb_outp_rep_handler(struct bt_hids_rep *rep,
					  struct bt_conn *conn,
					  bool write)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!write) {
		printk("Output report read\n");
		return;
	};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Boot Keyboard Output report has been received %s\n", addr);
	caps_lock_handler(rep);
}


static void hids_pm_evt_handler(enum bt_hids_pm_evt evt,
				struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	size_t i;

	for (i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			break;
		}
	}

	if (i >= CONFIG_BT_HIDS_MAX_CLIENT_COUNT) {
		printk("Cannot find connection handle when processing PM");
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	switch (evt) {
	case BT_HIDS_PM_EVT_BOOT_MODE_ENTERED:
		printk("Boot mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = true;
		break;

	case BT_HIDS_PM_EVT_REPORT_MODE_ENTERED:
		printk("Report mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = false;
		break;

	default:
		break;
	}
}


static void hid_init(void)
{
	int err;
	struct bt_hids_init_param    hids_init_obj = { 0 };
	struct bt_hids_inp_rep       *hids_inp_rep;
	struct bt_hids_outp_feat_rep *hids_outp_rep;

	static const uint8_t report_map[] = {
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x06,       /* Usage (Keyboard) */
		0xA1, 0x01,       /* Collection (Application) */

		/* Keys */
#if INPUT_REP_KEYS_REF_ID
		0x85, INPUT_REP_KEYS_REF_ID,
#endif
		0x05, 0x07,       /* Usage Page (Key Codes) */
		0x19, 0xe0,       /* Usage Minimum (224) */
		0x29, 0xe7,       /* Usage Maximum (231) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x75, 0x01,       /* Report Size (1) */
		0x95, 0x08,       /* Report Count (8) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */

		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x08,       /* Report Size (8) */
		0x81, 0x01,       /* Input (Constant) reserved byte(1) */

		0x95, 0x06,       /* Report Count (6) */
		0x75, 0x08,       /* Report Size (8) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x65,       /* Logical Maximum (101) */
		0x05, 0x07,       /* Usage Page (Key codes) */
		0x19, 0x00,       /* Usage Minimum (0) */
		0x29, 0x65,       /* Usage Maximum (101) */
		0x81, 0x00,       /* Input (Data, Array) Key array(6 bytes) */

		/* LED */
#if OUTPUT_REP_KEYS_REF_ID
		0x85, OUTPUT_REP_KEYS_REF_ID,
#endif
		0x95, 0x05,       /* Report Count (5) */
		0x75, 0x01,       /* Report Size (1) */
		0x05, 0x08,       /* Usage Page (Page# for LEDs) */
		0x19, 0x01,       /* Usage Minimum (1) */
		0x29, 0x05,       /* Usage Maximum (5) */
		0x91, 0x02,       /* Output (Data, Variable, Absolute), */
				  /* Led report */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x03,       /* Report Size (3) */
		0x91, 0x01,       /* Output (Data, Variable, Absolute), */
				  /* Led report padding */

		0xC0              /* End Collection (Application) */
	};

	hids_init_obj.rep_map.data = report_map;
	hids_init_obj.rep_map.size = sizeof(report_map);

	hids_init_obj.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init_obj.info.b_country_code = 0x00;
	hids_init_obj.info.flags = (BT_HIDS_REMOTE_WAKE |
				    BT_HIDS_NORMALLY_CONNECTABLE);

	hids_inp_rep =
		&hids_init_obj.inp_rep_group_init.reports[INPUT_REP_KEYS_IDX];
	hids_inp_rep->size = INPUT_REPORT_KEYS_MAX_LEN;
	hids_inp_rep->id = INPUT_REP_KEYS_REF_ID;
	hids_init_obj.inp_rep_group_init.cnt++;

	hids_outp_rep =
		&hids_init_obj.outp_rep_group_init.reports[OUTPUT_REP_KEYS_IDX];
	hids_outp_rep->size = OUTPUT_REPORT_MAX_LEN;
	hids_outp_rep->id = OUTPUT_REP_KEYS_REF_ID;
	hids_outp_rep->handler = hids_outp_rep_handler;
	hids_init_obj.outp_rep_group_init.cnt++;

	hids_init_obj.is_kb = true;
	hids_init_obj.boot_kb_outp_rep_handler = hids_boot_kb_outp_rep_handler;
	hids_init_obj.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_obj);
	__ASSERT(err == 0, "HIDS initialization failed\n");
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	int err;

	struct pairing_data_mitm pairing_data;
#ifdef dev_mode

#endif

	pairing_data.conn    = bt_conn_ref(conn);
	pairing_data.passkey = passkey;

	err = k_msgq_put(&mitm_queue, &pairing_data, K_NO_WAIT);
	if (err) {
		printk("Pairing queue is full. Purge previous data.\n");
	}

	/* In the case of multiple pairing requests, trigger
	 * pairing confirmation which needed user interaction only
	 * once to avoid display information about all devices at
	 * the same time. Passkey confirmation for next devices will
	 * be proccess from queue after handling the earlier ones.
	 */
	if (k_msgq_num_used_get(&mitm_queue) == 1) {
		k_work_submit(&pairing_work);
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

//might need to mess with this, like this is more complicated than the central_kbds
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct pairing_data_mitm pairing_data;

	if (k_msgq_peek(&mitm_queue, &pairing_data) != 0) {
		return;
	}

	if (pairing_data.conn == conn) {
		bt_conn_unref(pairing_data.conn);
		k_msgq_get(&mitm_queue, &pairing_data, K_NO_WAIT);
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}

//I think I'll need to create another bt_conn_auth_cb and bt_conn_auth_info_cb
//one for scaning and another for advertising
static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};


/** @brief Function process keyboard state and sends it
 *
 *  @param pstate     The state to be sent
 *  @param boot_mode  Information if boot mode protocol is selected.
 *  @param conn       Connection handler
 *
 *  @return 0 on success or negative error code.
 */
static int key_report_con_send(const struct keyboard_state *state,
			bool boot_mode,
			struct bt_conn *conn)
{
	int err = 0;
	uint8_t  data[INPUT_REPORT_KEYS_MAX_LEN];
	uint8_t *key_data;
	const uint8_t *key_state;
	size_t n;

	data[0] = state->ctrl_keys_state;
	data[1] = 0;
	key_data = &data[2];
	key_state = state->keys_state;
	
	printk("%x %x | ",data[0], data[1]);
	for (n = 0; n < KEY_PRESS_MAX; ++n) {
		printk("%x ",*key_state);
		*key_data++ = *key_state++;
	}
	printk("\n");
	if (boot_mode) {
		err = bt_hids_boot_kb_inp_rep_send(&hids_obj, conn, data,
							sizeof(data), NULL);
		printk("in boot mode apparently?\n");
	} else {
		err = bt_hids_inp_rep_send(&hids_obj, conn,
						INPUT_REP_KEYS_IDX, data,
						sizeof(data), NULL);
		//printk("data sent \n");
	}
	
	return err;
}

/** @brief Function process and send keyboard state to all active connections
 *
 * Function process global keyboard state and send it to all connected
 * clients.
 *
 * @return 0 on success or negative error code.
 */
static int key_report_send(void)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			int err;

			err = key_report_con_send(&hid_keyboard_state,
						  conn_mode[i].in_boot_mode,
						  conn_mode[i].conn);
			if (err) {
				printk("Key report send error: %d\n", err);
				return err;
			}
			printk("sent key_report\n");
		}
	}
	return 0;
}

/** @brief Change key code to ctrl code mask
 *
 *  Function changes the key code to the mask in the control code
 *  field inside the raport.
 *  Returns 0 if key code is not a control key.
 *
 *  @param key Key code
 *
 *  @return Mask of the control key or 0.
 */
static uint8_t button_ctrl_code(uint8_t key)
{
	if (KEY_CTRL_CODE_MIN <= key && key <= KEY_CTRL_CODE_MAX) {
		return (uint8_t)(1U << (key - KEY_CTRL_CODE_MIN));
	}
	return 0;
}


static int hid_kbd_state_key_set(uint8_t key)
{
	//printk("In setting kbd state\n");
	uint8_t ctrl_mask = button_ctrl_code(key);

	if (ctrl_mask) {
		hid_keyboard_state.ctrl_keys_state |= ctrl_mask;
		printk("matches a control key\n");
		return 0;
	}
	for (size_t i = 0; i < KEY_CTRL_CODE_MAX; ++i) {
		if (hid_keyboard_state.keys_state[i] == 0) {
			hid_keyboard_state.keys_state[i] = key;
			return 0;
		}
	}
	/* All slots busy */
	printk("we busy\n");
	return -EBUSY;
}


static int hid_kbd_state_key_clear(uint8_t key)
{
	uint8_t ctrl_mask = button_ctrl_code(key);

	if (ctrl_mask) {
		hid_keyboard_state.ctrl_keys_state &= ~ctrl_mask;
		return 0;
	}
	for (size_t i = 0; i < KEY_CTRL_CODE_MAX; ++i) {
		if (hid_keyboard_state.keys_state[i] == key) {
			hid_keyboard_state.keys_state[i] = 0;
			return 0;
		}
	}
	/* Key not found */
	return -EINVAL;
}

/** @brief Press a button and send report
 *
 *  @note Functions to manipulate hid state are not reentrant
 *  @param keys
 *  @param cnt
 *
 *  @return 0 on success or negative error code.
 */
static int hid_buttons_press(const uint8_t *keys, size_t cnt)
{
	while (cnt--) {
		int err;

		err = hid_kbd_state_key_set(*keys++);
		if (err) {
			printk("Cannot set selected key.\n");
			return err;
		}
	}
#ifndef dev_mode
	return key_report_send();
#else
	return 0;
#endif
}

/** @brief Release the button and send report
 *
 *  @note Functions to manipulate hid state are not reentrant
 *  @param keys
 *  @param cnt
 *
 *  @return 0 on success or negative error code.
 */
static int hid_buttons_release(const uint8_t *keys, size_t cnt)
{
	while (cnt--) {
		int err;

		err = hid_kbd_state_key_clear(*keys++);
		if (err) {
			printk("Cannot clear selected key.\n");
			return err;
		}
	}

#ifndef dev_mode
	return key_report_send();
#endif
}


static void button_text_changed(bool down)
{
#ifdef single_change
	static int i = 0;
	//static const uint8_t *chr = &key_map_left[0][1][1];
	static const uint8_t *chr;
	if(down){
		switch (i)
		{
			case 0:
				chr = &key_map_right[0][1][0];
				break;
			case 1:
				chr = &key_map_left[0][0][3];
				break;
			case 2:
				chr = &key_map_right[0][1][3];
				break;
			case 3:
				chr = &key_map_right[0][1][3];
				break;
			case 4:
				chr = &key_map_right[0][0][3];
				break;
			case 5:
				chr = &key_map_right[0][3][0];
				break;
			default:
				printk("didn't pick any case\n");
				break;
		}
		if(i>=5){i=0;}
		else{i++;}
	}
	if (down) {
		hid_buttons_press(chr, 1);
	} else {
		hid_buttons_release(chr, 1);
	}

#else
	static const uint8_t *chr = hello_world_str;

	if (down) {
		hid_buttons_press(chr, 1);
	} else {
		hid_buttons_release(chr, 1);
		if (++chr == (hello_world_str + sizeof(hello_world_str))) {
			chr = hello_world_str;
		}
	}
#endif
}

#ifdef dev_mode
static void button_text_change(bool down, bool l_or_r, int i, int j, int k)
{
	static const uint8_t *chr;
	if(l_or_r){
		printk("Using left key map\n");
		chr = &key_map_left[i][j][k];
	}
	else{
		printk("Using right key map\n");
		chr = &key_map_right[i][j][k];
	}

	if (down) {
		hid_buttons_press(chr, 1);
	} else {
		hid_buttons_release(chr, 1);
	}
	return;
}
#endif

static void button_shift_changed(bool down)
{
	if (down) {
		hid_buttons_press(shift_key, 1);
	} else {
		hid_buttons_release(shift_key, 1);
	}
}


static void num_comp_reply(bool accept)
{
	struct pairing_data_mitm pairing_data;
	struct bt_conn *conn;

	if (k_msgq_get(&mitm_queue, &pairing_data, K_NO_WAIT) != 0) {
		return;
	}

	conn = pairing_data.conn;

	if (accept) {
		bt_conn_auth_passkey_confirm(conn);
		printk("Numeric Match, conn %p\n", conn);
	} else {
		bt_conn_auth_cancel(conn);
		printk("Numeric Reject, conn %p\n", conn);
	}

	bt_conn_unref(pairing_data.conn);

	if (k_msgq_num_used_get(&mitm_queue)) {
		k_work_submit(&pairing_work);
	}
}

#ifndef dev_mode
static void button_changed(uint32_t button_state, uint32_t has_changed)
{
	static bool pairing_button_pressed;

	uint32_t buttons = button_state & has_changed;

	if (k_msgq_num_used_get(&mitm_queue)) {
		if (buttons & KEY_PAIRING_ACCEPT) {
			pairing_button_pressed = true;
			num_comp_reply(true);

			return;
		}

		if (buttons & KEY_PAIRING_REJECT) {
			pairing_button_pressed = true;
			num_comp_reply(false);

			return;
		}
	}

	/* Do not take any action if the pairing button is released. */
	if (pairing_button_pressed &&
	    (has_changed & (KEY_PAIRING_ACCEPT | KEY_PAIRING_REJECT))) {
		pairing_button_pressed = false;

		return;
	}

	if (has_changed & KEY_TEXT_MASK) {
		button_text_changed((button_state & KEY_TEXT_MASK) != 0);
	}
	if (has_changed & KEY_SHIFT_MASK) {
		button_shift_changed((button_state & KEY_SHIFT_MASK) != 0);
	}
}


static void configure_gpio(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}

	err = dk_leds_init();
	if (err) {
		printk("Cannot init LEDs (err: %d)\n", err);
	}
}
#else
//put code for gpio get state
uint32_t pairing_mode(uint32_t last_keystate_right, uint32_t right_keystate_change){
	static bool pairing_button_pressed;

	uint32_t buttons = last_keystate_right & right_keystate_change;

	if(buttons != 0){
		printk("buttons: %d\n", buttons);
	}
	if (k_msgq_num_used_get(&mitm_queue)) {
		if (buttons & BUT_ACCEPT_POS) {
			pairing_button_pressed = true;
			num_comp_reply(true);
			in_pairing_mode = false;

			return last_keystate_right;
		}

		if (buttons & BUT_REJECT_POS) {
			pairing_button_pressed = true;
			num_comp_reply(false);

			return last_keystate_right;
		}
		
	}

	/* Do not take any action if the pairing button is released. */
	
	if (pairing_button_pressed &&
	    (right_keystate_change & (BUT_ACCEPT_POS | BUT_REJECT_POS))) {
		pairing_button_pressed = false;

		return last_keystate_right;
	}

	return last_keystate_right;
}

#endif

#ifdef dev_mode
void create_report(bool down, bool l_or_r, uint32_t position){
	#define LAYER_KEY_1 22
	#define LAYER_KEY_2 19
	uint8_t *(key_map[4][4][6]);
	int j = 0, k = 0;

	//need to mess with this logic to implement layer switching
	if(l_or_r && (position == 22)){return;}
	if(!l_or_r && (position == 19)){return;}

	j = position / 6;
	k = position % 6;
	//hard coding the fix for the second layer shift problem  
	if(layer_selection == 2 && (j == 0) && ((l_or_r && k > 0) || (!l_or_r && k < 5))){
		set_or_clear_mod_byte(&hid_keyboard_state.ctrl_keys_state, down, &(key_map_left[0][2][0]));
	}

	printk("These are the indexes i: %d, j: %d, k: %d\n", layer_selection,j,k);
	printk("This is what it should be sending %x \n", key_map_left[layer_selection][j][k]);
	uint8_t *chr;
	if(l_or_r){//left_key_map
		//key_map = &key_map_left;
		chr = &(key_map_left[layer_selection][j][k]);
		if(is_mod_chr(l_or_r,position)){
			set_or_clear_mod_byte(&hid_keyboard_state.ctrl_keys_state, down, &(key_map_left[layer_selection][j][k]));
		}else{
			button_text_change(down, l_or_r, layer_selection, j, k);
		}
	}else{//right_key_map
		//key_map = &key_map_right;
		chr = &(key_map_right[layer_selection][j][k]);
		if(is_mod_chr(l_or_r,position)){
			set_or_clear_mod_byte(&hid_keyboard_state.ctrl_keys_state, down, &(key_map_right[layer_selection][j][k]));
		}else{
			button_text_change(down, l_or_r, layer_selection, j, k);
		}
	}

	key_report_send();
	return;
} 

#define NUM_OF_ROW 4
#define NUM_OF_COL 6
#define NUM_OF_BUT 0

#ifndef dongle
const static struct gpio_dt_spec row[NUM_OF_ROW] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin12 ),gpios),//12	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin11 ),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin4),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin3),gpios)//3
													};

const static struct gpio_dt_spec col[NUM_OF_COL] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin22),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin23),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin24),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin25),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin26),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin27),gpios)	
													};
#else
const static struct gpio_dt_spec row[NUM_OF_ROW] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin2 ),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin29 ),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin31),gpios),	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin9),gpios)
													};

const static struct gpio_dt_spec col[NUM_OF_COL] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin13),gpios),//24
													GPIO_DT_SPEC_GET(DT_ALIAS(pin15),gpios),//22
													GPIO_DT_SPEC_GET(DT_ALIAS(pin17),gpios),//20
													GPIO_DT_SPEC_GET(DT_ALIAS(pin20),gpios),//17	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin22),gpios),//15	
													GPIO_DT_SPEC_GET(DT_ALIAS(pin24),gpios)//13	
													};
#endif

int gpio_init(void){
    int err;
    for(int i = 0; i<NUM_OF_ROW; i++){
		err = gpio_pin_configure_dt(&row[i],GPIO_OUTPUT);
        if(err){
            printk("Couldn't configure row[%d]\n", i);
        }
		err = gpio_pin_set_dt(&row[i],0);
        if(err){
            printk("Couldn't set row[%d]\n", i);
        }
    }
    for(int i = 0; i<NUM_OF_COL; i++){
		err = gpio_pin_configure_dt(&col[i],GPIO_INPUT);
        if(err){
            printk("Couldn't configure col[%d]\n", i);
        }
    }
#ifdef dongle
    for(int i = 0; i<NUM_OF_LED; i++){
		err = gpio_pin_configure_dt(&led[i],GPIO_OUTPUT);
        if(err){
            printk("Couldn't configure led[%d]\n", i);
        }
		err = gpio_pin_set_dt(&led[i],0);
        if(err){
            printk("Couldn't set led[%d]\n", i);
        }
    }
#endif
    return err;
}

uint32_t get_keystate(uint32_t last_button_state, uint32_t *has_changed){
	static uint32_t button_state;
	button_state = 0;
    //printk("getting right keystate \n");

	for(int i = 0; i<NUM_OF_ROW; i++){
		gpio_pin_set_dt(&row[i],1); //set pin to GND
		for(int j = 0; j<NUM_OF_COL; j++){
			button_state |= gpio_pin_get_dt(&col[j]) << (NUM_OF_BUT + (NUM_OF_ROW - 1 -i )*
														NUM_OF_COL + (NUM_OF_COL-1 -j));
		}
		gpio_pin_set_dt(&row[i],0); //set pin to VCC
	}

	*has_changed = button_state ^ last_button_state;
	//if you want to analyse the button_state, do it between these 2 operations of has_changed ^^
	return button_state;
}

#endif

void main(void)
{
	int err;
	int blink_status = 0;

	printk("Starting Bluetooth Peripheral HIDS keyboard example\n");

#ifndef dev_mode
	configure_gpio();
#endif
	gpio_init();

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		printk("Failed to register authorization callbacks.\n");
		return;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		printk("Failed to register authorization info callbacks.\n");
		return;
	}
#ifdef dev_mode
	bt_kbds_client_init(&kbds);
#endif

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	hid_init();

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	advertising_start();

#ifdef dev_mode
	scan_init();

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}
	printk("scanning started\n");

#endif

	k_work_init(&pairing_work, pairing_process);

	for (;;) {
		/*
		if (is_adv) {
			dk_set_led(ADV_STATUS_LED, (++blink_status) % 2);
		} else {
			dk_set_led_off(ADV_STATUS_LED);
		}
		*/
		k_sleep(K_MSEC(ADV_LED_BLINK_INTERVAL));
#ifdef dev_mode
		if(conn_mode[0].conn && !in_pairing_mode){
			call_key_report();
			last_keystate_right = get_keystate(last_keystate_right, &right_keystate_change);
			//gpio_pin_set_dt(&led[DEBUG_LED],0);
		}else if(in_pairing_mode){
			//gpio_pin_set_dt(&led[DEBUG_LED],1);
			last_keystate_right = get_keystate(last_keystate_right, &right_keystate_change);
			last_keystate_right = pairing_mode(last_keystate_right, right_keystate_change);
		}
#endif
		/* Battery level simulation */
	}
}
