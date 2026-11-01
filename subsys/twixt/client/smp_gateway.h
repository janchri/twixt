/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the core of the MCUmgr gateway and its front ends. All of it runs in the gateway's one
 * thread, except smp_gateway_wake().
 */

#ifndef SUBSYS_TWIXT_SMP_GATEWAY_H_
#define SUBSYS_TWIXT_SMP_GATEWAY_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/iterable_sections.h>

/* Sends a whole SMP packet back to the tool that sent the request */
#define SMP_HDR_LEN 8

typedef void (*smp_gateway_respond_fn)(const uint8_t *pkt, size_t len, void *ctx);

/* Passes a request to the MCU at addr and answers it: its response, or an SMP error. Frees req. */
void smp_gateway_forward(uint8_t addr, struct net_buf *req, smp_gateway_respond_fn respond,
			 void *ctx);

/* A front end has something to serve. From any context. */
void smp_gateway_wake(void);

/* A way in for SMP tools: start() once on the gateway's thread, serve() after every wake */
struct smp_gateway_frontend {
	void (*start)(void);
	void (*serve)(void);
};

#define SMP_GATEWAY_FRONTEND_DEFINE(name, start_fn, serve_fn)                                      \
	static const STRUCT_SECTION_ITERABLE(smp_gateway_frontend, name) = {                       \
		.start = start_fn,                                                                 \
		.serve = serve_fn,                                                                 \
	}

#endif /* SUBSYS_TWIXT_SMP_GATEWAY_H_ */
