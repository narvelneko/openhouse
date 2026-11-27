/* openhouse link v1: tracker radio (nRF51822) <-> dongle (nRF52840). Shared by both firmwares.
 *
 * Air: Nordic proprietary 2 Mbit, 8-bit preamble, 4+1 byte address, 8-bit length, CRC16 (0x11021,
 * init 0xFFFF), no whitening.
 *
 * Network: the dongle leads. A cycle = one POLL per active slot (each answered by that tracker's RESP),
 * all on one channel; every 20 ms the dongle also sends a JOIN beacon on the join channel. Cycles hop
 * over 16 channels: channel = hop_ch[allowed[cycle % n_allowed]], allowed = set bits of the mask the
 * previous POLL announced. Each slot has its own address (BASE1 = per-dongle network base, prefix
 * DML_SLOT_PREFIX + slot), so a tracker's radio only ever hears polls for its own slot and can answer
 * with a hardware turnaround (END->DISABLE->TXEN shortcut, ~150 us) using a RESP prepared beforehand.
 *
 * Timing: fixed cycle period P; the k-th scheduled slot is polled at cycle_start + k * slot_time, so a
 * tracker that misses polls keeps hopping on its own clock (cycle = last heard + round(dt / P)).
 *
 * POLL  [type][slot][flags][cycle][mask lo][mask hi][period/50us][max resp len][downlink frames...]
 *        flags: bits 0-2 ack (last in-order RESP seq received), bit 3 dseq, bit 4 has downlink
 *        mask: allowed hop channels for the NEXT cycle
 * RESP  [type][slot][flags][rssi][missed][crc_err][uplink frames...]
 *        flags: bits 0-2 seq (go-back-N, window 2), bit 3 dack (last downlink dseq received), bit 4 more
 *        (frames still queued after this answer: the dongle's saturation measure, radios 0x6A000025 / 0x0A000016+)
 *        rssi: -dBm of a recent poll; missed/crc_err: tracker counters mod 256
 * JOIN  [type][net base x4][flags][assign id x8][assign slot][mask lo][mask hi][cycle][period/50us]
 *        [us since the assigned slot's poll, u16]           (join address, join channel)
 *        flags bit0: pairing open; assign id = tracker DEVICEID the slot is handed to (0 = none)
 *        the hop fields let a new tracker follow the hops at once (the first hop channel may be off)
 *        flags bit1: the assigned tracker's coded frames carry the codec's CRC-8 (it asked, JREQ bit1);
 *        bit2: it sends batches (it asked, JREQ bit2)
 * JREQ  [type][tracker id x8][flags]                               (answer to JOIN)
 *        flags bit0: pairing mode; bit1: can add the codec's CRC-8 (doorman_codec.h `check`); bit2: can send
 *        batches (doorman_lh2p.h: every frame queued for an answer coded together, LH2 light predicted)
 * frames = repeated [len][len bytes]: exactly the frames the tracker MCU and its radio exchange over SPI.
 *
 * Repeater dongles (called satellites in the code), dongle firmware 0x0031. These go only between dongles;
 * device radios never receive them, because their address base, DML_SAT_BASE(net base), is no slot or join
 * address. Slot masks are u16, bit = slot.
 * NEED  [06][sat id][next cycle][mask lo][mask hi][period/50us][us from this ADDRESS to the next cycle's
 *        start, u16][listen u16][wanted u16][max SREP len][flags][expected seq of each wanted slot, in slot
 *        order]                                                (main -> repeater, prefix 0xC0 + sat id)
 *        The main sends one to each repeater in use after the device slots (to an absent repeater only every
 *        16th cycle; none in a cycle without room). wanted: the slots whose next answer the main is missing
 *        (lost this cycle, or held behind a lost one, see DML_F_SACK); 0 means no reply is expected. listen:
 *        the slots the repeater listens to in the next cycle (at most 7: the radio has 8 addresses, one is the
 *        NEED's). Right after a NEED, the repeater moves to the next cycle's channel. flags bit 0: the next
 *        start is unknown (the cycle overran).
 * SREP  [07][sat id][cycle][heard u16][-dBm of each heard slot, in slot order][records: [slot][n][n bytes of
 *        the RESP as heard]...]   (repeater -> main: the reply to a NEED with wanted != 0, same address,
 *                                  at most max SREP len bytes)
 *        heard: the slots whose RESP the repeater received with a good CRC this cycle. For each wanted slot,
 *        the repeater keeps the RESPs it heard in this cycle and the one before and sends the one with the
 *        expected seq, then the one after it (seq + 1) if it has that too, or only that one: the main takes the
 *        first and keeps the second as a selective ack would. Records that don't fit are left out.
 */
#pragma once
#include <stdint.h>

