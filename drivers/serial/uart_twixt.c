/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART over twixt (dts/bindings/serial/twixt,uart.yaml).
 *
 *     request:  TWIXT_SERVICE_UART, chunk of up to CONFIG_UART_TWIXT_CHUNK bytes
 *     response: how many bytes the peer took
 *
 * What the peer did not take is sent again, so a full receive buffer holds the
 * sender back. If nobody answers, the bytes are dropped, as without a cable.
 * Received bytes go to the UART whose peer is the request's src.
 *
 * A work item per UART sends and plays the UART interrupt for the
 * interrupt-driven API. The work items share one work queue.
 */

#define DT_DRV_COMPAT twixt_uart

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/ring_buffer.h>

#include <twixt/twixt.h>

LOG_MODULE_REGISTER(uart_twixt, CONFIG_UART_LOG_LEVEL);

#define CHUNK CONFIG_UART_TWIXT_CHUNK

BUILD_ASSERT(CHUNK <= TWIXT_REQ_DATA_MAX, "a chunk must fit CONFIG_I2C_MM_MSG_MAX");

#define POLL_OUT_TIMEOUT_MS 100

struct uart_twixt_config {
	uint8_t peer;
};

struct uart_twixt_data {
	const struct device *dev;
	struct k_work_delayable work;
	struct k_spinlock lock;
	struct ring_buf rx;
	struct ring_buf tx;
	uint8_t rx_buf[CONFIG_UART_TWIXT_RX_BUF_SIZE];
	uint8_t tx_buf[CONFIG_UART_TWIXT_TX_BUF_SIZE];
	uart_irq_callback_user_data_t cb;
	void *cb_data;
	bool rx_irq;
	bool tx_irq;
};

static K_THREAD_STACK_DEFINE(uart_twixt_stack, CONFIG_UART_TWIXT_WORKQ_STACK_SIZE);
static struct k_work_q uart_twixt_workq;

static void uart_twixt_kick(struct uart_twixt_data *d)
{
	k_work_reschedule_for_queue(&uart_twixt_workq, &d->work, K_NO_WAIT);
}

static int uart_twixt_fifo_fill(const struct device *dev, const uint8_t *buf, int size)
{
	struct uart_twixt_data *d = dev->data;
	uint32_t n;

	K_SPINLOCK(&d->lock) {
		n = ring_buf_put(&d->tx, buf, size);
	}
	if (n > 0) {
		uart_twixt_kick(d);
	}
	return n;
}

static int uart_twixt_fifo_read(const struct device *dev, uint8_t *buf, const int size)
{
	struct uart_twixt_data *d = dev->data;
	uint32_t n;

	K_SPINLOCK(&d->lock) {
		n = ring_buf_get(&d->rx, buf, size);
	}
	return n;
}

static int uart_twixt_poll_in(const struct device *dev, unsigned char *c)
{
	return uart_twixt_fifo_read(dev, c, 1) == 1 ? 0 : -1;
}

/* Cannot wait in an interrupt, nor in the work queue that makes the room */
static void uart_twixt_poll_out(const struct device *dev, unsigned char c)
{
	if (k_is_in_isr() || k_current_get() == k_work_queue_thread_get(&uart_twixt_workq)) {
		uart_twixt_fifo_fill(dev, &c, 1);
		return;
	}
	(void)WAIT_FOR(uart_twixt_fifo_fill(dev, &c, 1) == 1, POLL_OUT_TIMEOUT_MS * USEC_PER_MSEC,
		       k_msleep(1));
}

/* The interrupt-driven API: uart_twixt_work() calls the callback while it is pending */

static void uart_twixt_irq_tx_enable(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	d->tx_irq = true;
	uart_twixt_kick(d);
}

static void uart_twixt_irq_tx_disable(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	d->tx_irq = false;
}

static int uart_twixt_irq_tx_ready(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	return d->tx_irq && ring_buf_space_get(&d->tx) > 0;
}

static int uart_twixt_irq_tx_complete(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	return ring_buf_is_empty(&d->tx);
}

static void uart_twixt_irq_rx_enable(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	d->rx_irq = true;
	uart_twixt_kick(d);
}

static void uart_twixt_irq_rx_disable(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	d->rx_irq = false;
}

static int uart_twixt_irq_rx_ready(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	return !ring_buf_is_empty(&d->rx);
}

static int uart_twixt_irq_is_pending(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	return uart_twixt_irq_tx_ready(dev) || (d->rx_irq && uart_twixt_irq_rx_ready(dev));
}

static void uart_twixt_irq_callback_set(const struct device *dev, uart_irq_callback_user_data_t cb,
					void *user_data)
{
	struct uart_twixt_data *d = dev->data;

	d->cb = cb;
	d->cb_data = user_data;
}

static void uart_twixt_irq_run(const struct device *dev)
{
	struct uart_twixt_data *d = dev->data;

	if (d->cb != NULL && uart_twixt_irq_is_pending(dev)) {
		d->cb(dev, d->cb_data);
	}
}

