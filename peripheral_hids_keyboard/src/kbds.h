/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BT_KBDS_H_
#define BT_KBDS_H_

/**@file
 * @defgroup bt_kbds LED Button Service API
 * @{
 * @brief API for the LED Button Service (KBDS).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>

/** @brief KBDS Service UUID. */
#define BT_UUID_KBDS_VAL \
	BT_UUID_128_ENCODE(0x00001523, 0x1212, 0xedfe, 0x2523, 0x7855eabcd123)

/** @brief Button Characteristic UUID. */
#define BT_UUID_KBDS_BUTTON_VAL \
	BT_UUID_128_ENCODE(0x00001524, 0x1212, 0xedfe, 0x2523, 0x7855eabcd123)


#define BT_UUID_KBDS           BT_UUID_DECLARE_128(BT_UUID_KBDS_VAL)
#define BT_UUID_KBDS_BUTTON    BT_UUID_DECLARE_128(BT_UUID_KBDS_BUTTON_VAL)

/** @brief Callback type for when the button state is pulled. */
typedef uint32_t (*button_cb_t)(void);

/** @brief Callback struct used by the KBDS Service. */
struct bt_kbds_cb {
	/** Button read callback. */
	button_cb_t button_cb;
};

/** @brief Initialize the KBDS Service.
 *
 * This function registers a GATT service with two characteristics: Button
 * and LED.
 * Send notifications for the Button Characteristic to let connected peers know
 * when the button state changes.
 * Write to the LED Characteristic to change the state of the LED on the
 * board.
 *
 * @param[in] callbacks Struct containing pointers to callback functions
 *			used by the service. This pointer can be NULL
 *			if no callback functions are defined.
 *
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int bt_kbds_init(struct bt_kbds_cb *callbacks);

/** @brief Send the button state.
 *
 * This function sends a binary state, typically the state of a
 * button, to all connected peers.
 *
 * @param[in] keystate The state of the button.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int bt_kbds_send_keystate(uint32_t keystate);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* BT_KBDS_H_ */
