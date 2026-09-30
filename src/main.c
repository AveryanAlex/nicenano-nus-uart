/*
 * Derived from Nordic Semiconductor's nRF Connect SDK peripheral_uart sample.
 *
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * Copyright (c) 2026 nice!nano NUS UART bridge contributors
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <bluetooth/services/nus.h>

LOG_MODULE_REGISTER(nicenano_nus_uart, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define UART_BUF_SIZE CONFIG_NUS_UART_BUFFER_SIZE
#define UART_RX_TIMEOUT CONFIG_NUS_UART_IDLE_TIMEOUT_US
#define UART_RETRY_DELAY K_MSEC(10)
#define BLE_TX_TIMEOUT K_MSEC(1000)

#if !DT_HAS_CHOSEN(nordic_nus_uart)
#error "The board must define chosen nordic,nus-uart"
#endif

static const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(nordic_nus_uart));
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#if defined(CONFIG_NUS_UART_PAIRING)
static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#define BOND_RESET_PIN 8
#endif

struct uart_data {
	void *fifo_reserved;
	uint8_t data[UART_BUF_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(uart_to_ble_fifo);
static K_FIFO_DEFINE(ble_to_uart_fifo);
static K_SEM_DEFINE(ble_ready, 0, 1);
static K_SEM_DEFINE(nus_tx_done, 0, 1);
static K_MUTEX_DEFINE(conn_lock);

static struct bt_conn *current_conn;
static atomic_t connected;
static atomic_t notifications_enabled;
static struct k_work advertising_work;
static struct k_work_delayable uart_restart_work;
#if defined(CONFIG_NUS_UART_PAIRING)
static bool erase_bonds;
#endif

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data scan_response[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void led_set(bool on)
{
	(void)gpio_pin_set_dt(&status_led, on ? 1 : 0);
}

#if defined(CONFIG_NUS_UART_PAIRING)
static bool bond_reset_requested(void)
{
	if (!device_is_ready(gpio0) ||
	    gpio_pin_configure(gpio0, BOND_RESET_PIN, GPIO_INPUT | GPIO_PULL_UP) != 0) {
		return false;
	}

	k_sleep(K_MSEC(20));
	if (gpio_pin_get(gpio0, BOND_RESET_PIN) != 0) {
		return false;
	}

	/* A deliberate hold prevents ordinary UART startup traffic from clearing bonds. */
	for (int i = 0; i < 20; ++i) {
		led_set((i & 1) != 0);
		k_sleep(K_MSEC(100));
		if (gpio_pin_get(gpio0, BOND_RESET_PIN) != 0) {
			led_set(false);
			return false;
		}
	}

	led_set(false);
	return true;
}
#endif

static struct uart_data *uart_buffer_alloc(void)
{
	struct uart_data *buf = k_malloc(sizeof(*buf));

	if (buf != NULL) {
		buf->len = 0;
	}

	return buf;
}

static void uart_restart(struct k_work *work)
{
	ARG_UNUSED(work);

	struct uart_data *buf = uart_buffer_alloc();
	if (buf == NULL) {
		LOG_WRN("No memory for UART RX buffer");
		k_work_reschedule(&uart_restart_work, UART_RETRY_DELAY);
		return;
	}

	int err = uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_RX_TIMEOUT);
	if (err != 0) {
		k_free(buf);
		LOG_WRN("Cannot restart UART RX (%d)", err);
		k_work_reschedule(&uart_restart_work, UART_RETRY_DELAY);
	}
}