static DEVICE_API(uart, uart_twixt_api) = {
	.poll_in = uart_twixt_poll_in,
	.poll_out = uart_twixt_poll_out,
	.fifo_fill = uart_twixt_fifo_fill,
	.fifo_read = uart_twixt_fifo_read,
	.irq_tx_enable = uart_twixt_irq_tx_enable,
	.irq_tx_disable = uart_twixt_irq_tx_disable,
	.irq_tx_ready = uart_twixt_irq_tx_ready,
	.irq_tx_complete = uart_twixt_irq_tx_complete,
	.irq_rx_enable = uart_twixt_irq_rx_enable,
	.irq_rx_disable = uart_twixt_irq_rx_disable,
	.irq_rx_ready = uart_twixt_irq_rx_ready,
	.irq_is_pending = uart_twixt_irq_is_pending,
	.irq_callback_set = uart_twixt_irq_callback_set,
};

/* 1: bytes went, 0: none to send, -EAGAIN: try again later */
static int uart_twixt_send(const struct device *dev)
{
	const struct uart_twixt_config *cfg = dev->config;
	struct uart_twixt_data *d = dev->data;
	uint8_t chunk[CHUNK];
	uint8_t taken = 0;
	uint32_t n;
	int ret;

	K_SPINLOCK(&d->lock) {
		n = ring_buf_peek(&d->tx, chunk, CHUNK);
	}
	if (n == 0) {
		return 0;
	}

	ret = twixt_request(cfg->peer, TWIXT_SERVICE_UART, chunk, n, &taken, 1);
	if (ret == -EAGAIN || ret == -EBUSY) {
		return -EAGAIN;
	}
	if (ret) {
		taken = n; /* nobody answers: drop, as without a cable */
	}
	K_SPINLOCK(&d->lock) {
		ring_buf_consume(&d->tx, MIN(taken, n));
	}
	return taken > 0 ? 1 : -EAGAIN;
}

static void uart_twixt_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct uart_twixt_data *d = CONTAINER_OF(dwork, struct uart_twixt_data, work);
	int ret;

	uart_twixt_irq_run(d->dev);
	ret = uart_twixt_send(d->dev);
	if (ret > 0) {
		uart_twixt_kick(d);
	} else if (ret < 0) {
		k_work_schedule_for_queue(&uart_twixt_workq, dwork,
					  K_MSEC(CONFIG_UART_TWIXT_RETRY_MS));
	}
}

static int uart_twixt_init(const struct device *dev)
{
	static bool started;
	struct uart_twixt_data *d = dev->data;

	if (!started) {
		const struct k_work_queue_config cfg = {.name = "uart_twixt"};

		k_work_queue_start(&uart_twixt_workq, uart_twixt_stack,
				   K_THREAD_STACK_SIZEOF(uart_twixt_stack),
				   CONFIG_UART_TWIXT_WORKQ_PRIORITY, &cfg);
		started = true;
	}

	d->dev = dev;
	k_work_init_delayable(&d->work, uart_twixt_work);
	ring_buf_init(&d->rx, sizeof(d->rx_buf), d->rx_buf);
	ring_buf_init(&d->tx, sizeof(d->tx_buf), d->tx_buf);
	return 0;
}

#define UART_TWIXT_DEFINE(n)                                                                       \
	static struct uart_twixt_data uart_twixt_data_##n;                                         \
	static const struct uart_twixt_config uart_twixt_config_##n = {                            \
		.peer = DT_REG_ADDR(DT_INST_PHANDLE(n, peer)),                                     \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, uart_twixt_init, NULL, &uart_twixt_data_##n,                      \
			      &uart_twixt_config_##n, POST_KERNEL, CONFIG_SERIAL_INIT_PRIORITY,    \
			      &uart_twixt_api);

DT_INST_FOREACH_STATUS_OKAY(UART_TWIXT_DEFINE)

#define UART_TWIXT_DEV(n) DEVICE_DT_INST_GET(n),

static const struct device *const uarts[] = {DT_INST_FOREACH_STATUS_OKAY(UART_TWIXT_DEV)};

static int uart_twixt_serve(const struct twixt_req *req, uint8_t *rsp_data)
{
	for (size_t i = 0; i < ARRAY_SIZE(uarts); i++) {
		const struct uart_twixt_config *cfg = uarts[i]->config;
		struct uart_twixt_data *d = uarts[i]->data;
		uint32_t n;

		if (cfg->peer != req->src) {
			continue;
		}
		K_SPINLOCK(&d->lock) {
			n = ring_buf_put(&d->rx, req->data, req->len);
		}
		if (n > 0) {
			uart_twixt_kick(d);
		}
		rsp_data[0] = n; /* the rest the sender sends again */
		return 1;
	}
	return -ENODEV;
}

TWIXT_SERVICE_DEFINE(uart_twixt_service, TWIXT_SERVICE_UART, uart_twixt_serve);
