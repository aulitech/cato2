/*
 * SPDX-License-Identifier: MIT
 *
 * Zephyr shell over Bluetooth, using the Nordic UART Service (NUS) layout so
 * that existing UART terminal apps work.
 *
 * The service is defined here rather than taken from the SDK because its
 * writable attributes must require an encrypted link: a device that has not
 * paired can neither send shell input nor subscribe to shell output, and its
 * attempt is answered with the error that makes phones and computers pair.
 *
 * The shell runs from boot and is shared: input from any connected device is
 * accepted and output goes to every device that has subscribed. It is dropped
 * while none has.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>

#include "app.h"

LOG_MODULE_REGISTER(ble_shell, LOG_LEVEL_INF);

#define UUID_NUS_RX BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e))
#define UUID_NUS_TX BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e))

/* Index of the TX characteristic value in the service's attribute table. */
#define TX_ATTR_INDEX 2

/* ATT header: opcode and attribute handle. */
#define NOTIFY_OVERHEAD 3

RING_BUF_DECLARE(rx_ringbuf, 256);

static shell_transport_handler_t shell_handler;
static void *shell_context;

static ssize_t on_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint32_t stored = ring_buf_put(&rx_ringbuf, buf, len);

	if (stored < len) {
		LOG_WRN("Input overflow, %u bytes dropped", len - stored);
	}
	if (shell_handler != NULL) {
		shell_handler(SHELL_TRANSPORT_EVT_RX_RDY, shell_context);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(nus_service,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(APP_UUID_NUS_SERVICE_VAL)),
	BT_GATT_CHARACTERISTIC(UUID_NUS_TX, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CHARACTERISTIC(UUID_NUS_RX,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, on_rx_write, NULL),
);

struct output {
	const uint8_t *data;
	size_t length;
};

static void write_to(struct bt_conn *conn, void *user_data)
{
	const struct bt_gatt_attr *tx_attr = &nus_service.attrs[TX_ATTR_INDEX];
	const struct output *output = user_data;
	size_t sent = 0;

	while (sent < output->length &&
	       bt_gatt_is_subscribed(conn, tx_attr, BT_GATT_CCC_NOTIFY)) {
		size_t chunk = MIN(output->length - sent,
				   bt_gatt_get_mtu(conn) - NOTIFY_OVERHEAD);

		if (bt_gatt_notify(conn, tx_attr, &output->data[sent], chunk) != 0) {
			break;
		}
		sent += chunk;
	}
}

/* ----------------------------------------------------- shell transport --- */

static int transport_init(const struct shell_transport *transport, const void *config,
			  shell_transport_handler_t handler, void *context)
{
	shell_handler = handler;
	shell_context = context;

	return 0;
}

static int transport_uninit(const struct shell_transport *transport)
{
	return 0;
}

static int transport_enable(const struct shell_transport *transport, bool blocking_tx)
{
	return 0;
}

static int transport_write(const struct shell_transport *transport, const void *data,
			   size_t length, size_t *count)
{
	struct output output = { .data = data, .length = length };

	/* Everything is always "written": with nobody listening it is dropped. */
	*count = length;

	if (!k_is_in_isr()) {
		bt_conn_foreach(BT_CONN_TYPE_LE, write_to, &output);
	}

	if (shell_handler != NULL) {
		shell_handler(SHELL_TRANSPORT_EVT_TX_RDY, shell_context);
	}

	return 0;
}

static int transport_read(const struct shell_transport *transport, void *data,
			  size_t length, size_t *count)
{
	*count = ring_buf_get(&rx_ringbuf, data, length);

	return 0;
}

static const struct shell_transport_api transport_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.write = transport_write,
	.read = transport_read,
};

static struct shell_transport ble_transport = {
	.api = &transport_api,
};

/* The prompt is replaced by the board's name once that is known. */
SHELL_DEFINE(shell_ble, "bt_nus:~$ ", &ble_transport, 256, 0, SHELL_FLAG_OLF_CRLF);

int ble_shell_set_prompt(const char *prompt)
{
	return shell_prompt_change(&shell_ble, prompt);
}

int ble_shell_init(void)
{
	static const struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	return shell_init(&shell_ble, NULL, flags, true, LOG_LEVEL_INF);
}