static void uart_callback(const struct device *dev, struct uart_event *event, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	static uint8_t *aborted_buffer;
	static size_t aborted_len;
	static bool rx_disable_pending;
	struct uart_data *buf;

	switch (event->type) {
	case UART_TX_DONE:
		if (event->data.tx.buf == NULL || event->data.tx.len == 0) {
			break;
		}

		if (aborted_buffer != NULL) {
			buf = CONTAINER_OF(aborted_buffer, struct uart_data, data[0]);
			aborted_buffer = NULL;
			aborted_len = 0;
		} else {
			buf = CONTAINER_OF(event->data.tx.buf, struct uart_data, data[0]);
		}
		k_free(buf);

		buf = k_fifo_get(&ble_to_uart_fifo, K_NO_WAIT);
		if (buf != NULL && uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS) != 0) {
			k_fifo_put(&ble_to_uart_fifo, buf);
		}
		break;

	case UART_TX_ABORTED:
		if (aborted_buffer == NULL) {
			aborted_buffer = (uint8_t *)event->data.tx.buf;
		}
		aborted_len += event->data.tx.len;
		buf = CONTAINER_OF(aborted_buffer, struct uart_data, data[0]);
		(void)uart_tx(uart, &buf->data[aborted_len], buf->len - aborted_len,
			      SYS_FOREVER_MS);
		break;

	case UART_RX_RDY:
		buf = CONTAINER_OF(event->data.rx.buf, struct uart_data, data[0]);
		buf->len = MAX(buf->len, event->data.rx.offset + event->data.rx.len);

		/* Flush each burst after the configured UART idle timeout. */
		if (!rx_disable_pending && buf->len > 0) {
			rx_disable_pending = true;
			(void)uart_rx_disable(uart);
		}
		break;

	case UART_RX_BUF_REQUEST:
		buf = uart_buffer_alloc();
		if (buf != NULL) {
			(void)uart_rx_buf_rsp(uart, buf->data, sizeof(buf->data));
		}
		break;

	case UART_RX_BUF_RELEASED:
		buf = CONTAINER_OF(event->data.rx_buf.buf, struct uart_data, data[0]);
		if (buf->len > 0) {
			k_fifo_put(&uart_to_ble_fifo, buf);
		} else {
			k_free(buf);
		}
		break;

	case UART_RX_DISABLED:
		rx_disable_pending = false;
		k_work_reschedule(&uart_restart_work, K_NO_WAIT);
		break;

	case UART_RX_STOPPED:
		rx_disable_pending = false;
		LOG_WRN("UART RX stopped (%d)", event->data.rx_stop.reason);
		k_work_reschedule(&uart_restart_work, UART_RETRY_DELAY);
		break;

	default:
		break;
	}
}

static int uart_init(void)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	k_work_init_delayable(&uart_restart_work, uart_restart);

	int err = uart_callback_set(uart, uart_callback, NULL);
	if (err != 0) {
		return err;
	}

	struct uart_data *buf = uart_buffer_alloc();
	if (buf == NULL) {
		return -ENOMEM;
	}

	err = uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_RX_TIMEOUT);
	if (err != 0) {
		k_free(buf);
	}

	return err;
}

static void advertising_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, advertising_data,
				  ARRAY_SIZE(advertising_data), scan_response,
				  ARRAY_SIZE(scan_response));
	if (err != 0) {
		LOG_ERR("Advertising failed (%d)", err);
	} else {
		LOG_INF("Advertising as %s", DEVICE_NAME);
	}
}

static void connected_callback(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		LOG_WRN("Connection failed: 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	k_mutex_lock(&conn_lock, K_FOREVER);
	current_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_lock);
	atomic_set(&connected, 1);
	led_set(true);
	LOG_INF("BLE connected");

#if defined(CONFIG_NUS_UART_PAIRING)
	err = bt_conn_set_security(conn, BT_SECURITY_L3);
	if (err != 0) {
		LOG_WRN("Cannot request authenticated security (%d)", err);
	}
#endif
}

static void disconnected_callback(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	atomic_clear(&connected);
	atomic_clear(&notifications_enabled);
	k_sem_reset(&nus_tx_done);

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	k_mutex_unlock(&conn_lock);

	LOG_INF("BLE disconnected: 0x%02x %s", reason, bt_hci_err_to_str(reason));
}

static void recycled_callback(void)
{
	k_work_submit(&advertising_work);
}

#if defined(CONFIG_NUS_UART_PAIRING)
static void security_changed_callback(struct bt_conn *conn, bt_security_t level,
				      enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if (err == BT_SECURITY_ERR_SUCCESS) {
		LOG_INF("BLE security level %u", level);
	} else {
		LOG_WRN("BLE security failed: %d %s", err, bt_security_err_to_str(err));
	}
}
#endif

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected_callback,
	.disconnected = disconnected_callback,
	.recycled = recycled_callback,
#if defined(CONFIG_NUS_UART_PAIRING)
	.security_changed = security_changed_callback,
#endif
};

#if defined(CONFIG_NUS_UART_PAIRING)
static uint32_t pairing_passkey(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	return CONFIG_NUS_UART_PASSKEY;
}

static void pairing_cancelled(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	LOG_INF("Pairing cancelled");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	LOG_INF("Pairing complete, bonded: %d", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("Pairing failed: %d %s", reason, bt_security_err_to_str(reason));
}

static const struct bt_conn_auth_cb authentication_callbacks = {
	.cancel = pairing_cancelled,
	.app_passkey = pairing_passkey,
};

static struct bt_conn_auth_info_cb authentication_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};
#endif