#include "doorman_versions.h"
#ifdef DML_IDX
#define DML_VERSION        DML_VERSION_IDX
#else
#define DML_VERSION        DML_VERSION_HTC
#endif        /* radio fw version reported for 'X' (HTC: 0x576B0104) */
#define DML_AIR_UPD        0xF0u              /* downlink/uplink frame op: radio's own updater */
#define DML_JOIN_CHANNEL   80                 /* 2480 MHz */
#define DML_JOIN_BASE      0x4F484A4Eu        /* "OHJN" */
#define DML_JOIN_PREFIX    0x5Au
#define DML_SLOT_PREFIX    0xA0u
#define DML_MAXLEN         250
#define DML_SLOTS          10                 /* radios before 0x6A000026 / 0x0A000017 take slots 0-7 only */
#define DML_NHOP           16
static const uint8_t dml_hop_ch[DML_NHOP] = { 4, 9, 14, 19, 24, 29, 34, 39, 44, 49, 54, 59, 64, 69, 74, 79 };

enum { DML_POLL = 0x01, DML_RESP = 0x02, DML_JOIN = 0x03, DML_JREQ = 0x04 };
#define DML_POLL_HDR       8
#define DML_RESP_HDR       6
#define DML_JOIN_LEN       21
/* From dongle 0x0032 the beacon is 2 bytes longer (radios take >= DML_JOIN_LEN): [21] the worst device loss
   %, as the main alone (0-100); [22] repeaters in use (bits 0-1) | 0x80 relaying now. Repeaters pick a main by them. */
#define DML_JOIN_LEN_X     28
/* [23] a repeater it took on its own: id | 0x80 (0: none) and [24..27] that repeater's net base. The main
   names each such repeater in turn, so they know they still have their place. */
/* Tagged feature reads: the host sends [0xC5][tag][report id]; the radio asks the main MCU with
   ['P'][report id] and sends the answer back as [0xC4][tag][answer without the 'P']. The main MCU's
   answers carry no id, so without the tag a late answer was taken for the next request. */
#define DML_TAGGED_GET     0xC5
#define DML_TAGGED_REPLY   0xC4
#define DML_JREQ_LEN       10
#define DML_F_SEQ_MASK     0x07
#define DML_F_DSEQ         0x08
#define DML_F_DOWN         0x10
#define DML_F_SACK         0x20               /* POLL: the dongle already holds the answer after the one it acks (seq ack+2);
                                                 resend only the missing one */
#define DML_F_MORE         0x10               /* RESP: more queued than this answer held */
#define DML_JF_PAIR        0x01               /* JOIN: pairing open; JREQ: pairing mode */
#define DML_JF_CHECK       0x02               /* JREQ: can add the codec CRC-8; JOIN: do (for the assigned id) */
#define DML_JF_BATCH       0x04               /* JREQ: can send batches (doorman_lh2p.h); JOIN: do (for the assigned id) */
#define DML_JF_BATCH32      0x08               /* with BATCH: wide batches, up to 32 frames (doorman_lh2p.h DMC_BATCH_W) */
/* Repeater dongles: NEED / SREP (see above). */
enum { DML_NEED = 0x06, DML_SREP = 0x07, DML_SREQ = 0x08 };
/* SREQ [type][repeater net base x4][main net base x4]: a repeater without a set main asks for a place, in reply
   to that main's JOIN (as a JREQ, same address), and again every few seconds to keep it. */
#define DML_SREQ_LEN       9
/* From 0x0032 it goes on with the repeater's status, for the host to show (a repeater on a charger has no USB):
   [9..16] its device id (FICR DEVICEID[1], [0]: the USB serial), [17] board (cmd 0x30 byte 57), [18..19]
   firmware, [20] -dBm it hears the main at, [21] its LED state (link_led). */
#define DML_SREQ_LEN_X     22
#define DML_SAT_PREFIX     0xC0u              /* + repeater id */
#define DML_SAT_MAX        2
#define DML_SAT_LISTEN     7                  /* slots a repeater listens to (radio addresses 1-7) */
#define DML_NEED_HDR       14
#define DML_SREP_HDR       5
#define DML_NEED_F_NOSTART 0x01               /* NEED flag: no valid next start (the cycle overran) */
#define DML_SREP_TURN_US   60                 /* NEED end -> repeater TXEN (main receives from ~45 us) */
/* Top bit set in every byte: never a network base (they have it clear) or the join base. */
#define DML_SAT_BASE(net)  ((net) ^ 0x80808080u)

/* channel for a cycle: the (cycle % n)-th allowed hop channel */
static inline uint8_t dml_channel(uint8_t cycle, uint16_t mask)
{
  int n = 0;
  for (int i = 0; i < DML_NHOP; i++) n += (mask >> i) & 1;
  if (n == 0) return dml_hop_ch[0];
  int k = cycle % n;
  for (int i = 0; i < DML_NHOP; i++)
    if ((mask >> i) & 1) { if (k == 0) return dml_hop_ch[i]; k--; }
  return dml_hop_ch[0];
}
