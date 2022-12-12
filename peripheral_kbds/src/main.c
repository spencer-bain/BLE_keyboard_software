/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

//#include <bluetooth/services/kbds.h>

#include <zephyr/settings/settings.h>


#include "kbds.h"

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)

#define RUN_LED_BLINK_INTERVAL  3

#define NUM_OF_ROW 4
#define NUM_OF_COL 6
#define NUM_OF_BUT 0

const static struct gpio_dt_spec user_led[] = {GPIO_DT_SPEC_GET(DT_ALIAS(led0),gpios)};
const static struct gpio_dt_spec conn_led[] = {GPIO_DT_SPEC_GET(DT_ALIAS(led1),gpios)};
const static struct gpio_dt_spec run_led[]  = {GPIO_DT_SPEC_GET(DT_ALIAS(led2),gpios)};
const static struct gpio_dt_spec test_led[] = {GPIO_DT_SPEC_GET(DT_ALIAS(led3),gpios)};

const static struct gpio_dt_spec test_but[] = {GPIO_DT_SPEC_GET(DT_ALIAS(sw0),gpios)};

const static struct gpio_dt_spec col[] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin24),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin22),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin20),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin17),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin15),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin13),gpios)
										};

const static struct gpio_dt_spec row[] = {GPIO_DT_SPEC_GET(DT_ALIAS(pin2),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin29),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin31),gpios),
										GPIO_DT_SPEC_GET(DT_ALIAS(pin9),gpios),
										};


static uint32_t app_keystate;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_KBDS_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}

	printk("Connected\n");

	gpio_pin_set_dt(conn_led,1);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %u)\n", reason);

	//dk_set_led_off(CON_STATUS_LED);
	gpio_pin_set_dt(conn_led,0);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
};

static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;

static uint32_t app_button_cb(void)
{
	return app_keystate;
}

static struct bt_kbds_cb kbds_callbacs = {
	.button_cb = app_button_cb,
};

/*
static void button_changed(uint32_t keystate, uint32_t has_changed)
{
	uint32_t user_keystate = keystate + 1048576;

	bt_kbds_send_keystate(user_keystate);
}

static int init_button(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}

	return err;
}
*/

uint32_t test_func(uint32_t last_val){
	static uint32_t val;	
	gpio_pin_set_dt(&row[1],1);//set to GRN
	val = gpio_pin_get_dt(&col[2]);
	gpio_pin_set_dt(&row[1],0);//set to VCC

	if(val != last_val){
		bt_kbds_send_keystate(val);
	}

	return val;

	/* This works great
	static uint32_t val;	
	val = gpio_pin_get_dt(test_but);
	if(val != last_val){
		bt_kbds_send_keystate(val);
	}
	return val;
	*/
}

uint32_t get_keystate(uint32_t last_button_state){
	static uint32_t has_changed, button_state;
	button_state = 0;

	for(int i = 0; i<NUM_OF_ROW; i++){
		gpio_pin_set_dt(&row[i],1); //set pin to GND
		for(int j = 0; j<NUM_OF_COL; j++){
			button_state |= gpio_pin_get_dt(&col[j]) << (NUM_OF_BUT + (NUM_OF_ROW - 1 -i )*
														NUM_OF_COL + (NUM_OF_COL-1 -j));
		}
		gpio_pin_set_dt(&row[i],0); //set pin to VCC
	}

	has_changed = button_state ^ last_button_state;
	//if you want to analyse the button_state, do it between these 2 operations of has_changed 
	app_keystate = button_state;
	if(has_changed != 0){
		bt_kbds_send_keystate(button_state);
	}
	return button_state;
}

int gpio_init(void){
	int err;
	err = gpio_pin_configure_dt(user_led,GPIO_OUTPUT);
	if(err != 0){
		return err;
	}
	err = gpio_pin_configure_dt(conn_led,GPIO_OUTPUT);
	if(err != 0){
		return err;
	}
	err = gpio_pin_configure_dt(run_led,GPIO_OUTPUT);
	if(err != 0){
		return err;
	}
	err = gpio_pin_configure_dt(test_led,GPIO_OUTPUT);
	if(err != 0){
		return err;
	}
	gpio_pin_set_dt(user_led, 0);
	gpio_pin_set_dt(conn_led, 0);
	gpio_pin_set_dt(run_led, 0);
	gpio_pin_set_dt(test_led, 0);
	
	err = gpio_pin_configure_dt(test_but,GPIO_INPUT);
	if(err != 0){
		return err;
	}

	for(int i = 0; i < NUM_OF_ROW; i++){
		err = gpio_pin_configure_dt(&row[i], GPIO_OUTPUT);
		if(err){
			printk("problem with row %d (err %d)\n",i,err);
			return err;
		}
		gpio_pin_set_dt(&row[i],0);//VCC aka 5v
	}
	
	for(int i = 0; i < NUM_OF_COL; i++){
		err = gpio_pin_configure_dt(&col[i], GPIO_INPUT);
		if(err){
			printk("problem with row %d (err %d)\n",i,err);
			return err;
		}
	}

	return err;
}

void main(void)
{
	int blink_status = 0;
	int err;

	printk("Starting Bluetooth Peripheral KBDS example\n");

	err = gpio_init();
	if (err) {
		printk("Button init failed (err %d)\n", err);
		return;
	}

	/*
	if (IS_ENABLED(CONFIG_BT_KBDS_SECURITY_ENABLED)) {
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
	}
	*/

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = bt_kbds_init(&kbds_callbacs);
	if (err) {
		printk("Failed to init KBDS (err:%d)\n", err);
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	printk("Advertising successfully started\n");
	uint32_t button_state;
	for (;;) {
		//dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		//gpio_pin_set_dt(run_led,(++blink_status) % 2);
		button_state = get_keystate(button_state);
		//button_state = test_func(button_state);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
	}
}
