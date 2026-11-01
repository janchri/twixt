/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief MCUmgr service of twixt.
 */

#ifndef TWIXT_SERVICE_SMP_H_
#define TWIXT_SERVICE_SMP_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>

#include <twixt/twixt.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup twixt_smp MCUmgr service
 * @ingroup twixt
 * @{
 *
 * MCUmgr (SMP) packets between the MCUs (CONFIG_TWIXT_SMP), e.g. to update
 * the image of an MCU through another one that has Bluetooth or USB. A packet
 * goes in fragments, with a CRC-16 after it (CONFIG_TWIXT_SMP_CRC):
 *
 *     request:  fragment header, up to TWIXT_SMP_FRAG_MAX bytes of the packet
 *     response: no data
 *
 * The fragment header holds TWIXT_SMP_FIRST on the first fragment and the
 * fragment index in its low 7 bits. A request packet (SMP op read or write)
 * goes to this MCU's MCUmgr, whose response goes back to the sender the same
 * way; a response packet goes to twixt_smp_rsp_get(). A packet with another op
 * is dropped, and so is one whose sender stays silent for
 * CONFIG_TWIXT_SMP_TIMEOUT_MS before it is complete.
 *
 * While a packet comes in, the receiver ignores a fragment sent again and drops
 * the packet on a gap. That is not exactly-once: a fragment with
 * TWIXT_SMP_FIRST sent again starts the packet anew, so a packet of one
 * fragment runs twice, and the last fragment sent again gets -EPROTO although
 * the packet went through. After a failure of twixt_smp_send() the packet may
 * have run: sending again is safe only for a command that may run twice.
 */

/** Fragment header: first fragment of a packet */
#define TWIXT_SMP_FIRST 0x80

/** Most packet bytes in one fragment */
#define TWIXT_SMP_FRAG_MAX (TWIXT_REQ_DATA_MAX - 1)

/** @brief Packets dropped by this MCU */
struct twixt_smp_stats {
	/** Incomplete packets whose sender went silent */
	uint32_t expired;
	/** Responses nobody took: CONFIG_TWIXT_SMP_RSP_MAX waiting, or no request sent */
	uint32_t rsp_dropped;
};

/**
 * @brief Send an SMP packet.
 *
 * Blocks until every fragment went; one packet at a time from this MCU.
 *
 * @param addr MCU to send to.
 * @param pkt SMP packet, header included.
 * @param len Length of @p pkt.
 *
 * @return 0, or what twixt_request() returned for a fragment.
 */
int twixt_smp_send(uint8_t addr, const uint8_t *pkt, size_t len);

/**
 * @brief Take the next SMP response packet.
 *
 * Only an MCU that sent a request with twixt_smp_send() keeps responses, up to
 * CONFIG_TWIXT_SMP_RSP_MAX of them; further ones are dropped.
 *
 * With CONFIG_TWIXT_SMP_GATEWAY the gateway takes the responses.
 *
 * Responses are not matched to requests here. The caller must check @p src and
 * the sequence number in the SMP header: after a timeout, the response to an
 * earlier request may still come, and would look like the answer to the next.
 *
 * @param src Address of the MCU that sent it; may be NULL.
 * @param timeout How long to wait.
 *
 * @return The packet, to give back with smp_packet_free()
 *         (<zephyr/mgmt/mcumgr/smp/smp.h>), or NULL.
 */
struct net_buf *twixt_smp_rsp_get(uint8_t *src, k_timeout_t timeout);

/**
 * @brief Copy the counts of dropped packets.
 *
 * @param stats Copy.
 */
void twixt_smp_get_stats(struct twixt_smp_stats *stats);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TWIXT_SERVICE_SMP_H_ */
