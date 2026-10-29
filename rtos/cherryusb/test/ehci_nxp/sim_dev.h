/*
 * Simulated AIC8800 D80-like composite device behind the root port (sim_dev.c).
 *
 * Interface 0, vendor (Wi-Fi):     0x01 bulk OUT data, 0x02 bulk OUT message,
 *                                  0x81 bulk IN data,  0x82 bulk IN message (512 bytes)
 * Interface 1, E0/01/01 (btusb):   0x83 interrupt IN HCI events (16 bytes, bInterval 4 = 1 ms),
 *                                  0x84 bulk IN ACL, 0x03 bulk OUT ACL (512 bytes)
 * Interface 2, E0/01/01 (SCO):     alt 0 none, alt 1 0x85/0x04 isochronous (not exercised)
 * EP0 (64 bytes): standard requests, vendor 0xFF stalls, class OUT bRequest 0 = HCI command,
 * answered by a Command Complete event on 0x83 after a latency.
 *
 * Every frame on the data endpoints carries a header the receiver verifies, so loss,
 * duplication, reordering and corruption are counted, not guessed:
 *   Wi-Fi frames: u16 len, u16 seq, u8 ep, u8 kind, payload byte i = pat(seq, i, ep)
 *   ACL frames:   u16 handle (0x0001), u16 payload len, payload byte i = pat(seq, i, ep),
 *                 payload[0..1] = seq
 * A frame ends with a short packet (a zero-length packet when its length is a multiple of
 * the max packet size).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SIM_DEV_H
#define SIM_DEV_H

#include <stdint.h>

struct sim_dev_cfg {
    uint32_t nak_permille;     /* random NAK on any IN/OUT data transaction */
    uint32_t msg_latency_uf;   /* message response delay, max (random 0..max) */
    uint32_t hci_latency_uf;   /* HCI command -> Command Complete delay */
    uint32_t noresp_permille;  /* random transaction error (no handshake): CERR path */
    uint32_t acl_ncp_every;    /* Number Of Completed Packets event every N ACL OUT frames */
    int verbose;
};
extern struct sim_dev_cfg g_dev_cfg;

struct sim_ep_stats {
    uint64_t frames, bytes, bad, dup_or_gap, packets, naks;
};
struct sim_dev_stats {
    struct sim_ep_stats out[16], in[16];
    uint64_t toggle_errors, setups, stalls_sent, hci_cmds, hci_events_sent;
    uint8_t addr, config;
};
extern struct sim_dev_stats g_dev_stats;

static inline uint8_t sim_pat(uint32_t seq, uint32_t i, uint8_t ep)
{
    return (uint8_t)(seq * 31U + i * 7U + ep * 13U + (i >> 8));
}

/* test -> device: frames the device will send on an IN endpoint */
void sim_dev_in_enqueue(uint8_t ep, uint32_t nframes, uint32_t minlen, uint32_t maxlen);
uint32_t sim_dev_in_pending(uint8_t ep);
uint32_t sim_dev_in_next_seq(uint8_t ep);
/* inject an HCI event (bytes from the event code on) */
void sim_dev_hci_event(const uint8_t *evt, uint32_t len);
/* make an endpoint STALL its next transaction (cleared by CLEAR_FEATURE(HALT)) */
void sim_dev_halt_ep(uint8_t ep_addr);
/* accept `packets` more data packets on the endpoint, then NAK (-1: no limit) */
void sim_dev_block(uint8_t ep_addr, int32_t packets);
/* bytes of the frame in progress: received on an OUT endpoint, sent on an IN endpoint */
uint32_t sim_dev_out_partial(uint8_t ep_addr);
uint32_t sim_dev_in_partial(uint8_t ep_addr);
/* the expected next sequence of an OUT endpoint (frames received in order so far) */
uint32_t sim_dev_out_next_seq(uint8_t ep);

/* frame builders for the tests */
uint32_t sim_frame_wifi(uint8_t *buf, uint32_t len, uint32_t seq, uint8_t ep);
uint32_t sim_frame_acl(uint8_t *buf, uint32_t payload, uint32_t seq, uint8_t ep);
/* verify a received frame; returns its seq or -1 */
int64_t sim_frame_check(const uint8_t *buf, uint32_t len, uint8_t ep);

#endif
