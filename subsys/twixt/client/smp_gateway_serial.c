/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The serial front end of the MCUmgr gateway (twixt,smp-gateway-serial): to a tool, one port per
 * MCU, with serial SMP on it.
 */

#define DT_DRV_COMPAT twixt_smp_gateway_serial

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/sys/ring_buffer.h>

#include "smp_gateway.h"

LOG_MODULE_REGISTER(twixt_smp_gateway_serial, CONFIG_TWIXT_LOG_LEVEL);

struct port {
	const struct device *uart;
	uint8_t addr;
	struct ring_buf *rx;
	uint8_t *line;
	size_t line_len;
	struct mcumgr_serial_rx_ctxt ctxt;
};

/* Stops the host at a full buffer, by not taking its bytes; serve_port() takes them on again */
static void uart_isr(const struct device *dev, void *user_data)
{
	struct port *p = user_data;

	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev)) {
		uint8_t chunk[32];
		uint32_t room = MIN(ring_buf_space_get(p->rx), sizeof(chunk));
		int n;

		if (room == 0) {
			uart_irq_rx_disable(dev);
			break;
		}
		n = uart_fifo_read(dev, chunk, room);
		if (n <= 0) {
			break;
		}
		ring_buf_put(p->rx, chunk, n);
	}
	smp_gateway_wake();
}

/* Nothing to a host that has gone: with flow control, poll_out would wait for it for ever */
static const struct device *tx_uart;

static int host_tx(const void *data, int len)
{
	const struct device *uart = tx_uart;
	const uint8_t *bytes = data;
	uint32_t dtr = 0;

	if (uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) != 0 || dtr == 0) {
		return -ENODEV;
	}
	for (int i = 0; i < len; i++) {
		uart_poll_out(uart, bytes[i]);
	}
	return 0;
}

static void port_respond(const uint8_t *pkt, size_t len, void *ctx)
{
	const struct port *p = ctx;

	tx_uart = p->uart;
	(void)mcumgr_serial_tx_pkt(pkt, len, host_tx);
}

static void serve_port(struct port *p)
{
	uint8_t c;

	while (ring_buf_get(p->rx, &c, 1) == 1) {
		if (p->line_len < CONFIG_TWIXT_SMP_GATEWAY_LINE_MAX) {
			p->line[p->line_len++] = c;
		}
		if (c == '\n') {
			struct net_buf *req =
				mcumgr_serial_process_frag(&p->ctxt, p->line, p->line_len);

			p->line_len = 0;
			if (req != NULL) {
				if (req->len >= SMP_HDR_LEN) {
					smp_gateway_forward(p->addr, req, port_respond, p);
				} else {
					smp_packet_free(req);
				}
			}
		}
	}
	uart_irq_rx_enable(p->uart);
}

#define PORT_DEFINE(n)                                                                             \
	RING_BUF_DECLARE(port_rx_##n, CONFIG_TWIXT_SMP_GATEWAY_RX_BUF_SIZE);                       \
	static uint8_t port_line_##n[CONFIG_TWIXT_SMP_GATEWAY_LINE_MAX];

DT_INST_FOREACH_STATUS_OKAY(PORT_DEFINE)

#define PORT_INIT(n)                                                                               \
	{                                                                                          \
		.uart = DEVICE_DT_GET(DT_INST_PHANDLE(n, serial)),                                 \
		.addr = DT_REG_ADDR(DT_INST_PHANDLE(n, peer)),                                     \
		.rx = &port_rx_##n,                                                                \
		.line = port_line_##n,                                                             \
	},

static struct port ports[] = {DT_INST_FOREACH_STATUS_OKAY(PORT_INIT)};

static void ports_start(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		if (!device_is_ready(ports[i].uart)) {
			LOG_ERR("%s not ready", ports[i].uart->name);
			continue;
		}
		uart_irq_callback_user_data_set(ports[i].uart, uart_isr, &ports[i]);
		uart_irq_rx_enable(ports[i].uart);
	}
}

static void ports_serve(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		serve_port(&ports[i]);
	}
}

SMP_GATEWAY_FRONTEND_DEFINE(serial_frontend, ports_start, ports_serve);