static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(conn);

	for (uint16_t offset = 0; offset < len;) {
		struct uart_data *buf = uart_buffer_alloc();
		if (buf == NULL) {
			LOG_WRN("Dropping BLE data: no UART TX buffer");
			return;
		}

		buf->len = MIN((uint16_t)sizeof(buf->data), (uint16_t)(len - offset));
		memcpy(buf->data, &data[offset], buf->len);
		offset += buf->len;

		if (uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS) != 0) {
			k_fifo_put(&ble_to_uart_fifo, buf);
		}
	}
}

static void nus_sent(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	k_sem_give(&nus_tx_done);
}

static void nus_send_enabled(enum bt_nus_send_status status)
{
	atomic_set(&notifications_enabled, status == BT_NUS_SEND_STATUS_ENABLED);
}

static struct bt_nus_cb nus_callbacks = {
	.received = nus_received,
	.sent = nus_sent,
	.send_enabled = nus_send_enabled,
};

static struct bt_conn *connection_get(void)
{
	struct bt_conn *conn = NULL;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn != NULL) {
		conn = bt_conn_ref(current_conn);
	}
	k_mutex_unlock(&conn_lock);

	return conn;
}

static void uart_to_ble_thread(void)
{
	k_sem_take(&ble_ready, K_FOREVER);

	for (;;) {
		struct uart_data *buf = k_fifo_get(&uart_to_ble_fifo, K_FOREVER);

		if (!atomic_get(&connected) || !atomic_get(&notifications_enabled)) {
			LOG_DBG("Dropping UART data: NUS notifications are unavailable");
			k_free(buf);
			continue;
		}

		struct bt_conn *conn = connection_get();
		if (conn == NULL) {
			k_free(buf);
			continue;
		}

		uint16_t offset = 0;
		while (offset < buf->len && atomic_get(&connected)) {
			uint16_t mtu = (uint16_t)bt_nus_get_mtu(conn);
			uint16_t chunk = MIN((uint16_t)(buf->len - offset), mtu);
			int err;

			do {
				err = bt_nus_send(conn, &buf->data[offset], chunk);
				if (err == -ENOMEM || err == -EAGAIN) {
					k_sleep(K_MSEC(2));
				}
			} while ((err == -ENOMEM || err == -EAGAIN) && atomic_get(&connected));

			if (err != 0) {
				LOG_WRN("NUS send failed (%d)", err);
				break;
			}

			(void)k_sem_take(&nus_tx_done, BLE_TX_TIMEOUT);
			offset += chunk;
		}

		bt_conn_unref(conn);
		k_free(buf);
	}
}

K_THREAD_DEFINE(uart_to_ble_thread_id, 1536, uart_to_ble_thread, NULL, NULL, NULL,
		7, 0, 0);

int main(void)
{
	if (!gpio_is_ready_dt(&status_led) || gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE)) {
		return 0;
	}

#if defined(CONFIG_NUS_UART_PAIRING)
	erase_bonds = bond_reset_requested();
#endif

	if (uart_init() != 0) {
		LOG_ERR("UART initialization failed");
		for (;;) {
			led_set(true);
			k_sleep(K_MSEC(100));
			led_set(false);
			k_sleep(K_MSEC(100));
		}
	}

	int err;

#if defined(CONFIG_NUS_UART_PAIRING)
	err = bt_conn_auth_cb_register(&authentication_callbacks);
	if (err != 0) {
		LOG_ERR("Cannot register pairing callbacks (%d)", err);
		return 0;
	}

	err = bt_conn_auth_info_cb_register(&authentication_info_callbacks);
	if (err != 0) {
		LOG_ERR("Cannot register pairing info callbacks (%d)", err);
		return 0;
	}
#endif

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth initialization failed (%d)", err);
		return 0;
	}

#if defined(CONFIG_NUS_UART_PAIRING)
	settings_load();

	if (erase_bonds) {
		err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
		if (err != 0) {
			LOG_WRN("Cannot erase bonds (%d)", err);
		} else {
			LOG_INF("All stored BLE bonds erased");
		}
	}
#endif

	err = bt_nus_init(&nus_callbacks);
	if (err != 0) {
		LOG_ERR("NUS initialization failed (%d)", err);
		return 0;
	}

	k_work_init(&advertising_work, advertising_handler);
	k_sem_give(&ble_ready);
	k_work_submit(&advertising_work);

	bool blink = false;
	for (;;) {
		if (atomic_get(&connected)) {
			led_set(true);
		} else {
			blink = !blink;
			led_set(blink);
		}
		k_sleep(K_MSEC(500));
	}
}
