/* openhouse link v1, dongle side (see common/doorman_link.h): slots, joining/pairing, hopping, go-back-N.
 * A main dongle runs everything from the main loop. A repeater dongle receives in the RADIO interrupt, so it
 * doesn't miss packets while the main loop serves USB.
 *
 * Timers and PPI channels:
 *   TIMER3  1 MHz, free running: the link's microsecond clock. CC[0]: us_now(). CC[1]: the compare that
 *           starts a POLL in timing mode 3, or a repeater's SREP. CC[2]: time of the last RADIO ADDRESS.
 *           CC[4]: time of the last RADIO TXREADY.
 *   TIMER2  counts RADIO END events, so a repeater's interrupt can tell that it ran too late for one.
 *   PPI 0   the amplifier's turnaround (Holyiot only, see fem_*).
 *   PPI 1   TIMER3 CC[1] -> RADIO TXEN; its fork switches the amplifier to TX (Holyiot).
 *   PPI 2   RADIO DISABLED -> RADIO RXEN: receive the answer right after a POLL is sent. It's in PPI group 0,
 *           which its fork disables, so it fires once each time it's armed.
 *   PPI 3   RADIO ADDRESS -> TIMER3 CAPTURE[2].  PPI 7: RADIO TXREADY -> TIMER3 CAPTURE[4].
 *   PPI 6   RADIO END -> TIMER2 COUNT. */
#include <string.h>
#include "nrf.h"
#include "doorman_link.h"
#pragma GCC push_options
#pragma GCC optimize("O2")   /* the codec: see decode_step */
#include "doorman_codec.h"
#include "doorman_lh2p.h"
#pragma GCC pop_options
#include "sizing.h"
#include "link.h"
#include "dongle.h"
#include "banks.h"

extern volatile uint32_t ms_ticks;
/* Code size (the image has to fit the SoftDevice boot stub, ld/stub_sd.ld):
   KEEP_PTR hides how a pointer was computed, so the compiler keeps it in a register. At -Os it otherwise works
   out &sl[i] again (sl + i * sizeof, 8 bytes) at every use.
   GRP_*: variables that share a section are reached through one address (a section anchor). With -fdata-sections
   every other variable is a section of its own and costs each function that uses it 4 bytes for its address.
   The big buffers stay out: with them in a group, the offsets no longer fit the short instructions. */
#define KEEP_PTR(p) __asm__("" : "+r"(p))
#define GRP_M __attribute__((section(".bss.link_m")))    /* repeaters, main side; timing mode 3 */
#define GRP_R __attribute__((section(".bss.link_r")))    /* repeater side */
#define GRP_D __attribute__((section(".data.link")))     /* either, with an initial value */
static uint32_t us_now(void) { NRF_TIMER3->TASKS_CAPTURE[0] = 1; return NRF_TIMER3->CC[0]; }
/* us_now() out of line: half the bytes a use, ~0.1 us slower. For where that is fine. */
static __attribute__((noinline)) uint32_t us_time(void) { return us_now(); }

/* ---------------- paired trackers (persisted in the last app flash page) ---------------- */
#define PAIR_MAGIC  0x3150484Fu        /* "OHP1": DML_SLOTS slots */
#define PAIR_MAGIC8 0x5250484Fu        /* "OHPR": the 8-slot page of fw 0x0029 and earlier, moved over at boot */
/* Names (cmd 0x34/0x35), the user's own for each paired device, kept here so they go wherever the
   dongle goes. A page from older firmware ends after id[] (erased: names_magic is 0xFFFFFFFF), and
   reads as no names. UTF-8, NUL-padded; at most DML_NAME_MAX bytes (what one command carries). */
#define NAME_MAGIC  0x4D4E484Fu        /* "OHNM" */
#define DML_NAME_MAX 59
/* Radio versions (cmd 0x38): each paired device's radio firmware as it last said ('X' answers seen on
   their way to the host), so a host can tell a device needs an update while it's off. An older page
   ends before them (erased: radios_magic 0xFFFFFFFF) and reads as none known. */
#define RADIO_MAGIC 0x5652484Fu        /* "OHRV" */
static int pairs_dirty;                /* a radio version changed: saved from link_poll, not mid-frame */
#define ROLE_MAGIC  0x4C52484Fu        /* "OHRL" */
/* The repeater settings (cmd 0x3F) follow the radio versions. Older firmware ignores them, so a repeater that
   is downgraded keeps its pairings and names and works as a normal dongle. Anything stored after them must
   come after this block, with a magic of its own. */
typedef struct {
  uint32_t rmagic;                     /* ROLE_MAGIC: the fields below are set (else: a main, no repeaters) */
  uint8_t role;                        /* 0: main (a normal dongle), 1: repeater */
  uint8_t sat_id;                      /* repeater: its id, 0 or 1 */
  uint8_t sat_mask;                    /* main: the repeater ids it uses, one bit each */
  uint8_t rsv;                         /* unused */
  uint32_t main_base;                  /* repeater: the network base of the main it serves */
} role_store_t;
typedef struct {
  uint32_t magic, used; uint8_t id[DML_SLOTS][8]; uint32_t names_magic; char name[DML_SLOTS][64];
  uint32_t radios_magic; uint32_t radio[DML_SLOTS];
  role_store_t r;
} pair_store_t;
static pair_store_t pairs;
/* The repeater ids the main uses (cmd 0x3C). A change applies in RAM at once. It's written to flash
   (pairs.r.sat_mask) only when the host asks, or once no device is connected: erasing the page stops the CPU
   for ~85 ms, long enough for connected devices to lose the dongle's hopping. */
static GRP_M uint8_t sat_on;
/* Repeaters that asked for a place on their own (SREQ, not saved to flash): their net base and when they last
   asked or answered. Dropped after SAT_AUTO_DROP_MS without either. */
#define SAT_AUTO_DROP_MS 12000u
static GRP_M uint8_t sat_auto, sat_annc;
/* How strongly the main hears each repeater (its SREPs and SREQs), -dBm x16. Weaker than SAT_WEAK_DBM costs
   relays (the driver tells the user to bring them closer; the repeater flashes its LED). */
#define SAT_WEAK_DBM 80u
static GRP_M uint16_t sat_rssi16[2];
static GRP_M uint8_t sat_info[2][DML_SREQ_LEN_X - DML_SREQ_LEN];   /* each auto repeater's status (SREQ from byte 9) */
static void sat_rssi_add(int k, uint8_t v) { sat_rssi16[k] = sat_rssi16[k] ? (uint16_t)(sat_rssi16[k] - sat_rssi16[k] / 8 + v * 2) : (uint16_t)(v * 16); }
typedef struct { uint32_t asks, sreps, acc, last_ms; uint16_t quiet; } satst_t;
static GRP_M satst_t sats[DML_SAT_MAX];
static GRP_M uint32_t sat_rbase[2], sat_seen_ms[2];

static void nvmc_wait(void) { while (!NRF_NVMC->READY) {} }
/* the 8-slot page; repeater firmware 0x0028 kept its settings right after it */
typedef struct { uint32_t magic, used; uint8_t id[8][8]; uint32_t names_magic; char name[8][64]; role_store_t r; } pair_store8_t;
static void pairs_save(void);
static void pairs_load(void)
{
  const pair_store8_t *old = (const pair_store8_t *)PAIR_PAGE;
  const int convert = old->magic == PAIR_MAGIC8;
  if (convert) {                                            /* 8 slots: same pairings, names and role, two slots more */
    memset(&pairs, 0, sizeof pairs);
    pairs.magic = PAIR_MAGIC; pairs.used = old->used & 0xFF;
    memcpy(pairs.id, old->id, sizeof old->id);
    pairs.names_magic = NAME_MAGIC;
    pairs.radios_magic = RADIO_MAGIC;
    if (old->names_magic == NAME_MAGIC) memcpy(pairs.name, old->name, sizeof old->name);
    pairs.r = old->r;
  } else memcpy(&pairs, (const void *)PAIR_PAGE, sizeof pairs);
  if (pairs.magic != PAIR_MAGIC) { memset(&pairs, 0, sizeof pairs); pairs.magic = PAIR_MAGIC; }
  if (pairs.names_magic != NAME_MAGIC) { memset(pairs.name, 0, sizeof pairs.name); pairs.names_magic = NAME_MAGIC; }
  if (pairs.radios_magic != RADIO_MAGIC) { memset(pairs.radio, 0, sizeof pairs.radio); pairs.radios_magic = RADIO_MAGIC; }
  role_store_t *r = &pairs.r;
  if (r->rmagic != ROLE_MAGIC || r->role > 1 || r->sat_id >= DML_SAT_MAX) {
    r->rmagic = ROLE_MAGIC; r->role = 0; r->sat_id = 0; r->sat_mask = 0; r->rsv = 0; r->main_base = 0;
  }
  r->sat_mask &= (1u << DML_SAT_MAX) - 1;
  /* a repeater without a main (main_base 0) picks one itself */
  if (convert) pairs_save();
}
static void pairs_save(void)
{
  extern uint32_t __etext;                                  /* never erase our own code */
  if ((uint32_t)&__etext > PAIR_PAGE) return;
  NRF_NVMC->CONFIG = 2; nvmc_wait();
  NRF_NVMC->ERASEPAGE = PAIR_PAGE; nvmc_wait();
  NRF_NVMC->CONFIG = 1; nvmc_wait();
  const uint32_t *w = (const uint32_t *)&pairs;
  for (unsigned i = 0; i < sizeof pairs / 4; i++) { ((volatile uint32_t *)PAIR_PAGE)[i] = w[i]; nvmc_wait(); }
  NRF_NVMC->CONFIG = 0; nvmc_wait();
}

/* ---------------- per-slot state ---------------- */
#define UPQ 1024
#define UPQ_REPLIES 256                 /* kept for replies: data may only fill UPQ - this */
typedef struct {
  uint8_t active;                       /* answered recently */
  uint8_t last_seq;                     /* last in-order RESP seq (the ack we send) */
  uint8_t dseq, down_len, down_buf[200];
  uint32_t last_resp_ms;
  uint32_t poll_t;                      /* start time of its last POLL */
  uint8_t down_sent;                    /* downlink bytes sent and not yet acked: the start of down_buf */
  uint8_t upq[UPQ]; uint16_t up_head, up_tail;
  /* stats */
  uint32_t polls, resp_ok, crc_err, timeouts, dup, frames_up, bytes_up, drops, down_frames, down_resends;
  uint32_t rssi_d16, rssi_t16, t_missed, t_crc;
  uint8_t last_missed, last_crc, have_tel;
  uint8_t dup_run;                      /* consecutive out-of-order answers (a stalled uplink) */
  uint8_t check;                        /* its radio adds the codec CRC-8 (JREQ DML_JF_CHECK) */
  uint8_t batch;                        /* its radio sends batches (JREQ DML_JF_BATCH) */
  uint32_t rx_t, imu_q_t;               /* latency (cmd 0x3A): its last answer in, its last IMU sample queued */
  uint16_t batch_mismatch;              /* answers that didn't match the agreed batches: made to rejoin */
  /* a batch being decoded, a frame a step; its frames held until its CRC-8 passes, so a corrupted one
     never reaches the host */
  dmp_state_t lh2p;
  dmb_r_t br;
  uint8_t in_batch, bn, bql[DMB_MAX_W];
  /* Selective ack: an answer that came while the one before it was lost, kept until that one arrives (both then go
     on in order) and announced in the POLL (DML_F_SACK), so the radio resends only the lost one. Go-back alone cost
     two cycles and a wasted answer per loss. */
  uint8_t held, held_seq, held_n, held_buf[DML_MAXLEN];
  uint8_t late_state;                   /* its radio's late answers as it last said (0xFF: not known yet) */
  uint8_t ver_asked;                    /* its radio was asked for its version (air 'X') since it joined */
  uint32_t late_ask_ms;                 /* when late answers were last sent to it (0: nothing waiting) */
  uint32_t held_used, held_set;
  uint8_t bq[DMB_MAX_W][DMB_MAXF];          /* a batch's frames, held until its CRC-8 passes */
  uint32_t resyncs;                     /* codec out of step: made to rejoin */
  uint32_t skip_until;                  /* ms_ticks until which this slot is not polled */
  dmc_state_t codec; uint32_t air_bytes, bad_frames;
  uint16_t dev_drops, dev_reply_drops;   /* the radio's own drop totals (its 'D' frames) */
  uint8_t rssi_last_d, rssi_last_t, rssi_min_d, rssi_max_d;
  uint8_t ident[16];                    /* last 'H' frame, ident[0] = 0: none yet */
  uint16_t frag_left;                   /* bytes of a big frame still to send as 0x81 fragments */
  uint16_t pk_rest;                     /* packed reports: bytes of an item that started in an earlier report */
  uint8_t pk_seq;
  uint16_t upq_hw;                      /* most bytes waiting for USB since the last 0x28 read */
  uint8_t pend[DML_MAXLEN], pend_n, pend_k;   /* the last answer's frames, decoded from pend_k on */
  /* Ghost-follow: the hop state the device took from its last two answered POLLs (when the POLL went out, cycle,
     announced mask, period as the device keeps it; [0] valid when g_have), and the polls it missed since */
  uint32_t g_t[2]; uint16_t g_mask[2], g_per[2], g_miss; uint8_t g_cyc[2], g_have;
  uint32_t load_bytes, load_budget;     /* test load (0x2B): filler queued, and owed */   /* raw -dBm; min/max reset on read (0x29) */
} slot_t;
static slot_t sl[DML_SLOTS];
static uint8_t link_on = 1;
static uint32_t net_base;
static uint8_t cycle;
static uint16_t mask = 0xFFFF, next_mask = 0xFFFF;
static uint8_t cur_ch;
/* Home (cmd 0x36): the channels this dongle may hop on at all, default all 16. Two dongles side by side hop
   over the same channels in the same order, so whenever their cycles line up they collide for many cycles in
   a row; on disjoint homes (0x00FF / 0xFF00) they never share a channel. Devices follow it through the mask
   like any mask change. RAM only: the host sets it. */
static uint16_t home = 0xFFFF;
static uint8_t home_set;                   /* a new home, taken at the next cycle start */

/* per hop channel loss statistics (for adaptive hopping) */
static struct { uint16_t polls, lost; uint32_t off_until_ms; uint8_t loss_pct, strikes; } chs[DML_NHOP];
static uint32_t ch_eval_ms;

/* pairing */
static uint32_t pairing_until_ms;
static uint8_t pair_target = 0xFF;   /* slot the next pairing goes to (0xFF: first free) */
/* Pairing open for a device that isn't in pairing mode itself: one with no working buttons, or whose
   main firmware never tells the radio to pair (a Vive wand's "pair" over USB). Only into a chosen slot,
   only while pairing is open, and only a device this dongle doesn't know: ends with the first one. */
static uint8_t pair_any;
static uint8_t assign_id[8], assign_slot = 0xFF, assign_left;
/* The device paired last: its slot and until when its join beacons still say "pairing open". Pairing
   closes here the moment the device is stored, but the device stays in pairing mode until it joins, and
   a device in pairing mode only takes a beacon with pairing open: without this it ignored its own slot
   assignment until its pairing timed out (~35 s), shown here as paired with 100% loss. Ends when it
   answers a poll: a pairing started after that needs pairing open again. */
static uint8_t just_paired_slot = 0xFF;
static uint32_t just_paired_until_ms;
static uint32_t last_join_ms, joins_heard;

static uint8_t pkt_tx[2 + DML_MAXLEN], pkt_rx[2 + DML_MAXLEN];
static uint32_t period_us = 2000, slot_us = 1450;
#define PERIOD_MAX_US (255u * 50u)              /* what POLL p[6] / JOIN p[18] can say */
static sizing_t sizing;                    /* answer size by saturation (sizing.h) */
static int plan_n;                         /* devices in this cycle's plan */
static uint32_t sizing_ms;
static uint8_t max_resp = DML_MAXLEN;
static void (*idle_hook)(void);
/* Where USB is serviced inside a radio exchange (cmd 0x2C, for A/B tests): bit 0 while the POLL goes
   out, bit 1 while listening for the answer, bit 2 between decoded frames. Measured with six devices:
   0 -> ~500 reports/s per interface, 2 or 3 -> 1021/s (the USB limit). Bit 0 is off: while the packet
   goes out, the shortcuts turn the radio around to receive on their own, and USB work there can outlast
   the whole answer, which is then missed. Join requests got lost that way (Tracker 1.0s couldn't pair).
   Bit 2 delays the next POLL and loss went to ~18%, so it's off too. */
static uint8_t usb_in_radio = 2;
/* Exchange timing (POLL out, answer in): the 90th percentile of each 32 cycles sizes the slots; a longer
   exchange just starts the next POLL a little late (the slowest one alone kept every slot at the bound). */
#define EX_BUCKETS 50                         /* 50 us each, up to 2.5 ms */
static uint16_t ex_hist[EX_BUCKETS]; static uint32_t ex_hn;
#define SLOT_GUARD_US 120u
#define JOIN_TAIL_US 100u                                    /* a cycle without a join window: after its last slot */
/* Test switches (cmd 0x3B), off at power-on: the join window in every cycle (as before 0x002E), slots left
   out of the cycle (their devices drop out and rejoin when let back in), and answers dropped as if lost. */
static uint8_t test_join_always, test_drop_pct;
static uint16_t test_skip;
static uint32_t test_rng = 0x2545F491u;
static uint32_t cycle_len = 2000;
static uint8_t adaptive = 1, repoll = 0;   /* repoll: tried, ~8% of re-polls were answered (a miss repeats on the same channel) */
static uint32_t repolls, repolls_ok;
/* Ghost-follow (cmd 0x32, on by default). A device that missed the one POLL announcing a new mask (or period) keeps
   hopping on what it heard last (radio main.c: cycle = last + round(dt / P), dml_channel() with the old mask), and
   hears its dongle again only where the old and new channels happen to coincide: 0.1-1 s, or it rejoins. While a
   slot misses polls, its POLL goes where the device listens by that same rule, worked out from what the dongle sent
   in the device's last answered POLL (misses 1, 4, 7, ..), in the one before (2, 5, ..: a POLL received with a bad
   CRC is still answered but not used), and on the cycle's channel (3, 6, ..). Only within 1 s of the last answered
   POLL (the device rejoins after that). Where the device's rule gives the cycle's channel, nothing changes.
   Ghost polls stay out of the per-channel loss statistics. */
static uint8_t ghost = 1;
static uint32_t ghost_polls, ghost_ok;
static uint32_t ex_budget, ex_cycles, ex_sum, ex_n, ex_max;
void link_set_idle_hook(void (*f)(void)) { idle_hook = f; }
static int decode_some(void);
static uint8_t defer_decode = 1;           /* cmd 0x2E: 0 decodes each answer at once (A/B) */
static void wait_until(uint32_t t)
{
  while ((int32_t)(us_now() - t) < 0) {
    if (idle_hook) idle_hook();
    if ((int32_t)(us_now() - t) < -150) decode_some();     /* a frame takes up to ~100 us */
  }
}

static uint16_t upq_used(const slot_t *s) { return (uint16_t)((s->up_tail - s->up_head) & (UPQ - 1)); }
/* One frame into a slot's USB queue; 0 if dropped. The queue's last UPQ_REPLIES bytes are for replies
   (feature/config answers, identity): with the queue filling, they were dropped like IMU and light, and
   every config read over a busy link failed and was retried, which made it busier. A light or IMU frame
   can be lost; a reply can't. */
static int upq_push(slot_t *s, const uint8_t *f, int gl)
{
  const int reply = f[0] == 'P' || f[0] == 'H' || f[0] == DML_TAGGED_REPLY || f[0] == DML_AIR_UPD;   /* + radio update answers */
  if (upq_used(s) + gl + 1 >= (reply ? UPQ : UPQ - UPQ_REPLIES)) { s->drops++; return 0; }
  s->upq[s->up_tail] = (uint8_t)gl; s->up_tail = (s->up_tail + 1) & (UPQ - 1);
  for (int i = 0; i < gl; i++) { s->upq[s->up_tail] = f[i]; s->up_tail = (s->up_tail + 1) & (UPQ - 1); }
  if (upq_used(s) > s->upq_hw) s->upq_hw = upq_used(s);
  return 1;
}
/* The air codec is decoded here for every device: speed over size (~4700 frames/s at 10). Only the decoder:
   the rest of the file stays at -Os, or the image no longer fits the SoftDevice boot stub (ld/stub_sd.ld). */
#pragma GCC push_options
#pragma GCC optimize("O2")
/* Decodes one frame of a slot's kept answer (in order: the air codec's state follows every frame);
   0 when there's nothing left. */
static uint32_t dec_sum, dec_n, dec_max;
/* Latency counters (cmd 0x3A, cleared by each read), 250 us buckets, the last open-ended: dec, from an answer's
   end to its IMU sample decoded and queued for USB; usb, from that to the report carrying it being queued. */
static uint16_t h_dec[8], h_usb[8];
static void lat_hist(uint16_t *h, uint32_t us) { uint32_t b = us / 250u; h[b > 7 ? 7 : b]++; }
/* A decoded frame on its way to the host. */
static void deliver(slot_t *s, const uint8_t *f, int gl)
{
  if (f[0] == 'H' && gl <= 16) { memset(s->ident, 0, 16); memcpy(s->ident, f, gl); }
  if (f[0] == 0xF0 && f[1] == 'X' && gl >= 6) {            /* its radio's version: kept for when it's off */
    const int k = (int)(s - sl);
    const uint32_t v = (uint32_t)f[2] | (uint32_t)f[3] << 8 | (uint32_t)f[4] << 16 | (uint32_t)f[5] << 24;
    if (k >= 0 && k < DML_SLOTS && ((pairs.used >> k) & 1) && v && pairs.radio[k] != v) { pairs.radio[k] = v; pairs_dirty = 1; }
  }
  if (f[0] == 0xF0 && f[1] == '[' && gl >= 3 && f[2] == 0) {   /* a new radio committed: its version is unknown */
    const int k = (int)(s - sl);                            /* until it says (or the installer sets it, 0x39) */
    if (k >= 0 && k < DML_SLOTS && pairs.radio[k]) { pairs.radio[k] = 0; pairs_dirty = 1; }
  }
  if (f[0] == 0xF0 && f[1] == 'A' && gl >= 3) { s->late_state = f[2] ? 1 : 0; s->late_ask_ms = 0; }   /* late answers: its state */
  if (f[0] == 'D' && gl >= 5) { s->dev_drops = (uint16_t)(f[1] | f[2] << 8); s->dev_reply_drops = (uint16_t)(f[3] | f[4] << 8); }
  else {
    s->frames_up++; s->bytes_up += (uint32_t)gl;
    if (upq_push(s, f, gl) && f[0] == 'p' && gl == 18) { const uint32_t t = us_now(); lat_hist(h_dec, t - s->rx_t); s->imu_q_t = t; }
  }
}
/* Not what the radio coded: the coders are out of step for good (every frame after it decodes as
   garbage). Left alone past the radio's 1 s "dongle gone", it rejoins and both ends reset. */
static void codec_lost(slot_t *s)
{
  s->bad_frames++; s->resyncs++; s->pend_k = s->pend_n; s->in_batch = 0;
  s->skip_until = ms_ticks + 1200;
}
static int decode_step(slot_t *s)
{
  if (s->in_batch) {
    const uint32_t td = us_now();
    const int g = dmb_next(&s->br, &s->codec, &s->lh2p, s->bq[s->bn], s->check);
    if (g <= 0) { codec_lost(s); return 0; }
    s->bql[s->bn++] = (uint8_t)g;
    if (s->br.left == 0) {                                  /* whole, CRC passed: hand it on */
      for (int i = 0; i < s->bn; i++) deliver(s, s->bq[i], s->bql[i]);
      s->in_batch = 0;
    }
    const uint32_t dd = us_now() - td;
    dec_sum += dd; dec_n++; if (dd > dec_max) dec_max = dd;
    return 1;
  }
  if (s->pend_k >= s->pend_n) return 0;
  const uint32_t td = us_now();
  const uint8_t *r = s->pend; const int n = s->pend_n, k = s->pend_k;
  uint8_t fl = r[k];
  if (fl == 0 || k + 1 + fl > n) { s->bad_frames++; s->pend_k = s->pend_n; return 0; }
  const uint8_t *f = r + k + 1;
  s->pend_k = (uint8_t)(k + 1 + fl);
  /* Batches are agreed at join and both ends keep to it: with them on, only replies come outside a batch.
     Anything else means the radio and we disagree (it kept what it agreed with a dongle before ours): we
     skip it past its 1 s timeout and it rejoins, negotiating again. Without this it ran without batches,
     at 3x the airtime, until it happened to rejoin. */
  const int reply = f[0] == 'P' || f[0] == 'H' || f[0] == 'D' || f[0] == DML_TAGGED_REPLY;
  const int is_batch = f[0] == DMC_BATCH || f[0] == DMC_BATCH_W;
  if (!reply && is_batch != (s->batch != 0)) { s->batch_mismatch++; codec_lost(s); return 0; }
  if (is_batch && s->batch) {                     /* its frames come out a step at a time */
    if (!dmb_open(&s->br, f, fl)) { codec_lost(s); return 0; }
    s->in_batch = 1; s->bn = 0;
    return 1;
  }
  uint8_t g[DMB_MAXF]; int gl = fl;
  if (f[0] >= DMC_IMU && f[0] <= DMC_LH2) {
    gl = dmc_decode(&s->codec, f, fl, g); f = g;
    if (!gl) { codec_lost(s); return 0; }
  }
  else { dmc_observe(&s->codec, f, fl); if (s->batch) dmp_observe(&s->lh2p, f, fl); }
  deliver(s, f, gl);
  const uint32_t dd = us_now() - td;
  dec_sum += dd; dec_n++; if (dd > dec_max) dec_max = dd;
  return 1;
}
/* One frame of whichever slot has a kept answer; 0 if none. */
static int decode_some(void)
{
  for (int i = 0; i < DML_SLOTS; i++) if (decode_step(&sl[i])) return 1;
  return 0;
}
#pragma GCC pop_options

/* ---------------- front-end amplifier (Holyiot 21017: SKY66112, CTX P0.24 / CRX P0.22) ----------------
   Both pins are GPIOTE task outputs. Software raises CTX before TXEN and CRX before RXEN; the TX->RX
   turnaround (DISABLED_RXEN shortcut) is too fast for the CPU, so PPI swaps CTX->CRX on that DISABLED. */
#if BOARD_HOLYIOT
#define FEM_CTX_PIN 24
#define FEM_CRX_PIN 22
#define FEM_GT_CTX 6
#define FEM_GT_CRX 7
#define FEM_PPI 0
static void fem_init(void)
{
  NRF_GPIOTE->CONFIG[FEM_GT_CTX] = 3 | (FEM_CTX_PIN << 8);   /* task mode, starts low */
  NRF_GPIOTE->CONFIG[FEM_GT_CRX] = 3 | (FEM_CRX_PIN << 8);
  NRF_PPI->CH[FEM_PPI].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;
  NRF_PPI->CH[FEM_PPI].TEP = (uint32_t)&NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX];
  NRF_PPI->FORK[FEM_PPI].TEP = (uint32_t)&NRF_GPIOTE->TASKS_SET[FEM_GT_CRX];
  NRF_PPI->CHENCLR = 1u << FEM_PPI;
}
static void fem_tx(void)  { NRF_GPIOTE->TASKS_CLR[FEM_GT_CRX] = 1; NRF_GPIOTE->TASKS_SET[FEM_GT_CTX] = 1; }
static void fem_rx(void)  { NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX] = 1; NRF_GPIOTE->TASKS_SET[FEM_GT_CRX] = 1; }
static void fem_off(void) { NRF_PPI->CHENCLR = 1u << FEM_PPI;
                            NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX] = 1; NRF_GPIOTE->TASKS_CLR[FEM_GT_CRX] = 1; }
static void fem_turnaround_arm(void)    { NRF_PPI->CHENSET = 1u << FEM_PPI; }
static void fem_turnaround_disarm(void) { NRF_PPI->CHENCLR = 1u << FEM_PPI; }
/* SKY66112 adds ~22 dB: -4 dBm at the chip is ~+18 dBm out, under the 20 dBm EIRP limit */
#define RADIO_TXPOWER RADIO_TXPOWER_TXPOWER_Neg4dBm
#else
static void fem_init(void) {}
static void fem_tx(void) {}
static void fem_rx(void) {}
static void fem_off(void) {}
static void fem_turnaround_arm(void) {}
static void fem_turnaround_disarm(void) {}
#define RADIO_TXPOWER RADIO_TXPOWER_TXPOWER_Pos8dBm
#endif

/* ---------------- radio primitives ---------------- */
#define PPI_LAUNCH 1                   /* TIMER3 COMPARE[1] -> TXEN; fork: amplifier to TX (Holyiot) */
#define PPI_TURN   2                   /* RADIO DISABLED -> RXEN; fork: disables group 0 (fires once) */
#define PPI_ADDR   3                   /* RADIO ADDRESS -> TIMER3 CAPTURE[2] */
#define PPI_ENDCNT 6                   /* RADIO END -> TIMER2 COUNT */
#define PPI_TXRDY  7                   /* RADIO TXREADY -> TIMER3 CAPTURE[4] */
#define HX_GROUP   0                   /* PPI group: PPI_TURN (and the amplifier's turnaround, Holyiot) */
#define CC_LAUNCH  1
#define CC_ADDR    2
#define CC_TXRDY   4
static void radio_off(void)
{
  NRF_RADIO->INTENCLR = 0xFFFFFFFF;                         /* a repeater's receive interrupts */
  NRF_PPI->CHENCLR = 1u << PPI_LAUNCH;                      /* else it fires again when TIMER3 wraps */
  NRF_PPI->TASKS_CHG[HX_GROUP].DIS = 1;
  NRF_RADIO->SHORTS = 0;
  fem_turnaround_disarm();
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->TASKS_DISABLE = 1;
  uint32_t t = us_now(); while (!NRF_RADIO->EVENTS_DISABLED && us_now() - t < 200) {}
  NRF_RADIO->EVENTS_DISABLED = 0;
  fem_off();
}
/* send pkt_tx, then listen for one answer into pkt_rx. Returns answer length (payload) or -1/-2. */
static int txrx(uint8_t ch, uint32_t base, uint8_t prefix, uint32_t wait_us)
{
  NRF_RADIO->FREQUENCY = ch;
  NRF_RADIO->BASE0 = base; NRF_RADIO->PREFIX0 = prefix;
  NRF_RADIO->TXADDRESS = 0; NRF_RADIO->RXADDRESSES = 1;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_tx;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_DISABLED_RXEN_Msk;
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->EVENTS_ADDRESS = 0;
  fem_tx(); fem_turnaround_arm();                           /* PPI flips to RX on the turnaround */
  NRF_RADIO->TASKS_TXEN = 1;
  uint32_t t0 = us_now();
  while (!NRF_RADIO->EVENTS_ADDRESS) if (us_now() - t0 > 1500) { radio_off(); return -2; }
  NRF_RADIO->EVENTS_ADDRESS = 0;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_rx;                  /* latched at the RX START */
  /* From here the radio runs on its own (shortcuts): USB is serviced while it sends and listens. The
     exchange took up to ~2.5 ms per slot with USB untouched, and with six devices the interfaces got
     one report out per 2 ms instead of per 1 ms (measured: ~470/s with the link on, 1021/s off). */
  while (!NRF_RADIO->EVENTS_DISABLED) {
    if (us_now() - t0 > 2500) { radio_off(); return -2; }
    if (idle_hook && (usb_in_radio & 1)) idle_hook();
  }
  NRF_RADIO->EVENTS_DISABLED = 0;                           /* TX done, RXEN fired by the shortcut */
  fem_turnaround_disarm();                                  /* the RX end must not re-trigger it */
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  uint32_t t1 = us_now();
  for (;;) {
    if (NRF_RADIO->EVENTS_DISABLED) break;
    uint32_t dt = us_now() - t1;
    if (NRF_RADIO->EVENTS_ADDRESS ? dt > 1600 : dt > wait_us) { radio_off(); return -1; }
    if (usb_in_radio & 2) { if (idle_hook) idle_hook(); decode_some(); }   /* the radio receives on its own */
  }
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->SHORTS = 0;
  fem_off();
  if (!NRF_RADIO->CRCSTATUS) return -3;
  return pkt_rx[0];
}

/* ---------------- one poll ---------------- */
/* Consecutive out-of-order answers after which a slot is left alone so its radio rejoins. At the
   poll rate (a few hundred a second per slot) this is a few hundred ms of a link that is receiving
   answers but cannot advance, not a loss burst. */
#define DUP_RUN_LIMIT 64
static int slot_skipping(const slot_t *s) { return s->skip_until && (int32_t)(ms_ticks - s->skip_until) < 0; }
/* Its radio gives up after 1 s without a POLL and is usually back 1.0-1.2 s after the skip starts, sometimes
   later (1.3 s seen); slot_reset ends the skip then. Polled again before that, the slot goes inactive and the
   host drops the device. */
static void slot_skip(slot_t *s) { s->skip_until = ms_ticks + 1500; if (!s->skip_until) s->skip_until = 1; }
static int hop_of(uint8_t ch) { for (int k = 0; k < DML_NHOP; k++) if (dml_hop_ch[k] == ch) return k; return -1; }

/* Builds slot i's POLL in b (b[0]: length). Its downlink is picked only when nothing is in flight: whole
   frames from down_buf, at most down_max bytes. Until the device acks them, every POLL resends the same bytes,
   and frames queued meanwhile wait for a later POLL. Returns ch's hop index, or -1 for a ghost-follow channel. */
static int poll_build(slot_t *s, uint8_t *b, uint8_t i, uint8_t ch, int down_max)
{
  uint8_t *p = b + 1;
  int len = DML_POLL_HDR;
  p[0] = DML_POLL; p[1] = i;
  p[2] = (uint8_t)((s->last_seq & DML_F_SEQ_MASK) | (s->dseq ? DML_F_DSEQ : 0) | (s->held ? DML_F_SACK : 0));
  p[3] = cycle; p[4] = (uint8_t)next_mask; p[5] = (uint8_t)(next_mask >> 8);
  p[6] = (uint8_t)(period_us >= PERIOD_MAX_US ? 255 : period_us / 50); p[7] = max_resp;
  if (!s->down_sent)
    while (s->down_sent < s->down_len && s->down_sent + 1 + s->down_buf[s->down_sent] <= down_max)
      s->down_sent = (uint8_t)(s->down_sent + 1 + s->down_buf[s->down_sent]);
  if (s->down_sent) { p[2] |= DML_F_DOWN; memcpy(p + len, s->down_buf, s->down_sent); len += s->down_sent; }
  b[0] = (uint8_t)len;
  s->polls++;
  const int hop = ch == cur_ch ? hop_of(ch) : -1;           /* (cur_ch is always a hop channel: -1 only for a ghost) */
  if (hop >= 0) chs[hop].polls++;
  else ghost_polls++;                                       /* ghost polls stay out of the channel statistics */
  return hop;
}
static void answer_take(slot_t *s, uint8_t i, const uint8_t *r, int n, uint8_t rd, int now);
static void answer_state(slot_t *s, uint8_t i, const uint8_t *r, uint8_t rd);
/* The end of slot i's exchange: n bytes received in b (or the receive error; -3 is a CRC error), hop from
   poll_build. Counts a POLL that got no valid answer, or takes the answer. 1: it answered. */
static int poll_end(slot_t *s, uint8_t i, const uint8_t *b, int n, int hop)
{
  const uint8_t *r = b + 1;
  if (n < DML_RESP_HDR || r[0] != DML_RESP || r[1] != i) {
    if (n == -3) s->crc_err++; else s->timeouts++;
    if (hop >= 0) chs[hop].lost++;
    if (s->active && ms_ticks - s->last_resp_ms > 1000) s->active = 0;
    if (s->g_miss < 0xFFFF) s->g_miss++;
    return 0;
  }
  if (hop < 0) ghost_ok++;
  answer_take(s, i, r, n, (uint8_t)NRF_RADIO->RSSISAMPLE, 1);
  return 1;
}
static int poll_slot(slot_t *s, uint8_t i, uint8_t ch, uint32_t t_tx)   /* 1: it answered; ch: cur_ch, or a ghost-follow channel */
{
  while (decode_step(s)) {}                                 /* its last answer first: the ack is for it */
  const int hop = poll_build(s, pkt_tx, i, ch, 255);
  s->poll_t = t_tx;
  int n = txrx(ch, net_base, (uint8_t)(DML_SLOT_PREFIX + i), 400);
  if (test_drop_pct && n > 0) {                             /* test: lost as if in the air (the device resends) */
    test_rng = test_rng * 1664525u + 1013904223u;
    if ((test_rng >> 16) % 100u < test_drop_pct) n = -2;
  }
  return poll_end(s, i, pkt_rx, n, hop);
}
/* Takes a valid answer (type and slot already checked) from slot i, heard directly or relayed by a repeater.
   rd: its signal here in -dBm, 0 if relayed. now: it answers this cycle's POLL (a repeater can also relay one
   from the cycle before, which only fills the gap in the sequence: the device's hop timing, telemetry and
   downlink ack are newer than it). */
static void answer_take(slot_t *s, uint8_t i, const uint8_t *r, int n, uint8_t rd, int now)
{
  if (now) answer_state(s, i, r, rd);
  uint8_t seq = r[2] & DML_F_SEQ_MASK;
  if (seq != ((s->last_seq + 1) & DML_F_SEQ_MASK)) {                          /* duplicate / out of order */
    if (!s->held && seq == ((s->last_seq + 2) & DML_F_SEQ_MASK) && n - DML_RESP_HDR <= DML_MAXLEN) {   /* the one after a lost one */
      s->held = 1; s->held_seq = seq; s->held_n = (uint8_t)(n - DML_RESP_HDR); s->held_set++;
      memcpy(s->held_buf, r + DML_RESP_HDR, s->held_n);
      s->dup_run = 0;
      return;
    }
    s->dup++;
    /* Go-back gives one or two of these in a row. A long run means the radio is repeating a seq we
       already accepted and cannot hear which one we want - a POLL whose ack bits were corrupted but
       whose CRC passed, and whose answer was lost (issue #130). Nothing we send can break that, so
       leave the slot alone for longer than the radio's 1 s "dongle gone" timeout: it rejoins, which
       resets the sequence on both sides. DUP_RUN_LIMIT is well above any loss burst: a lost answer
       is a timeout, not a dup. */
    if (++s->dup_run >= DUP_RUN_LIMIT) { s->dup_run = 0; slot_skip(s); }
    return;
  }
  s->dup_run = 0;
  s->last_seq = seq;
  s->air_bytes += (uint32_t)(n - DML_RESP_HDR);
  sizing_answer(&sizing, i, n, r[2], s->check, plan_n);   /* radios with the CRC-8 set the "more" flag too */
  /* Kept and decoded while the radio is busy with the next exchange (decode_some): decoding here took
     ~320 us of each ~870 us exchange (up to 720) with the radio idle, every slot waiting on it. */
  s->rx_t = us_now();
  memcpy(s->pend, r + DML_RESP_HDR, (size_t)(n - DML_RESP_HDR));
  s->pend_n = (uint8_t)(n - DML_RESP_HDR); s->pend_k = 0;
  if (s->held) {
    if (s->held_seq == ((seq + 1) & DML_F_SEQ_MASK)) {      /* the lost one came: it, then the kept one, in order */
      while (decode_step(s)) {}
      s->last_seq = s->held_seq; s->air_bytes += s->held_n; s->held_used++;
      memcpy(s->pend, s->held_buf, s->held_n);
      s->pend_n = s->held_n; s->pend_k = 0;
    }
    s->held = 0;                                           /* (otherwise stale: the radio went back further) */
  }
  if (!defer_decode) while (decode_step(s)) {}
}
/* What an answer to this cycle's POLL says about the device: it's there, its hop timing, signal, telemetry,
   and whether it has the downlink in flight. */
static void answer_state(slot_t *s, uint8_t i, const uint8_t *r, uint8_t rd)
{
  s->resp_ok++;
  s->g_t[1] = s->g_t[0]; s->g_cyc[1] = s->g_cyc[0]; s->g_mask[1] = s->g_mask[0];
  s->g_per[1] = s->g_have ? s->g_per[0] : 0;               /* 0: no earlier record */
  s->g_t[0] = s->poll_t; s->g_cyc[0] = cycle; s->g_mask[0] = next_mask;
  s->g_per[0] = (uint16_t)(period_us / 50 * 50); s->g_have = 1;   /* what the device keeps of p[6] */
  s->g_miss = 0;
  s->active = 1; s->last_resp_ms = ms_ticks;
  if (i == just_paired_slot) just_paired_slot = 0xFF;     /* it joined: its pairing is over, the next one needs pairing open */
  s->rssi_last_t = r[3];
  if (rd) {
    s->rssi_last_d = rd;
    if (!s->rssi_min_d || rd < s->rssi_min_d) s->rssi_min_d = rd;   /* strongest (smallest -dBm) */
    if (rd > s->rssi_max_d) s->rssi_max_d = rd;                      /* weakest */
    s->rssi_d16 = s->rssi_d16 ? s->rssi_d16 - s->rssi_d16 / 16 + rd : (uint32_t)rd * 16;
  }
  s->rssi_t16 = s->rssi_t16 ? s->rssi_t16 - s->rssi_t16 / 16 + r[3] : (uint32_t)r[3] * 16;
  if (s->have_tel) { s->t_missed += (uint8_t)(r[4] - s->last_missed); s->t_crc += (uint8_t)(r[5] - s->last_crc); }
  s->last_missed = r[4]; s->last_crc = r[5]; s->have_tel = 1;
  uint8_t dack = (r[2] & DML_F_DSEQ) ? 1 : 0;
  if (s->down_sent) {
    if (dack == s->dseq) {                                  /* acked: drop the sent bytes, keep what was queued since */
      const uint8_t *from = s->down_buf + s->down_sent;
      const int left = s->down_len - s->down_sent;
      for (int k = 0; k < left; k++) s->down_buf[k] = from[k];   /* (memmove would add 52 B of libc) */
      s->down_len = (uint8_t)left; s->down_sent = 0; s->dseq ^= 1;
    } else s->down_resends++;
  }
}

/* ---------------- timing mode 3 (cmd 0x2D): compare-started POLLs ----------------
   The same schedule as mode 1, but the hardware starts each POLL: TIMER3 COMPARE[1] triggers TXEN through
   PPI, and a one-shot PPI link (DISABLED -> RXEN) switches the radio to receive the answer into the same
   buffer. The CPU has no deadline inside an exchange, so it can serve USB while one is on the air. */
static GRP_D uint8_t tmode = 3, tmode_req = 3;
#define HX_RAMP_US       40u           /* radio ramp-up, TXEN to TXREADY (fast mode) */
#define HX_ARM_MIN_US    10            /* a start closer than this is done by software */
#define HX_DOWN_MAX      64            /* max downlink bytes per POLL (one air-update block) */
#define HX_PREP_US       30u           /* after an exchange: take the answer, set up the next */
static uint8_t hx_buf[2 + DML_MAXLEN];
static GRP_D uint32_t hx_usb_cost = 100;    /* recent worst idle_hook() (USB) pass, us; decays */
static GRP_M uint32_t ex_t_tx;              /* TXREADY time of the last exchange (start + ramp-up) */

/* Prepares one exchange (address and channel already set): send pkt, then receive the answer into it. */
static void hx_setup(const uint8_t *pkt)
{
  NRF_RADIO->PACKETPTR = (uint32_t)pkt;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  /* the events hx_txrx reads (DISABLED is cleared where an exchange ends; a main never reads END or READY) */
  NRF_RADIO->EVENTS_ADDRESS = 0; NRF_RADIO->EVENTS_TXREADY = 0; NRF_RADIO->EVENTS_RXREADY = 0;
  NRF_RADIO->EVENTS_CRCOK = 0; NRF_RADIO->EVENTS_CRCERROR = 0;
  fem_off();
  NRF_PPI->TASKS_CHG[HX_GROUP].EN = 1;                     /* arm the turnaround: DISABLED -> RXEN, once */
  NRF_TIMER3->EVENTS_COMPARE[CC_LAUNCH] = 0;
}
/* Starts the prepared exchange at target through the compare, or from software right away when it isn't
   timed or target is too close. Call with interrupts off. Returns the start time. */
static uint32_t hx_launch(uint32_t target, int timed)
{
  if (timed && (int32_t)(target - us_time()) >= HX_ARM_MIN_US) {
    NRF_TIMER3->CC[CC_LAUNCH] = target;
    NRF_PPI->CHENSET = 1u << PPI_LAUNCH;
    if ((int32_t)(target - us_time()) > 0) return target;   /* still ahead of the timer: the compare will fire */
  }
  NRF_PPI->CHENCLR = 1u << PPI_LAUNCH;
  const uint32_t tstart = us_time();
  if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled && !NRF_TIMER3->EVENTS_COMPARE[CC_LAUNCH]) {
    fem_tx(); NRF_RADIO->TASKS_TXEN = 1;                   /* the compare didn't fire: start now */
  }
  return tstart;
}
/* Serves USB and decodes frames while an exchange is on the air, if there's time before the next start
   (next_t; 0: no next start). s: the slot whose frames are decoded first. */
static void hx_service(uint32_t now, uint32_t next_t, slot_t *s)
{
  const int32_t left = next_t ? (int32_t)(next_t - now) : 0x7FFFFFFF;
  if (idle_hook && left > (int32_t)(hx_usb_cost + HX_PREP_US)) {
    idle_hook();
    const uint32_t d = us_time() - now;
    if (d > hx_usb_cost) hx_usb_cost = d > 200 ? 200 : d;   /* longer: a flash write or enumeration */
  }
  /* a frame takes up to ~100 us */
  if ((int32_t)(next_t ? next_t - us_time() : 0x7FFFFFFF) > (int32_t)(100u + HX_PREP_US))
    if (!(s && decode_step(s))) decode_some();
}
/* Sends hx_buf at target, then receives one answer into hx_buf, serving USB while it waits. Returns the
   answer's length, -1 for no answer, -3 for a CRC error. */
static int hx_txrx(uint8_t ch, uint32_t base, uint8_t prefix, uint32_t target, int timed, uint32_t next_t,
                   uint32_t wait_us, slot_t *s)
{
  const uint8_t plen = hx_buf[0];
  NRF_RADIO->FREQUENCY = ch;
  NRF_RADIO->BASE0 = base; NRF_RADIO->PREFIX0 = prefix;
  NRF_RADIO->TXADDRESS = 0; NRF_RADIO->RXADDRESSES = 1;
  hx_setup(hx_buf);
  __disable_irq();
  const uint32_t tstart = hx_launch(target, timed);
  __enable_irq();
  uint32_t t_rx = 0;
  int r, rx = 0;
  for (;;) {
    /* received; the END -> DISABLE short stops the radio */
    if (NRF_RADIO->EVENTS_CRCOK || NRF_RADIO->EVENTS_CRCERROR) {
      const uint32_t t = us_time();
      while (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled && us_time() - t < 20) {}
      r = NRF_RADIO->EVENTS_CRCOK ? hx_buf[0] : -3;
      break;
    }
    const uint32_t now2 = us_time();
    /* The answer has begun: an ADDRESS after the receiver came up. (The POLL's own ADDRESS is at start + 64 us,
       so one after start + 150 us is the answer's.) */
    if (!rx && NRF_RADIO->EVENTS_RXREADY && NRF_RADIO->EVENTS_ADDRESS &&
        (int32_t)(NRF_TIMER3->CC[CC_ADDR] - (tstart + 150u)) > 0) { rx = 1; t_rx = NRF_TIMER3->CC[CC_ADDR]; }
    if (rx ? (int32_t)(now2 - t_rx) > 1600 : (int32_t)(now2 - (tstart + HX_RAMP_US + 4u * (9u + plen) + 4u + wait_us)) > 0) {
      radio_off(); r = -1; break;
    }
    hx_service(now2, next_t, s);
  }
  NRF_PPI->CHENCLR = 1u << PPI_LAUNCH;                      /* disarm after every exchange */
  NRF_PPI->TASKS_CHG[HX_GROUP].DIS = 1;
  NRF_RADIO->SHORTS = 0; NRF_RADIO->EVENTS_DISABLED = 0;
  fem_off();
  ex_t_tx = NRF_RADIO->EVENTS_TXREADY ? NRF_TIMER3->CC[CC_TXRDY] : tstart + HX_RAMP_US;
  return r;
}
/* poll_slot() for timing mode 3. Differences: at most HX_DOWN_MAX downlink bytes per POLL, and the slot's
   previous answer is decoded while the exchange is on the air instead of before it. */
static int poll_slot_hw(slot_t *s, uint8_t i, uint8_t ch, uint32_t target, int timed, uint32_t next_t)
{
  const int hop = poll_build(s, hx_buf, i, ch, HX_DOWN_MAX);
  int n = hx_txrx(ch, net_base, (uint8_t)(DML_SLOT_PREFIX + i), target, timed, next_t, 400, s);
  s->poll_t = ex_t_tx - HX_RAMP_US;
  while (decode_step(s)) {}                                 /* its previous answer first (usually done by now) */
  return poll_end(s, i, hx_buf, n, hop);
}
/* One POLL in the timing mode in use: poll_slot_hw(), or poll_slot() at once (t: the time now). Not inlined:
   it has two callers. */
static __attribute__((noinline)) int poll_one(uint8_t i, uint8_t ch, uint32_t t, int timed, uint32_t next_t)
{
  slot_t *s = &sl[i]; KEEP_PTR(s);
  return tmode == 3 ? poll_slot_hw(s, i, ch, t, timed, next_t) : poll_slot(s, i, ch, t);
}

/* ---------------- test load (0x2B) ---------------- */
/* Filler 'Z' frames queued for USB as if devices sent them, at load_kbps kB/s on each slot in load_mask
   (empty slots too): the dongle's queues, USB and the host's reader carry them like data (the driver
   skips them), so a busy eight-device dongle can be tried with a few real devices. Off by default. */
static uint8_t load_mask, load_kbps;
static uint32_t load_at;
static void load_tick(void)
{
  const uint32_t now = ms_ticks, dt = now - load_at;
  load_at = now;
  if (!load_mask || !load_kbps || dt > 100) return;
  static uint8_t z[40] = { 'Z' };
  for (int i = 0; i < DML_SLOTS; i++) {
    if (!((load_mask >> i) & 1)) continue;
    slot_t *s = &sl[i];
    s->load_budget += (uint32_t)load_kbps * dt;                 /* kB/s x ms = bytes */
    while (s->load_budget >= sizeof z) {
      s->load_budget -= sizeof z;
      z[1]++;
      if (upq_push(s, z, sizeof z)) s->load_bytes += sizeof z;
    }
  }
}

/* ---------------- joining ---------------- */
/* Late answers (Index-family radios 0x6A000029+, air 'A'): the radio builds its answer just before the POLL it
   expects instead of right after the last one, so the answer carries newer data, by up to a cycle. A POLL that
   comes earlier than it expected gets no answer (a lost exchange). Measured with 6 devices: at ~5 ms cycles they
   cost clean devices 1-1.5 points of loss and saved no delay; at ~9 ms about even, the worst delays a little
   better. So the dongle turns them on only while cycles are long, and off otherwise (radios boot with them on).
   HTC radios keep theirs off (it cost them 4-5 points). */
#define LATE_ON_US     8000u           /* on from this cycle length */
#define LATE_OFF_US    7000u           /* off below this one */
#define LATE_HOLD_MS   2000u           /* past a threshold this long before switching */
#define LATE_RETRY_MS  500u            /* no reply from the radio: sent again */
static uint32_t plan;                        /* (the cycle's planned slots, below) */
static uint8_t late_want;                    /* late answers on (1) or off (0) for every Index-family radio */
static uint32_t late_cross_ms;               /* when the cycle went past the threshold to switch (0: it hasn't) */
static int down_push(slot_t *s, const uint8_t *f, uint8_t n)   /* a downlink frame of the dongle's own */
{
  if (s->down_len + 1u + n > sizeof s->down_buf) return 0;
  s->down_buf[s->down_len++] = n; memcpy(s->down_buf + s->down_len, f, n); s->down_len += n;
  return 1;
}
/* Once a cycle: decides from the cycle's length (forbid: off regardless) and tells the planned slots' radios. */
static void late_tick(uint32_t period, int forbid)
{
  const int want = forbid ? 0 : late_want ? period >= LATE_OFF_US : period >= LATE_ON_US;
  if (want == late_want) late_cross_ms = 0;
  else if (forbid) { late_want = 0; late_cross_ms = 0; }
  else if (!late_cross_ms) late_cross_ms = ms_ticks | 1u;
  else if (ms_ticks - late_cross_ms >= LATE_HOLD_MS) { late_want = (uint8_t)want; late_cross_ms = 0; }
  for (uint8_t k = 0; k < DML_SLOTS; k++) {
    slot_t *s = &sl[k];
    if (!((plan >> k) & 1) || !s->active || slot_skipping(s)) continue;
    const uint32_t v = pairs.radio[k];
    if (!v) {                                               /* not known (no driver asked): ask it once */
      static const uint8_t ask[2] = { DML_AIR_UPD, 'X' };
      if (!s->ver_asked && down_push(s, ask, 2)) s->ver_asked = 1;
      continue;
    }
    if (v >> 24 != 0x6Au || v < 0x6A000029u || s->late_state == late_want) continue;
    if (s->late_ask_ms && ms_ticks - s->late_ask_ms < LATE_RETRY_MS) continue;
    const uint8_t a[3] = { DML_AIR_UPD, 'A', late_want };
    if (down_push(s, a, 3)) s->late_ask_ms = ms_ticks | 1u;
  }
}

static void slot_reset(uint8_t i)
{
  slot_t *s = &sl[i];
  s->last_seq = 7; s->dseq = 0; s->down_len = 0; s->have_tel = 0;
  s->dup_run = 0; s->skip_until = 0;
  dmc_reset(&s->codec);
  s->codec.check = s->check;
  dmp_reset(&s->lh2p);
  s->in_batch = 0;
  memset(s->ident, 0, sizeof s->ident);
  s->up_head = s->up_tail; s->frag_left = 0; s->pk_rest = 0; s->down_sent = 0;
  s->pend_n = s->pend_k = 0;                               /* the old device's answer isn't for the new codec */
  s->held = 0;
  s->late_state = 0xFF; s->ver_asked = 0; s->late_ask_ms = 0;   /* told again: a radio that rebooted has them on */
  s->g_have = 0; s->g_miss = 0; s->g_per[1] = 0;           /* joined afresh from a beacon: no old hop state */
  s->active = 1; s->last_resp_ms = ms_ticks;               /* poll it right away */
}
static uint32_t t_c;                                       /* current cycle start */
/* The slots polled this cycle: paired and connected. A paired device that's off costs no airtime;
   when it comes back it rejoins through the join beacons (its slot_reset puts it back in). */
static uint32_t plan;
static GRP_M uint16_t ru_worst_pm;   /* the worst device loss as the main alone, per mille (~2 s, ru_tick) */
static uint8_t sat_on_b, ru_on_b;    /* for the beacon: repeaters in use, relaying now */
/* Every dongle is a normal one (a main) until it hears a main with devices connected while it has none of its
   own: then it's a repeater on its own (auto_rep, RAM only: it starts as a dongle again after a reset).
   Back to a dongle after AUTO_ALONE_MS without hearing such a main, when pairing starts, or when one of its own
   devices answers the short JOIN beacon it keeps sending (every AUTO_OWN_JOIN_MS). Two dongles without devices
   both stay dongles: a main without devices doesn't count. A repeater set by hand (0x3F) stays one. */
#define AUTO_ALONE_MS     15000u
#define AUTO_LISTEN_EVERY 1000u      /* a main without devices listens for others this often... */
#define AUTO_LISTEN_US    22000u     /* ...this long: beacons are 20 ms apart */
#define AUTO_OWN_JOIN_MS  100u
static GRP_M uint8_t auto_rep;
static GRP_M uint32_t auto_listen_ms, auto_busy_ms, auto_own_ms;
static void join_beacon(void)
{
  uint8_t *p = pkt_tx + 1;
  p[0] = DML_JOIN; memcpy(p + 1, &net_base, 4);
  p[5] = ms_ticks < pairing_until_ms ||
         (assign_left && assign_slot == just_paired_slot && (int32_t)(just_paired_until_ms - ms_ticks) > 0) ? 1 : 0;
  if (assign_left) {
    memcpy(p + 6, assign_id, 8); p[14] = assign_slot; assign_left--;
    if (sl[assign_slot].check) p[5] |= DML_JF_CHECK;        /* only for a radio that asked: an old one reads p[5] as "pairing open" */
    if (sl[assign_slot].batch) p[5] |= DML_JF_BATCH;
    if (sl[assign_slot].batch == 2) p[5] |= DML_JF_BATCH32;
  }
  else { memset(p + 6, 0, 8); p[14] = 0xFF; }
  p[15] = (uint8_t)next_mask; p[16] = (uint8_t)(next_mask >> 8); p[17] = cycle; p[18] = (uint8_t)(period_us >= PERIOD_MAX_US ? 255 : period_us / 50);
  uint32_t since = 0;
  if (p[14] < DML_SLOTS) {                                  /* the assigned slot's place in this cycle */
    int k = 0;
    const uint32_t in = plan | 1u << p[14];               /* it's in the next cycle's plan */
    for (int i = 0; i < p[14]; i++) k += (in >> i) & 1;
    since = us_now() - (t_c + (uint32_t)k * slot_us);
    if (since > 0xFFFF) since = 0xFFFF;
  }
  p[19] = (uint8_t)since; p[20] = (uint8_t)(since >> 8);
  p[21] = (uint8_t)(ru_worst_pm >= 1000 ? 100 : ru_worst_pm / 10);
  p[22] = (uint8_t)((sat_on_b & 3) | (ru_on_b ? 0x80 : 0) | (plan ? 0x40 : 0));   /* 0x40: devices connected */
  p[23] = 0; memset(p + 24, 0, 4);
  for (int t = 0; t < 2; t++) {                             /* the auto repeaters in turn */
    sat_annc ^= 1;
    if ((sat_auto >> sat_annc) & 1) { p[23] = (uint8_t)(0x80 | sat_annc); memcpy(p + 24, &sat_rbase[sat_annc], 4); break; }
  }
  pkt_tx[0] = DML_JOIN_LEN_X;
  int n = txrx(DML_JOIN_CHANNEL, DML_JOIN_BASE, DML_JOIN_PREFIX, 400);
  const uint8_t *r = pkt_rx + 1;
  if (n >= DML_SREQ_LEN && r[0] == DML_SREQ) {               /* a repeater asking for (or keeping) a place */
    uint32_t rb, mb; memcpy(&rb, r + 1, 4); memcpy(&mb, r + 5, 4);
    if (mb != net_base) return;
    int id = -1;
    for (int k = 0; k < 2; k++) if ((sat_auto >> k) & 1 && sat_rbase[k] == rb) id = k;
    if (id < 0) for (int k = 0; k < 2 && id < 0; k++) if (!((sat_on >> k) & 1)) id = k;
    if (id < 0) return;                                     /* full: it looks elsewhere */
    if (!((sat_auto >> id) & 1)) {
      memset(&sats[id], 0, sizeof sats[id]); memset(sat_info[id], 0, sizeof sat_info[id]);
      sat_rbase[id] = rb; sat_auto |= (uint8_t)(1u << id); sat_on |= (uint8_t)(1u << id); sat_rssi16[id] = 0;
    }
    sat_seen_ms[id] = ms_ticks; sat_rssi_add(id, (uint8_t)NRF_RADIO->RSSISAMPLE);
    if (n >= DML_SREQ_LEN_X) memcpy(sat_info[id], r + DML_SREQ_LEN, sizeof sat_info[id]);
    return;
  }
  if (n < DML_JREQ_LEN || r[0] != DML_JREQ) return;
  joins_heard++;
  const uint8_t *id = r + 1;
  int pairing = r[9] & DML_JF_PAIR;                         /* the device is in pairing mode */
  int slot = -1;
  for (int i = 0; i < DML_SLOTS; i++) if ((pairs.used >> i) & 1 && memcmp(pairs.id[i], id, 8) == 0) slot = i;
  if (!pairing && pair_any && slot < 0 && pair_target < DML_SLOTS && ms_ticks < pairing_until_ms) {
    pairing = 1; pair_any = 0;
  }
  /* Like stock, a new pairing takes both sides: pairing open here, and the device in pairing mode
     (its pairing buttons, or a host's pair command). Known devices just reconnect. */
  if (pairing && ms_ticks < pairing_until_ms) {
    /* paired here: a new device, one already paired here (re-paired), or one moving to the chosen slot */
    int to = slot;
    if (pair_target < DML_SLOTS) to = pair_target;          /* the chosen slot: replaces what was there */
    else if (to < 0) for (int i = 0; i < DML_SLOTS && to < 0; i++) if (!((pairs.used >> i) & 1)) to = i;
    if (to < 0) return;                                     /* no free slot */
    if (to != slot) {
      if (slot >= 0) pairs.used &= ~(1u << slot);
      slot = to;
      memcpy(pairs.id[slot], id, 8); pairs.used |= 1u << slot;
      memset(pairs.name[slot], 0, sizeof pairs.name[slot]);   /* another device: not its name */
      pairs.radio[slot] = 0;                                    /* nor its radio */
      pairs_save();
    }
    pairing_until_ms = 0;
    just_paired_slot = (uint8_t)slot; just_paired_until_ms = ms_ticks + 35000u;
  } else if (pairing && !(slot >= 0 && slot == just_paired_slot && (int32_t)(just_paired_until_ms - ms_ticks) > 0)) {
    /* in pairing mode but not pairing here: it only joins a beacon with pairing open, so assigning it
       would show it connected at 100% loss until its pairing times out. Left alone until then. */
    return;
  }
  if (slot < 0) return;
  memcpy(assign_id, id, 8); assign_slot = (uint8_t)slot; assign_left = 5;
  sl[slot].check = (r[9] & DML_JF_CHECK) ? 1 : 0;
  sl[slot].batch = (r[9] & DML_JF_BATCH) ? ((r[9] & DML_JF_BATCH32) ? 2 : 1) : 0;   /* 2: wide batches */
  slot_reset((uint8_t)slot);
}

/* ---------------- adaptive hopping ---------------- */
/* Every 2 s: drop the WORST channel over the threshold (repeatedly, down to CH_MIN_ON), bring back at
 * most one channel whose time off is over, and keep repeat offenders off longer (30 s doubling to 5 min;
 * a channel that stays clean for a while is forgiven). Dropping worst-first matters when most of the band
 * is busy (Wi-Fi): dropping in frequency order kept the worst ones once the minimum was reached. */
#define CH_MIN_ON   8
#define CH_BAD_PCT  8
#define CH_EVERYWHERE_PCT 20
/* Inside a home (not all 16; at least HOME_MIN_SIZE channels, cmd 0x36) the floor of 8 would hold every channel
   of an 8-channel home, Wi-Fi hit or not. There, HOME_KEEP channels stay on, and a channel losing clearly more than
   the rest (bad, and at least twice the typical loss) may take it down to HOME_MIN. While the typical loss is
   under 8 %, every bad channel is also clearly bad, so the floor is then HOME_MIN. All 16: CH_MIN_ON as before. */
#define HOME_KEEP   4
#define HOME_MIN    3
/* The smallest home cmd 0x36 takes: 5, so three dongles split the 16 channels (5 + 5 + 6) and each still
   has one spare over the 4 it keeps on. 8 before 0x0030. */
#define HOME_MIN_SIZE 5
static void home_floors(int *keep, int *least)
{
  if (home == 0xFFFF) { *keep = *least = CH_MIN_ON; return; }
  *keep = HOME_KEEP; *least = HOME_MIN;
}
/* A channel is bad when it loses clearly more than the channels around it. Loss a device has on
   every channel (weak signal, body in the way) is not the channel's fault: judged on absolute loss,
   one weak device struck clean channels until only a few were left to hop on.
   low: the lower median. In a home, with 3-8 channels on, the upper one is often a bad channel itself
   (8 on, 4 of them bad: nothing would ever drop). */
static uint8_t channels_typical_loss(uint16_t m, int low)
{
  uint8_t v[DML_NHOP]; int n = 0;
  for (int k = 0; k < DML_NHOP; k++)
    if ((m >> k) & 1 && chs[k].polls >= 20) {
      uint8_t x = chs[k].loss_pct; int i = n++;
      while (i > 0 && v[i - 1] > x) { v[i] = v[i - 1]; i--; }
      v[i] = x;
    }
  return n ? v[low ? (n - 1) / 2 : n / 2] : 0;
}
static void channels_eval(void)
{
  if (ms_ticks - ch_eval_ms < 2000) return;
  ch_eval_ms = ms_ticks;
  uint16_t m = mask & home;
  if (!m) m = home;
  const int in_home = home != 0xFFFF;
  int on = 0, keep, least, home_first = 0;
  home_floors(&keep, &least);
  for (int k = DML_NHOP - 1; k >= 0; k--) if ((home >> k) & 1) home_first = k;
  for (int k = 0; k < DML_NHOP; k++) {
    chs[k].loss_pct = chs[k].polls ? (uint8_t)(100u * chs[k].lost / chs[k].polls) : 0;
    if ((m >> k) & 1) {
      on++;
      if (chs[k].polls >= 20 && chs[k].loss_pct <= 2 + channels_typical_loss(m, in_home) && chs[k].strikes && ms_ticks - chs[k].off_until_ms > 120000)
        chs[k].strikes--;                                    /* clean for 2 min since it came back */
    }
  }
  const unsigned typical = channels_typical_loss(m, in_home), bad = CH_BAD_PCT + typical;
  /* Loss this high on the typical channel is the link being full (or every channel bad alike), not some
     channels: striking them only narrowed the hopping down to the floor (8 of 16 with 8 busy devices). */
  const int everywhere = typical >= CH_EVERYWHERE_PCT;
  for (;;) {                                                 /* drop the worst bad channel, again and again */
    int worst = -1;
    for (int k = 0; k < DML_NHOP; k++)
      if ((m >> k) & 1 && chs[k].polls >= 20 && chs[k].loss_pct > bad &&
          (worst < 0 || chs[k].loss_pct > chs[worst].loss_pct)) worst = k;
    if (worst < 0 || everywhere) break;
    if (on <= keep && (on <= least || chs[worst].loss_pct < 2 * typical)) break;   /* below keep: clearly bad only */
    m &= (uint16_t)~(1u << worst); on--;
    uint32_t off = 30000u << (chs[worst].strikes < 4 ? chs[worst].strikes : 4);
    if (off > 300000u) off = 300000u;
    chs[worst].off_until_ms = ms_ticks + off;
    if (chs[worst].strikes < 8) chs[worst].strikes++;
  }
  int back = -1;                                             /* one expired channel back per round */
  for (int k = 0; k < DML_NHOP; k++)
    if (!((m >> k) & 1) && ((home >> k) & 1) && (int32_t)(ms_ticks - chs[k].off_until_ms) >= 0 &&
        (back < 0 || (int32_t)(chs[k].off_until_ms - chs[back].off_until_ms) < 0)) back = k;
  if (back >= 0 || on == 0) { if (back < 0) back = home_first; m |= (uint16_t)(1u << back); chs[back].off_until_ms = ms_ticks; }
  for (int k = 0; k < DML_NHOP; k++) { chs[k].polls = 0; chs[k].lost = 0; }
  next_mask = m;
}
/* The hopping at each cycle start: the mask announced in the last cycle is used from now on, the 2 s round
   is judged (next_mask, announced in this cycle), and a new home is taken. On its own so a host test can
   drive it (doorman/test/hop_test.c). */
static void hop_cycle_start(void)
{
  cycle++;
  mask = next_mask;
  cur_ch = dml_channel(cycle, mask);
  channels_eval();                                          /* sets next_mask (announced in this cycle) */
  if (home_set && home == 0xFFFF) {    /* back to all 16: the channels that aren't timed out, at least CH_MIN_ON */
    home_set = 0;
    uint16_t m = home;
    int on = DML_NHOP;
    /* Time-outs are at most 300 s: one further ahead is an old value read the wrong way round, and over.
       Outside the home nothing updates them, so after 24.8 days (ms_ticks on by 2^31) a signed compare
       would keep those channels off, even ones never struck (0) on a dongle up that long. */
    for (int c = 0; c < DML_NHOP; c++) if (chs[c].off_until_ms - ms_ticks - 1u < 300000u) { m &= (uint16_t)~(1u << c); on--; }
    while (on < CH_MIN_ON) {                                /* the ones due back soonest */
      int c1 = -1;
      for (int c = 0; c < DML_NHOP; c++)
        if (!((m >> c) & 1) && (c1 < 0 || (int32_t)(chs[c].off_until_ms - chs[c1].off_until_ms) < 0)) c1 = c;
      m |= (uint16_t)(1u << c1); chs[c1].off_until_ms = ms_ticks; on++;
    }
    next_mask = m;
  } else if (home_set) {
    /* A new home is judged on its own: all its channels on and a fresh 2 s round. Time-outs from the whole
       band would otherwise leave it on whatever survived them (3 of 8 seen) until they end; strikes stay, so
       a channel dropped again stays off longer. */
    home_set = 0;
    for (int c = 0; c < DML_NHOP; c++) {
      if ((home >> c) & 1) chs[c].off_until_ms = ms_ticks;
      chs[c].polls = 0; chs[c].lost = 0;
    }
    ch_eval_ms = ms_ticks;
    next_mask = home;
  }
}

/* ---------------- repeater dongles (the code calls them satellites: sat_*, sv_*) ----------------
 * A repeater listens through the whole cycle and keeps every device answer (RESP) it hears. After the device
 * slots, the main sends each repeater a NEED that lists the answers it missed, and the repeater replies with an
 * SREP carrying the ones it has. The main takes those through answer_take(), like direct answers, and only if
 * each is the next in sequence. None of this runs while sat_on is 0 (no repeaters in use). */
#define TX_ADDR_US     64u             /* TXEN to the packet's ADDRESS: 40 ramp-up + 24 */
#define SAT_NEED_US    220u            /* cycle time per repeater: a NEED with 8 seqs + set-up */
#define SAT_WAIT_US    200u            /* NEED end -> SREP ADDRESS wait (it comes at ~124) */
#define SAT_TAIL_US    60u             /* kept free before the next cycle's first POLL */
#define SAT_PROC_US    40u             /* to take an SREP's records */
#define SAT_JOIN_US    700u            /* the join window */
static int popc(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }
/* RADIO END events so far (TIMER2 counts them) */
static uint32_t end_count(void) { NRF_TIMER2->TASKS_CAPTURE[0] = 1; return NRF_TIMER2->CC[0]; }
/* ---- main side ---- */
static GRP_M uint8_t join_cyc;               /* this cycle ends with a join beacon (decided at its start) */
/* Each NEED tells the repeater when the next cycle starts, and link_poll starts it then (sat_next_t). */
static GRP_M uint32_t sat_len;               /* this cycle's planned length: slots, join window, reserve */
static GRP_M uint32_t sat_next_t;            /* the next start, as the last NEED announced it */
static GRP_M uint8_t sat_next_ok;            /* sat_next_t is set */
/* Per repeater: NEEDs that asked for something, SREPs received, answers taken from them, ms_ticks of its
   last SREP, NEEDs unanswered in a row. */
/* Per slot: answers relayed with the expected seq, answers taken, and its -dBm at the repeater. */
static GRP_M struct { uint32_t relayed, acc; uint8_t rssi; } rly[DML_SLOTS];
/* Relaying only while it's needed. A repeater in use costs every device on the main ~0.9 ms a cycle (the reserve
   and the join window kept in every cycle), lossy or not, so the main relays only while some device loses
   answers. Each slot's loss as the main alone (what the repeater took back counts as lost), per mille, is
   averaged over ~2 s (fast) and ~10 s (slow): on once one's fast average reaches RU_ON_PCT %, off once every
   slow one is under RU_OFF_PCT %. Single seconds were too noisy for this: clean devices at 1.5-1.8 % went over
   2 % now and then and kept it on. A repeater that takes back under a tenth of what's lost over RU_USELESS_S
   seconds (out of range, badly placed, unplugged), with at least RU_USELESS_N lost, isn't asked for
   RU_BACKOFF_S seconds. (Judged per second, a slot losing ~20 a second fell short of the count now and then
   and an unplugged repeater stayed in use, the join window kept in every cycle.) Meanwhile the repeater keeps time
   from the main's POLLs and JOIN beacons. */
#define RU_ON_PCT      4u
#define RU_OFF_PCT_X10 25u             /* 2.5 % */
#define RU_USELESS_S   10u
#define RU_USELESS_N   50u
#define RU_BACKOFF_S   60u
static GRP_M uint8_t ru_on;                  /* relaying now */
static GRP_M uint8_t ru_win_s;               /* seconds into the current RU_USELESS_S window */
static GRP_M uint32_t ru_win_lost, ru_win_taken;   /* answers lost (as the main alone) and taken back in it */
static GRP_M uint16_t ru_fast[DML_SLOTS], ru_slow[DML_SLOTS];   /* loss per mille, averaged ~2 s and ~10 s */
static GRP_M uint16_t ru_backoff_s;          /* seconds left before a useless repeater is tried again */
static GRP_M uint32_t ru_ms, ru_polls[DML_SLOTS], ru_lost[DML_SLOTS], ru_taken[DML_SLOTS];
static void ru_tick(void)
{
  if (ms_ticks - ru_ms < 1000u) return;
  ru_ms = ms_ticks;
  uint32_t fast = 0, slow = 0, lost_all = 0, taken_all = 0;  /* the highest averages, per mille */
  for (int i = 0; i < DML_SLOTS; i++) {
    const slot_t *s = &sl[i];
    const uint32_t polls = s->polls - ru_polls[i], lost = (s->polls - s->resp_ok) - ru_lost[i], taken = rly[i].acc - ru_taken[i];
    ru_polls[i] = s->polls; ru_lost[i] = s->polls - s->resp_ok; ru_taken[i] = rly[i].acc;
    if (!((plan >> i) & 1) || polls < 20) { ru_fast[i] = ru_slow[i] = 0; continue; }   /* (just joined or left) */
    const uint32_t alone = lost + taken;
    const uint32_t pm = 1000u * alone / polls;
    ru_fast[i] = (uint16_t)((ru_fast[i] + pm) / 2u);
    ru_slow[i] = (uint16_t)(ru_slow[i] + ((int32_t)pm - ru_slow[i]) / 8);
    if (ru_fast[i] > fast) fast = ru_fast[i];
    if (ru_slow[i] > slow) slow = ru_slow[i];
    lost_all += alone; taken_all += taken;
  }
  ru_worst_pm = (uint16_t)fast;
  if (ru_backoff_s) ru_backoff_s--;
  for (int k = 0; k < 2; k++) {                             /* auto repeaters gone quiet */
    if (!((sat_auto >> k) & 1)) continue;
    const uint32_t last = sats[k].last_ms && (int32_t)(sats[k].last_ms - sat_seen_ms[k]) > 0 ? sats[k].last_ms : sat_seen_ms[k];
    if (ms_ticks - last > SAT_AUTO_DROP_MS) { sat_auto &= (uint8_t)~(1u << k); sat_on &= (uint8_t)~(1u << k); }
  }
  if (!sat_on) { ru_on = 0; return; }
  if (!ru_on) {
    if (fast >= RU_ON_PCT * 10u && !ru_backoff_s) { ru_on = 1; ru_win_s = 0; ru_win_lost = ru_win_taken = 0; }
    return;
  }
  ru_win_lost += lost_all; ru_win_taken += taken_all;
  if (slow < RU_OFF_PCT_X10) { ru_on = 0; return; }
  if (++ru_win_s >= RU_USELESS_S) {
    if (ru_win_lost >= RU_USELESS_N && ru_win_taken * 10u < ru_win_lost) { ru_on = 0; ru_backoff_s = RU_BACKOFF_S; }
    ru_win_s = 0; ru_win_lost = ru_win_taken = 0;
  }
}

/* Writes the repeater ids in use (sat_on) to flash, if they changed. */
static void sat_save(void) { const uint8_t m = (uint8_t)(sat_on & ~sat_auto); if (pairs.r.sat_mask != m) { pairs.r.sat_mask = m; pairs_save(); } }

/* The repeater reserve: time added to every cycle while repeaters are in use, whether anything was missed or
   not. It must stay steady, because a device that misses a POLL keeps hopping on the last period it heard.
   It covers the NEEDs, plus the part of a full SREP that doesn't fit in the join window. */
static GRP_M uint32_t sat_r;                 /* the reserve in use; follows sat_reserve() by at most 8 us a cycle */
#define SAT_ABSENT_N   16              /* NEEDs unanswered in a row: the repeater counts as absent */
#define SAT_ABSENT_EVERY 16            /* an absent repeater still gets every 16th NEED, to resync if it's back */
static int sat_absent(int sid) { return sats[sid].quiet >= SAT_ABSENT_N; }
static uint32_t sat_reserve(void)
{
  int n = 0;
  for (int k = 0; k < DML_SAT_MAX; k++) if ((sat_on >> k) & 1 && !sat_absent(k)) n++;
  if (!n) return 0;                                         /* no repeater answering: no reserve */
  const uint32_t one = DML_SREP_TURN_US + 76u + SAT_PROC_US + 4u * (DML_SREP_HDR + DML_SLOTS + 2u + max_resp);
  const uint32_t idle = SAT_JOIN_US - SAT_TAIL_US;
  return (uint32_t)n * SAT_NEED_US + (one > idle ? one - idle : 0u);
}
/* The slots a repeater listens to next cycle: every planned one, or with more than it can hear, those that
   lost the most answers lately (loss16: per slot, 16x the share of the last ~16 cycles' POLLs lost). */
static GRP_M uint16_t loss16[DML_SLOTS];
static uint16_t sat_listen(uint32_t missed)
{
  uint16_t listen = (uint16_t)plan; int n = plan_n;
  for (int i = 0; i < DML_SLOTS; i++)
    if ((plan >> i) & 1) loss16[i] = (uint16_t)(loss16[i] - (loss16[i] + 15) / 16 + ((missed >> i) & 1 ? 16 : 0));
  while (n > DML_SAT_LISTEN) {                              /* drop the least lossy (the highest slot on a tie) */
    int lo = -1;
    for (int i = 0; i < DML_SLOTS; i++) if ((listen >> i) & 1 && (lo < 0 || loss16[i] <= loss16[lo])) lo = i;
    listen &= (uint16_t)~(1u << lo); n--;
  }
  return listen;
}
/* Sends one NEED to repeater sid at target and takes the answers its SREP relays. want: the slots still
   missing an answer; listen: the slots it should listen to next cycle; t_next: the next cycle's start; t_lim:
   the SREP must end by then. Returns the slots whose missing answer was taken. */
static uint16_t need_one(uint8_t sid, uint16_t want, uint16_t listen, uint32_t target, uint32_t t_next, uint32_t t_lim)
{
  satst_t *st = &sats[sid];
  /* At least 20 us ahead, so the compare starts it (unless building the NEED takes more than 10 us). */
  const uint32_t soon = us_time() + 20;
  if ((int32_t)(target - soon) < 0) target = soon;
  int len = DML_NEED_HDR + popc(want);
  const uint32_t t_end = target + HX_RAMP_US + 4u * (9u + (uint32_t)len);
  const int32_t room = (int32_t)(t_lim - t_end) - (int32_t)SAT_PROC_US;
  /* SREP bytes: 4 us each after 76 us ramp-up and framing (negative: no room at all) */
  int cap = (room - (int32_t)(DML_SREP_TURN_US + 76)) >> 2;
  if (cap > DML_MAXLEN) cap = DML_MAXLEN;
  /* No room for a useful SREP (one small answer; that's 260 us of room, more than the SAT_WAIT_US the main waits
     for one): the NEED still goes out (it carries the next cycle's timing), asking for nothing. */
  if (cap < DML_SREP_HDR + DML_SLOTS + 2 + DML_RESP_HDR + 8) { want = 0; cap = 0; len = DML_NEED_HDR; }
  uint8_t *p = hx_buf + 1;
  uint32_t to_next = t_next - (target + TX_ADDR_US);
  const uint8_t late = (int32_t)to_next <= 0 || to_next > 0xFFFFu;   /* this cycle overran: no next start to announce */
  if (late) to_next = 0;
  p[0] = DML_NEED; p[1] = sid; p[2] = (uint8_t)(cycle + 1); p[3] = (uint8_t)next_mask; p[4] = (uint8_t)(next_mask >> 8);
  p[5] = (uint8_t)(sat_len / 50);                          /* this cycle's length: the repeater's estimate */
  p[6] = (uint8_t)to_next; p[7] = (uint8_t)(to_next >> 8);
  p[8] = (uint8_t)listen; p[9] = (uint8_t)(listen >> 8); p[10] = (uint8_t)want; p[11] = (uint8_t)(want >> 8);
  p[12] = (uint8_t)cap; p[13] = late ? DML_NEED_F_NOSTART : 0;
  int k = DML_NEED_HDR;
  for (uint8_t i = 0; i < DML_SLOTS; i++) if ((want >> i) & 1) p[k++] = (uint8_t)((sl[i].last_seq + 1) & DML_F_SEQ_MASK);
  hx_buf[0] = (uint8_t)len;
  if (want) st->asks++;
  const int n = hx_txrx(cur_ch, DML_SAT_BASE(net_base), (uint8_t)(DML_SAT_PREFIX + sid), target, 1, t_next,
                        want ? SAT_WAIT_US : 30u, 0);   /* nothing wanted: no reply, wait 30 us past the end */
  if (!want) return 0;
  const uint8_t *r = hx_buf + 1;
  if (n < DML_SREP_HDR || r[0] != DML_SREP || r[1] != sid || r[2] != cycle) { if (st->quiet < 0xFFFF) st->quiet++; return 0; }
  st->quiet = 0; st->sreps++; st->last_ms = ms_ticks; sat_rssi_add(sid, (uint8_t)NRF_RADIO->RSSISAMPLE);
  const uint16_t heard = (uint16_t)(r[3] | r[4] << 8);
  int q = DML_SREP_HDR;
  for (uint8_t i = 0; i < DML_SLOTS; i++) if ((heard >> i) & 1) { rly[i].rssi = q < n ? r[q] : 0; q++; }
  uint16_t got = 0;
  const int hop = hop_of(cur_ch);                           /* (>= 0: cur_ch is always a hop channel) */
  while (q + 2 <= n) {                                      /* records [slot, 0x80: from the cycle before][len][RESP] */
    const uint8_t i = r[q] & 0x7F, rl = r[q + 1], now = !(r[q] & 0x80);
    const uint8_t *rp = r + q + 2;
    q += 2 + rl;
    if (q > n) break;
    /* (the repeater keeps only RESPs, each under its own slot: no need to check rp[0] and rp[1] again) */
    if (i >= DML_SLOTS || !((want >> i) & 1) || rl < DML_RESP_HDR) continue;
    slot_t *s = &sl[i]; KEEP_PTR(s);
    /* The expected seq (it goes on, and so does a held one after it), or the one after it (kept, as a selective
       ack: the POLL then asks the device for the missing one only). Anything else is old. */
    const uint8_t d = (uint8_t)((rp[2] - s->last_seq) & DML_F_SEQ_MASK);
    if (d != 1 && !(d == 2 && !s->held)) continue;
    rly[i].relayed++;
    while (decode_step(s)) {}                                /* its previous answer first, to keep frames in order */
    if (slot_skipping(s)) continue;                          /* that answer made the slot rejoin: drop this one */
    answer_take(s, i, rp, rl, 0, now);                       /* d 1: taken; d 2: kept, the slot still lacks its answer */
    st->acc++; rly[i].acc++;
    if (d == 1) got |= (uint16_t)(1u << i);
    if (now && chs[hop].lost) chs[hop].lost--;   /* this cycle's answer came after all: not lost on this channel */
  }
  return got;
}
/* After the device slots: one NEED to each repeater in use (the second asks only for what the first didn't
   deliver), then the join beacon if it's due and fits; otherwise it waits for the next cycle. Returns 1 if the
   beacon was sent. */
static int need_round(uint16_t want, uint16_t listen, uint32_t target, uint32_t t_plan)
{
  const uint32_t t_next = t_c + sat_len;
  sat_next_t = t_next; sat_next_ok = 1;
  /* A due beacon normally gives way to the NEEDs and moves to the next cycle. After 40 ms without one (twice
     its interval), its window is kept free first. */
  const int join_force = ms_ticks - last_join_ms >= 40;
  const uint32_t t_lim = t_next - SAT_TAIL_US - (join_force ? SAT_JOIN_US : 0u);
  const uint32_t last_start = t_lim - (uint32_t)popc(sat_on) * SAT_NEED_US;   /* latest start that fits all NEEDs */
  /* The NEEDs were pushed back to wait out a late answer from the last slot and no longer fit: none this
     cycle, rather than send over that answer. */
  const int skip = (int32_t)(target - last_start) > 0 && (int32_t)(target - t_plan) > 0;
  uint8_t ask = 0;
  for (uint8_t sid = 0; sid < DML_SAT_MAX; sid++)
    /* absent: every 16th cycle */
    if ((sat_on >> sid) & 1 && !(sat_absent(sid) && (cycle % SAT_ABSENT_EVERY))) ask |= (uint8_t)(1u << sid);
  for (uint8_t sid = 0; sid < DML_SAT_MAX && !skip; sid++) {
    if (!((ask >> sid) & 1)) continue;
    const uint32_t later = (uint32_t)popc(ask >> (sid + 1)) * SAT_NEED_US;   /* room for the NEEDs after this one */
    want &= (uint16_t)~need_one(sid, want, listen, target, t_next, t_lim - later);
    target = us_time();
  }
  if (join_cyc || join_force) {
    if ((int32_t)(t_next - us_time()) >= (int32_t)SAT_JOIN_US) {
      wait_until(t_next - SAT_JOIN_US);
      last_join_ms = ms_ticks; join_beacon();
      return 1;
    }
  }
  return 0;                                                 /* no room: next cycle (join_cyc stays set) */
}

/* ---- repeater side ----
   All radio work happens in the RADIO interrupt, which has priority over USB. sat_poll (its link_poll) only
   moves to the next cycle when no NEED came, and restarts after timeouts. */
/* SP_RX listening; SP_RETUNE stopping to change channel; SP_TXWAIT stopping the receiver for an SREP;
   SP_TXON SREP armed or on the air. */
enum { SP_OFF, SP_RX, SP_RETUNE, SP_TXWAIT, SP_TXON };
static GRP_R volatile uint8_t sp_phase;
static GRP_M uint8_t role_run, sv_started;   /* the role running; the repeater has started */
static uint8_t sr_buf[2][2 + DML_MAXLEN], sr_tx[2 + DML_MAXLEN];   /* 2 receive buffers in turn; the SREP */
static GRP_R uint8_t sr_cur;           /* the receive buffer in use */
/* The last two RESPs heard from each slot ([0] the newer), tagged with their cycle and ms_ticks. A NEED is
   answered from those of its cycle and the one before (no older: one could carry the expected 3-bit seq and be
   another answer). The older one fills a gap the main keeps a later answer behind (DML_F_SACK). */
typedef struct { uint8_t valid, rssi, cyc; uint32_t ms; uint8_t b[2 + DML_MAXLEN]; } kept_t;
static kept_t kept[DML_SLOTS][2];
static GRP_R uint32_t sv_endc;               /* END count at the last interrupt (+2: one was missed) */
/* The main followed and our id on it: set by hand (0x3F), or, for a repeater without a set main (sv_auto), the
   main it chose and the place that main gave it (SREQ, then named in its JOIN). */
static GRP_R volatile uint32_t sv_mbase;
static GRP_R volatile uint8_t sv_id, sv_auto;
static GRP_R volatile uint32_t sv_want;                  /* auto: the main it asks */
static GRP_R volatile uint8_t sv_confirm;
static GRP_R volatile uint8_t sv_keep;                   /* auto: renew the place (an SREQ) from the next JOIN */                /* auto: asked, not yet named in a JOIN */
static GRP_R volatile uint32_t sv_txt, sv_need_ms;   /* when the armed SREP or SREQ starts */
static GRP_R volatile uint32_t sv_ask_ms, sv_asked_first_ms, sv_kept_ms, sv_since_ms, sv_full_base, sv_full_ms;
static GRP_R volatile uint8_t sv_search;  /* 1: looking for the main's JOIN beacon (ch 80); sv_begin starts with it */
static GRP_R uint8_t sv_cyc, sv_ch;          /* the main's cycle, its channel */
static GRP_R uint16_t sv_listen;              /* the slots the main asked us to listen to (NEED listen) */
static GRP_R uint16_t sv_cmask, sv_nmask;   /* channel mask of this cycle and the next (from the JOIN beacon on) */
/* Keeping time. A POLL's period field is the time from the previous cycle's start to this one's, in 50 us
   steps. Only a NEED gives the exact time to the next start; without one the repeater coasts on its estimate. */
static GRP_D uint32_t sv_period = 6000;      /* estimate of the time from this cycle's start to the next's */
static GRP_R uint32_t sv_tcs, sv_tprev;      /* this cycle's start (estimated), the previous one's */
static GRP_R uint8_t sv_exact, sv_p50;       /* sv_tcs came from a NEED/JOIN; sv_period's period field */
static GRP_R volatile uint32_t sv_heard_ms, sv_isr_ms;   /* ms_ticks when the main was last heard; of the last interrupt */
static GRP_R uint32_t sv_cycles, sv_needs, sv_sreps;     /* status counters (cmd 0x3D) */
static GRP_R uint8_t sv_coast_run;           /* cycles in a row without a NEED */
#define SV_COAST_MAX 64                /* then resync from the JOIN beacon (every ~20 ms) */
static GRP_R uint32_t sv_heard[DML_SLOTS];                 /* RESPs heard per slot */
static GRP_R uint16_t sv_rssi16[DML_SLOTS], sv_mrssi16;    /* -dBm x16, smoothed: per slot, and of the main */

static void sv_rssi(uint16_t *a, uint8_t v) { *a = *a ? (uint16_t)(*a - *a / 16 + v) : (uint16_t)(v * 16); }
/* With the radio disabled: listen on this cycle's channel, or for the JOIN beacon while searching. */
/* Mains heard on the join channel while searching (cmd 0x3D page 2): to choose one. */
#define SV_MAINS 8
static GRP_R struct { uint32_t base, ms; uint16_t rssi16; uint8_t loss, reps, beacons; } sv_mains[SV_MAINS];
static void sv_main_heard(const uint8_t *p, int n, uint8_t rssi)
{
  uint32_t nb; memcpy(&nb, p + 1, 4);
  int e = -1, old = 0;
  for (int i = 0; i < SV_MAINS; i++) {
    if (sv_mains[i].base == nb) { e = i; break; }
    if (ms_ticks - sv_mains[i].ms > ms_ticks - sv_mains[old].ms) old = i;
  }
  if (e < 0) { e = old; sv_mains[e].base = nb; sv_mains[e].rssi16 = 0; sv_mains[e].beacons = 0; }
  sv_mains[e].ms = ms_ticks; sv_rssi(&sv_mains[e].rssi16, rssi);
  if (sv_mains[e].beacons < 255) sv_mains[e].beacons++;
  sv_mains[e].loss = n >= DML_JOIN_LEN + 2 ? p[21] : 0xFF;   /* 0xFF: older main, doesn't say */
  sv_mains[e].reps = n >= DML_JOIN_LEN + 2 ? p[22] : 0xFF;
  if (n >= DML_JOIN_LEN + 2 && (p[22] & 0x40)) auto_busy_ms = ms_ticks;   /* a main with devices: not alone */
}
static void sv_rx_start(void)
{
  NRF_RADIO->SHORTS = 0;
  uint32_t pw[2] = { DML_JOIN_PREFIX, 0 }, base = DML_JOIN_BASE, rxa = 1;   /* PREFIX0 and 1: logical addresses 0-7 */
  uint8_t ch = DML_JOIN_CHANNEL;
  if (!sv_search) {
    /* Logical address 0 is our own, for NEEDs. 1-7 are device slots on the main's network: the planned ones
       first, then the rest (devices that may join). The radio has 8, so with 8 devices one slot isn't heard. */
    ch = dml_channel(sv_cyc, sv_cmask);
    base = DML_SAT_BASE(sv_mbase); NRF_RADIO->BASE1 = sv_mbase;
    uint8_t *pf = (uint8_t *)pw;
    pf[0] = (uint8_t)(DML_SAT_PREFIX + sv_id);
    int k = 1;
    uint16_t m = sv_listen;
    for (int pass = 0; pass < 2; pass++, m = (uint16_t)~m)
      for (int i = 0; i < DML_SLOTS && k < 8; i++) if ((m >> i) & 1) pf[k++] = (uint8_t)(DML_SLOT_PREFIX + i);
    rxa = 0xFF;
  }
  NRF_RADIO->FREQUENCY = sv_ch = ch;
  NRF_RADIO->BASE0 = base; NRF_RADIO->PREFIX0 = pw[0]; NRF_RADIO->PREFIX1 = pw[1];
  NRF_RADIO->RXADDRESSES = rxa;
  sv_endc = end_count();
  NRF_RADIO->PACKETPTR = (uint32_t)sr_buf[0]; sr_cur = 0;   /* this START uses buffer 0; READY points at 1 */
  NRF_RADIO->EVENTS_READY = 0; NRF_RADIO->EVENTS_END = 0; NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  sp_phase = SP_RX;
  fem_rx();
  NRF_RADIO->TASKS_RXEN = 1;
}
/* Stops the receiver; the DISABLED interrupt then starts it on the current channel (sv_rx_start). */
static void sv_retune(void)
{
  NRF_RADIO->SHORTS = 0;
  sp_phase = SP_RETUNE;
  NRF_RADIO->TASKS_DISABLE = 1;
}
/* A packet of this cycle heard at ta, so the cycle can't end before ta + 500 us. Coasting on the period from
   the POLLs (rounded to 50 us) drifts; this moves an end estimated too early back. */
static void sv_seen(uint32_t ta)
{
  if ((int32_t)(ta + 500u - (sv_tcs + sv_period)) > 0) sv_tcs = ta + 500u - sv_period;
}
/* The exact period is the time between one slot's POLLs in consecutive cycles (the announced one is rounded
   down to 50 us). The last POLL heard per slot: */
static GRP_R struct { uint32_t t; uint8_t cyc, per50; } sv_lp[DML_SLOTS];
/* A POLL to slot k heard at ta: take the cycle, the next channel mask and the period from it. */
static void sv_poll_heard(uint8_t k, uint8_t cyc, uint16_t nm, uint8_t per50, uint8_t rssi, uint32_t ta)
{
  sv_heard_ms = ms_ticks; sv_rssi(&sv_mrssi16, rssi);
  const uint32_t est = per50 * 50u + 25u;                  /* mid-step: the main rounds down */
  if (cyc != sv_cyc) {                                      /* another cycle on our channel: follow it */
    for (int i = 0; i < DML_SLOTS; i++) kept[i][0].valid = kept[i][1].valid = 0;   /* the kept RESPs are another cycle's */
    sv_cyc = cyc; sv_tcs = ta - TX_ADDR_US; sv_tprev = sv_tcs - est; sv_exact = 0;   /* as if it were the first POLL */
  } else if (!sv_exact && per50 && per50 != sv_p50) sv_tcs = sv_tprev + est;   /* period changed while coasting */
  sv_nmask = nm;
  if (per50) {
    const uint32_t dt = ta - sv_lp[k].t;                    /* one cycle later: exact if nothing changed */
    if ((uint8_t)(sv_lp[k].cyc + 1) == cyc && sv_lp[k].per50 == per50 && dt - est + 26u <= 52u) sv_period = dt;   /* est +- 26 */
    else if (per50 != sv_p50) sv_period = est;
    sv_p50 = per50;
    sv_lp[k].t = ta; sv_lp[k].cyc = cyc; sv_lp[k].per50 = per50;
  }
  sv_seen(ta);
}
/* Slot i's kept RESP of cycle cyc, if any. */
static const kept_t *kept_of(int i, uint8_t cyc)
{
  for (int e = 0; e < 2; e++) {
    const kept_t *x = &kept[i][e]; KEEP_PTR(x);
    if (x->valid && x->cyc == cyc && ms_ticks - x->ms < 40) return x;   /* (40 ms: no stale one from a wrapped cycle) */
  }
  return 0;
}
/* A NEED for us: arms the SREP if anything is wanted, then takes the next cycle's number, mask and start
   from it. Returns 1 if the SREP is armed, 0 to retune for the next cycle. */
static int sv_need(const uint8_t *p, int n, uint8_t rssi, uint32_t ta)
{
  sv_needs++; sv_heard_ms = ms_ticks; sv_need_ms = ms_ticks; sv_rssi(&sv_mrssi16, rssi);
  const uint16_t want = (uint16_t)(p[10] | p[11] << 8);
  const uint8_t cap = p[12] > DML_MAXLEN ? DML_MAXLEN : p[12];   /* no more than sr_tx holds */
  int armed = 0;
  if (want) {
    uint8_t *q = sr_tx + 1;
    uint16_t heard = 0;
    const uint8_t cyc = (uint8_t)(p[2] - 1);               /* the cycle the NEED is about */
    int len = DML_SREP_HDR, k = DML_NEED_HDR;
    for (int i = 0; i < DML_SLOTS; i++) {
      const kept_t *e = kept_of(i, cyc);
      if (e) { heard |= (uint16_t)(1u << i); q[len++] = e->rssi; }
    }
    q[0] = DML_SREP; q[1] = sv_id; q[2] = sv_cyc; q[3] = (uint8_t)heard; q[4] = (uint8_t)(heard >> 8);   /* the main checks the cycle */
    for (int i = 0; i < DML_SLOTS; i++) {
      if (!((want >> i) & 1)) continue;
      if (k >= n) break;
      const uint8_t exp = p[k++] & DML_F_SEQ_MASK;
      for (int m = 0; m < 2; m++) {                         /* the expected answer, then the one after it */
        const uint8_t ws = (uint8_t)((exp + m) & DML_F_SEQ_MASK);
        const kept_t *e = 0;
        int old = 0;                                        /* from this cycle, or else (1) from the one before */
        for (; old < 2; old++) { e = kept_of(i, (uint8_t)(cyc - old)); if (e && (e->b[1 + 2] & DML_F_SEQ_MASK) == ws) break; }
        if (old == 2) continue;
        const uint8_t rl = e->b[0];
        if (len + 2 + rl > cap) break;
        q[len++] = (uint8_t)(i | old << 7); q[len++] = rl; memcpy(q + len, e->b + 1, rl); len += rl;
      }
    }
    sr_tx[0] = (uint8_t)len;
    const uint32_t t_tx = ta + 4u * (3u + (uint32_t)n) + DML_SREP_TURN_US;   /* NEED end + turnaround */
    if ((int32_t)(t_tx - us_now()) > HX_ARM_MIN_US && len <= cap) {
      NRF_RADIO->SHORTS = 0;
      NRF_RADIO->TASKS_DISABLE = 1;
      NRF_RADIO->PACKETPTR = (uint32_t)sr_tx; NRF_RADIO->TXADDRESS = 0;
      NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
      fem_off();                                            /* PPI_LAUNCH's fork sets the amplifier to TX (Holyiot) */
      NRF_TIMER3->EVENTS_COMPARE[CC_LAUNCH] = 0;
      NRF_TIMER3->CC[CC_LAUNCH] = t_tx; sv_txt = t_tx;
      NRF_PPI->CHENSET = 1u << PPI_LAUNCH;
      if ((int32_t)(t_tx - us_now()) > 0) { sp_phase = SP_TXWAIT; sv_sreps++; armed = 1; }
      else { NRF_PPI->CHENCLR = 1u << PPI_LAUNCH; sp_phase = SP_RETUNE; }   /* start passed while arming: no SREP */
    }
  }
  /* The next cycle: number, channel mask and start; listen to the planned slots first. */
  sv_cyc = p[2]; sv_cmask = sv_nmask = (uint16_t)(p[3] | p[4] << 8);
  const int nostart = p[13] & DML_NEED_F_NOSTART;           /* main's cycle overran: next one starts now */
  const uint32_t tn = nostart ? ta + 4u * (3u + (uint32_t)n) + 300u : ta + (uint32_t)(p[6] | p[7] << 8);
  if (p[5] && p[5] != sv_p50) { sv_period = p[5] * 50u + 25u; sv_p50 = p[5]; }
  sv_tprev = sv_tcs; sv_tcs = tn; sv_exact = !nostart;
  sv_listen = (uint16_t)(p[8] | p[9] << 8);
  sv_cycles++; sv_coast_run = 0;
  return armed;
}
/* Sends sr_tx at t_tx on the current address 0 (the receiver stops first). Returns 1 if armed. */
static int sv_tx_at(uint32_t t_tx)
{
  if ((int32_t)(t_tx - us_now()) <= HX_ARM_MIN_US) return 0;
  sv_txt = t_tx;
  NRF_RADIO->SHORTS = 0;
  NRF_RADIO->TASKS_DISABLE = 1;
  NRF_RADIO->PACKETPTR = (uint32_t)sr_tx; NRF_RADIO->TXADDRESS = 0;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
  fem_off();
  NRF_TIMER3->EVENTS_COMPARE[CC_LAUNCH] = 0;
  NRF_TIMER3->CC[CC_LAUNCH] = t_tx;
  NRF_PPI->CHENSET = 1u << PPI_LAUNCH;
  if ((int32_t)(t_tx - us_now()) > 0) { sp_phase = SP_TXWAIT; return 1; }
  NRF_PPI->CHENCLR = 1u << PPI_LAUNCH; sp_phase = SP_RETUNE; return 0;
}
/* A JOIN from main nb, ending 4*(3+n) us after ta: an auto repeater asks it for a place, or keeps it. Returns 1
   if the SREQ is armed, 0 if it's named (follow it now), -1 to keep listening. */
static int sv_auto_join(const uint8_t *p, int n, uint32_t nb, uint32_t ta)
{
  if (!sv_want || nb != sv_want) return -1;
  if (sv_keep && !sv_confirm) {                             /* renewing: send, whatever the JOIN says */
    sv_keep = 0; sv_ask_ms = ms_ticks - 4000;
  } else if (n >= DML_JOIN_LEN_X && (p[23] & 0x80) && !memcmp(p + 24, &net_base, 4)) {   /* named: our place */
    if (sv_mbase != nb) sv_since_ms = ms_ticks;
    sv_id = p[23] & 1; sv_mbase = nb; sv_confirm = 0;
    return 0;
  }
  if (!sv_confirm && sv_mbase == nb && ms_ticks - sv_ask_ms < 4000) return -1;   /* the JOIN names the other one: wait for ours */
  if (ms_ticks - sv_ask_ms < 60) return -1;               /* a few beacons for the answer */
  if (!sv_confirm) { sv_confirm = 1; sv_asked_first_ms = ms_ticks; }
  sv_ask_ms = ms_ticks;
  uint8_t *q = sr_tx + 1;
  q[0] = DML_SREQ; memcpy(q + 1, &net_base, 4); memcpy(q + 5, &nb, 4);
  const uint32_t id[2] = { NRF_FICR->DEVICEID[1], NRF_FICR->DEVICEID[0] };
  memcpy(q + 9, id, 8); q[17] = BOARD_ID; q[18] = (uint8_t)DONGLE_FW_VERSION; q[19] = (uint8_t)(DONGLE_FW_VERSION >> 8);
  q[20] = (uint8_t)(sv_mrssi16 / 16); q[21] = (uint8_t)link_led();
  sr_tx[0] = DML_SREQ_LEN_X;
  return sv_tx_at(ta + 4u * (3u + (uint32_t)n) + DML_SREP_TURN_US);
}
/* Auto: the main to ask. The one losing the most (what a repeater is for), then the one with fewer repeaters,
   then the strongest; not one that was full in the last 30 s, nor an older main (it can't take one itself). */
static uint32_t sv_choose(uint32_t skip)
{
  int best = -1; int32_t bs = 0;
  for (int i = 0; i < SV_MAINS; i++) {
    if (!sv_mains[i].base || ms_ticks - sv_mains[i].ms > 3000 || sv_mains[i].base == skip || sv_mains[i].reps == 0xFF) continue;
    if (auto_rep && !(sv_mains[i].reps & 0x40)) continue;   /* on its own: only mains with devices */
    if (sv_mains[i].base == sv_full_base && ms_ticks - sv_full_ms < 30000) continue;
    const int loss = sv_mains[i].loss > 100 ? 0 : sv_mains[i].loss;
    const int32_t sc = loss * 1000 - popc(sv_mains[i].reps & 3) * 300 - sv_mains[i].rssi16 / 16;
    if (best < 0 || sc > bs) { best = i; bs = sc; }
  }
  return best < 0 ? 0 : sv_mains[best].base;
}
void RADIO_IRQHandler(void)                                 /* repeaters only: a main never sets INTEN */
{
  sv_isr_ms = ms_ticks;
  if (NRF_RADIO->EVENTS_READY) {
    NRF_RADIO->EVENTS_READY = 0;
    /* START took buffer 0; the next packet uses 1 */
    if (sp_phase == SP_RX) NRF_RADIO->PACKETPTR = (uint32_t)sr_buf[1];
  }
  if (NRF_RADIO->EVENTS_END) {
    NRF_RADIO->EVENTS_END = 0;
    if (sp_phase == SP_RX) {
      uint8_t *b = sr_buf[sr_cur];
      const uint8_t crc = (uint8_t)NRF_RADIO->CRCSTATUS, la = (uint8_t)NRF_RADIO->RXMATCH, rssi = (uint8_t)NRF_RADIO->RSSISAMPLE;
      const uint32_t ta = NRF_TIMER3->CC[CC_ADDR];
      int act = -1;                                         /* -1 keep listening, 0 retune, 1 SREP armed */
      const int n = b[0] > DML_MAXLEN ? DML_MAXLEN : b[0];
      const uint8_t *p = b + 1;
      const uint32_t ec = end_count();
      const int late = ec - sv_endc > 1;                    /* ran after the next END too: its CRC etc. */
      sv_endc = ec;
      if (late || !crc) {}                                  /* drop it */
      else if (sv_search) {
        uint32_t nb; memcpy(&nb, p + 1, 4);
        int aj = -1;
        if (n >= DML_JOIN_LEN && p[0] == DML_JOIN) { sv_main_heard(p, n, rssi); if (sv_auto) aj = sv_auto_join(p, n, nb, ta); }
        if (aj == 1) act = 1;
        else if (n >= DML_JOIN_LEN && p[0] == DML_JOIN && sv_mbase && nb == sv_mbase && (!sv_auto || aj == 0)) {
          sv_cyc = (uint8_t)(p[17] + 1); sv_cmask = sv_nmask = (uint16_t)(p[15] | p[16] << 8);
          if (p[18]) { sv_period = p[18] * 50u + 25u; sv_p50 = p[18]; }
          sv_tcs = ta + SAT_JOIN_US - TX_ADDR_US;           /* next cycle: 700 us after the beacon */
          sv_tprev = sv_tcs - sv_period; sv_exact = 1;
          sv_search = 0; sv_heard_ms = ms_ticks; sv_rssi(&sv_mrssi16, rssi);
          act = 0;
        }
      } else if (la == 0) {
        if (n >= DML_NEED_HDR && p[0] == DML_NEED && p[1] == sv_id) act = sv_need(p, n, rssi, ta);
      } else if (p[0] == DML_RESP && n >= DML_RESP_HDR && p[1] < DML_SLOTS) {
        kept_t *e = kept[p[1]]; KEEP_PTR(e);
        if (e[0].valid && e[0].cyc != sv_cyc) e[1] = e[0];  /* (a second RESP in the same cycle replaces the first) */
        e[0].valid = 1; e[0].rssi = rssi; e[0].cyc = sv_cyc; e[0].ms = ms_ticks; memcpy(e[0].b, b, (size_t)n + 1);
        sv_heard[p[1]]++; sv_rssi(&sv_rssi16[p[1]], rssi); sv_heard_ms = ms_ticks;
        sv_seen(ta);
      } else if (p[0] == DML_POLL && n >= DML_POLL_HDR && p[1] < DML_SLOTS) sv_poll_heard(p[1], p[3], (uint16_t)(p[4] | p[5] << 8), p[6], rssi, ta);
      if (act == 0) sv_retune();
      else if (act < 0) { NRF_RADIO->PACKETPTR = (uint32_t)b; sr_cur ^= 1; }   /* for the packet after the next one */
    }
  }
  if (NRF_RADIO->EVENTS_DISABLED) {
    NRF_RADIO->EVENTS_DISABLED = 0;
    const uint8_t ph = sp_phase;
    if (ph == SP_TXWAIT) sp_phase = SP_TXON;               /* receiver off: the compare starts the SREP */
    else if (ph != SP_OFF) {                                /* SREP sent, retune, or the receiver stopped: listen */
      if (ph == SP_TXON) { NRF_PPI->CHENCLR = 1u << PPI_LAUNCH; fem_off(); }
      sv_rx_start();
    }
  }
  (void)NRF_RADIO->EVENTS_DISABLED;
}
static void sv_begin(void)   /* (re)start the repeater: look for the main's JOIN beacon */
{
  radio_off();
  sp_phase = SP_OFF;
  /* TinyUSB stops the crystal when VBUS drops (a weak charger), and the radio needs it */
  if (!(NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk)) NRF_CLOCK->TASKS_HFCLKSTART = 1;
  sv_search = 1; sv_started = 1; sv_listen = 0;
  sv_auto = pairs.r.main_base == 0;
  if (!sv_auto) { sv_mbase = pairs.r.main_base; sv_id = pairs.r.sat_id; }
  else { sv_mbase = 0; sv_want = 0; sv_confirm = 0; }
  sv_heard_ms = sv_isr_ms = ms_ticks;
  __disable_irq();
  sv_rx_start();                                            /* clears the three events: no interrupt for an old one */
  NRF_RADIO->INTENSET = RADIO_INTENSET_READY_Msk | RADIO_INTENSET_END_Msk | RADIO_INTENSET_DISABLED_Msk;
  __enable_irq();
}
/* link_poll for a repeater. It never blocks, so it runs without a USB host. Moves to the next cycle when no
   NEED came, and goes back to the JOIN beacon after 1 s without hearing the main. */
static void sat_poll(void)
{
  /* No radio interrupt for 3 s: start over. Compared signed, because the interrupt can set sv_isr_ms to a
     newer tick than the ms_ticks read just before it. */
  if (!sv_started || (int32_t)(ms_ticks - sv_isr_ms) > 3000) sv_begin();
  __disable_irq();
  /* an SREP that never finished */
  if (sp_phase >= SP_TXWAIT && (int32_t)(us_time() - sv_txt) > 3000) {   /* (from its start: sv_tcs is stale while searching) */   /* (SP_TXWAIT or SP_TXON) */
    NRF_PPI->CHENCLR = 1u << PPI_LAUNCH;
    if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) sv_rx_start(); else sv_retune();
  }
  if (sv_auto && sp_phase == SP_RX) {
    if (sv_confirm && ms_ticks - sv_asked_first_ms > 2000) {   /* no place given: full, or it doesn't hear us */
      sv_full_base = sv_want; sv_full_ms = ms_ticks; sv_want = 0; sv_mbase = 0; sv_confirm = 0;
    }
    if (!sv_want && sv_search && ms_ticks - sv_kept_ms > 1000) { sv_want = sv_choose(0); sv_kept_ms = ms_ticks; }
    if (sv_mbase && !sv_search && ms_ticks - sv_kept_ms > 4000) {   /* keep the place: ask again from the JOIN */
      sv_keep = 1; sv_kept_ms = ms_ticks; sv_search = 1; sv_retune();
    }
    /* Once a minute: move to a main losing much more than ours (what we hear while keeping the place) */
    if (sv_mbase && !sv_confirm && ms_ticks - sv_since_ms > 60000) {
      sv_since_ms = ms_ticks;
      const uint32_t o = sv_choose(sv_mbase);
      int lo = -1, lc = 0;
      for (int i = 0; i < SV_MAINS; i++) {
        if (sv_mains[i].base == o) lo = sv_mains[i].loss > 100 ? 0 : sv_mains[i].loss;
        if (sv_mains[i].base == sv_mbase) lc = sv_mains[i].loss > 100 ? 0 : sv_mains[i].loss;
      }
      if (o && lo >= lc + 10) { sv_mbase = 0; sv_want = o; sv_search = 1; sv_retune(); }
    }
  }
  if (sp_phase == SP_RX && !sv_search) {
    if (ms_ticks - sv_heard_ms > 1000) { sv_search = 1; sv_retune(); }
    /* just before the next start (its ADDRESS: +64) */
    else if ((int32_t)(us_time() - (sv_tcs + sv_period - 60u)) > 0) {
      sv_cyc++; sv_cmask = sv_nmask; sv_tprev = sv_tcs; sv_tcs += sv_period; sv_exact = 0; sv_cycles++;
      if (++sv_coast_run >= SV_COAST_MAX) { sv_coast_run = 0; sv_search = 1; }   /* no NEEDs: resync from the beacon */
      sv_retune();
    }
  }
  __enable_irq();
}
int link_led(void)                                          /* for main.c's LED, see link.h */
{
  if (!role_run) return -1;
  if (!sv_mbase || (sv_search && ms_ticks - sv_heard_ms > 1000)) return 0;
  if (sv_mrssi16 / 16 >= SAT_WEAK_DBM) return 3;
  return ms_ticks - sv_need_ms < 300 ? 2 : 1;
}

/* ---------------- main-loop entry points ---------------- */
void link_init(void)
{
  NRF_TIMER3->MODE = 0; NRF_TIMER3->BITMODE = 3; NRF_TIMER3->PRESCALER = 4;   /* 1 MHz */
  NRF_TIMER3->TASKS_CLEAR = 1; NRF_TIMER3->TASKS_START = 1;
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0; NRF_CLOCK->TASKS_HFCLKSTART = 1;
  uint32_t t = us_now(); while (!NRF_CLOCK->EVENTS_HFCLKSTARTED && us_now() - t < 5000) {}
  NRF_RADIO->POWER = 1;
  NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
  NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;   /* 40 us ramp-up */
  NRF_RADIO->TXPOWER = RADIO_TXPOWER;
  NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
  NRF_RADIO->PCNF1 = (DML_MAXLEN << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
  NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Two; NRF_RADIO->CRCINIT = 0xFFFF; NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->INTENCLR = 0xFFFFFFFF;
  uint32_t h = NRF_FICR->DEVICEID[0] * 2654435761u ^ NRF_FICR->DEVICEID[1];
  net_base = (h & 0x7F7F7F7Fu) | 0x01010101u;               /* per-dongle network address */
  fem_init();
  /* PPI, see the top of the file. The compare start and the one-shot turnaround stay off until an exchange
     arms them; the other channels are always on. Group 0 holds the turnaround (and the amplifier's, Holyiot);
     PPI_TURN's fork disables the group at the first DISABLED, so each fires once. */
  NRF_PPI->CHENCLR = 1u << PPI_LAUNCH | 1u << PPI_TURN;
  NRF_PPI->CH[PPI_LAUNCH].EEP = (uint32_t)&NRF_TIMER3->EVENTS_COMPARE[CC_LAUNCH];
  NRF_PPI->CH[PPI_LAUNCH].TEP = (uint32_t)&NRF_RADIO->TASKS_TXEN;
#if BOARD_HOLYIOT
  NRF_PPI->FORK[PPI_LAUNCH].TEP = (uint32_t)&NRF_GPIOTE->TASKS_SET[FEM_GT_CTX];   /* amplifier TX on (fem_tx) */
  NRF_PPI->CHG[HX_GROUP] = 1u << PPI_TURN | 1u << FEM_PPI;
#else
  NRF_PPI->FORK[PPI_LAUNCH].TEP = 0;
  NRF_PPI->CHG[HX_GROUP] = 1u << PPI_TURN;
#endif
  NRF_PPI->CH[PPI_TURN].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;
  NRF_PPI->CH[PPI_TURN].TEP = (uint32_t)&NRF_RADIO->TASKS_RXEN;
  NRF_PPI->FORK[PPI_TURN].TEP = (uint32_t)&NRF_PPI->TASKS_CHG[HX_GROUP].DIS;
  NRF_PPI->CH[PPI_ADDR].EEP = (uint32_t)&NRF_RADIO->EVENTS_ADDRESS;
  NRF_PPI->CH[PPI_ADDR].TEP = (uint32_t)&NRF_TIMER3->TASKS_CAPTURE[CC_ADDR];
  NRF_PPI->CH[PPI_TXRDY].EEP = (uint32_t)&NRF_RADIO->EVENTS_TXREADY;
  NRF_PPI->CH[PPI_TXRDY].TEP = (uint32_t)&NRF_TIMER3->TASKS_CAPTURE[CC_TXRDY];
  NRF_TIMER2->TASKS_STOP = 1; NRF_TIMER2->MODE = 2; NRF_TIMER2->BITMODE = 3;   /* RADIO END counter */
  NRF_TIMER2->TASKS_CLEAR = 1; NRF_TIMER2->TASKS_START = 1;
  NRF_PPI->CH[PPI_ENDCNT].EEP = (uint32_t)&NRF_RADIO->EVENTS_END;
  NRF_PPI->CH[PPI_ENDCNT].TEP = (uint32_t)&NRF_TIMER2->TASKS_COUNT;
  NVIC_SetPriority(RADIO_IRQn, 1);                          /* above USBD (2), below SysTick (0) */
  NVIC_ClearPendingIRQ(RADIO_IRQn); NVIC_EnableIRQ(RADIO_IRQn);   /* a main never sets INTEN: no interrupts */
  NRF_PPI->CHENSET = 1u << PPI_ADDR | 1u << PPI_TXRDY | 1u << PPI_ENDCNT;
  pairs_load(); sat_on = pairs.r.sat_mask;
  for (int i = 0; i < DML_SLOTS; i++) { sl[i].last_seq = 7; sl[i].dseq = 0; }
}

/* A main with no devices: listens on the join channel for a JOIN from another main that has some. */
static int hear_busy_main(uint32_t us)
{
  radio_off();
  NRF_RADIO->FREQUENCY = DML_JOIN_CHANNEL;
  NRF_RADIO->BASE0 = DML_JOIN_BASE; NRF_RADIO->PREFIX0 = DML_JOIN_PREFIX; NRF_RADIO->RXADDRESSES = 1;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_rx;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
  NRF_RADIO->EVENTS_END = 0;
  fem_rx();
  NRF_RADIO->TASKS_RXEN = 1;
  const uint32_t t0 = us_now();
  int found = 0;
  while (!found && us_now() - t0 < us) {
    if (NRF_RADIO->EVENTS_END) {
      NRF_RADIO->EVENTS_END = 0;
      const uint8_t *p = pkt_rx + 1;
      uint32_t nb; memcpy(&nb, p + 1, 4);
      if (NRF_RADIO->CRCSTATUS && pkt_rx[0] >= DML_JOIN_LEN + 2 && p[0] == DML_JOIN && nb != net_base && (p[22] & 0x40)) found = 1;
      else NRF_RADIO->TASKS_START = 1;
    }
    if (idle_hook) idle_hook();
  }
  radio_off();
  return found;
}
/* An auto repeater with devices paired: its own JOIN, so a device of its that turns on can find it. Returns 1 if
   one did (then it's a dongle again). */
static int own_join(void)
{
  NRF_RADIO->INTENCLR = 0xFFFFFFFF;
  radio_off();
  join_beacon();
  radio_off();
  NVIC_ClearPendingIRQ(RADIO_IRQn);
  for (int i = 0; i < DML_SLOTS; i++) if ((pairs.used >> i) & 1 && sl[i].active) return 1;
  __disable_irq();
  sv_rx_start();
  NRF_RADIO->INTENSET = RADIO_INTENSET_READY_Msk | RADIO_INTENSET_END_Msk | RADIO_INTENSET_DISABLED_Msk;
  __enable_irq();
  return 0;
}
void link_poll(void)
{
  load_tick();
  if (pairs_dirty) { pairs_dirty = 0; pairs_save(); }
  if (!link_on) { radio_off(); sv_started = 0; return; }
  /* role changed (0x3F): restart in it */
  if (pairs.r.role) auto_rep = 0;
  if (auto_rep) {
    if (ms_ticks < pairing_until_ms || ms_ticks - auto_busy_ms > AUTO_ALONE_MS) auto_rep = 0;
    else if (pairs.used && sp_phase == SP_RX && ms_ticks - auto_own_ms >= AUTO_OWN_JOIN_MS) {
      auto_own_ms = ms_ticks;
      if (own_join()) auto_rep = 0;
    }
  }
  const uint8_t want_role = pairs.r.role ? 1 : auto_rep;
  if (role_run != want_role) {
    NRF_RADIO->INTENCLR = 0xFFFFFFFF; radio_off(); NVIC_ClearPendingIRQ(RADIO_IRQn);
    role_run = want_role; sv_started = 0;
  }
  if (role_run) { sat_poll(); return; }                     /* a repeater never sends a JOIN or POLL */
  tmode = tmode_req; adaptive = tmode & 1;                  /* a mode change (0x2D) applies here */
  const int hw = tmode == 3;
  /* TDMA plan, rebuilt each cycle from the connected slots: the k-th polled at t_c + k*slot_us, then the
     join window. Radios don't need their place in it: they listen for their own address and take the
     period from each POLL, so the plan can change from one cycle to the next. */
  plan = 0;
  int n = 0;
  for (int i = 0; i < DML_SLOTS; i++) {
    if (sl[i].skip_until && !slot_skipping(&sl[i])) sl[i].skip_until = 0;   /* over (and no wrap later) */
    /* A skipping slot keeps its place, with no POLL in it: the cycle's length and every other
       slot's time stay as they were (and so does where they fall against another dongle's). */
    if (((pairs.used >> i) & 1) && sl[i].active && !((test_skip >> i) & 1)) { plan |= 1u << i; n++; }
  }
  /* bigger responses amortise the ~450 us per-slot overhead: 6 x 120 B = 6.3 ms cycle, ~18 kB/s per device
     (72 B gave 12.9 kB/s, too little headroom for resends; queues overflowed). 7-8 devices: 176 B, 9.9 ms
     and ~17.7 kB/s each at 8, over the ~13 kB/s a device sends at its worst (LH1, two bases) */
  plan_n = n;
  if (!n && !pairs.r.role && ms_ticks >= pairing_until_ms && ms_ticks - auto_listen_ms >= AUTO_LISTEN_EVERY) {
    auto_listen_ms = ms_ticks;
    if (hear_busy_main(AUTO_LISTEN_US)) { auto_rep = 1; auto_busy_ms = ms_ticks; return; }   /* a repeater from the next call */
  }
  if (ms_ticks - sizing_ms >= 2000) { sizing_ms = ms_ticks; sizing_eval(&sizing); }
  max_resp = (uint8_t)sizing_resp(&sizing, n);   /* sizing.h: the size for this many devices, or large answers while saturated */
  /* The cycle goes out as one byte of 50 us (POLL p[6], JOIN p[18]): 12.75 ms at most. Nine or ten devices with
     large answers made it longer, the byte wrapped, and every device hopped on a wrong clock and missed polls.
     So with many devices the answers stay short enough for it: 10 -> 188 B, 9 -> 222 B, 8 or fewer unchanged. */
  if (n) {                                                  /* (and the repeater reserve, when in use) */
    const int cap = (int)(((PERIOD_MAX_US - 700u - sat_r) / (uint32_t)n - 450u) / 4u);
    if (max_resp > cap) max_resp = (uint8_t)cap;
  }
  /* Each slot's share of the cycle: what the slowest exchange of the last 32 cycles took plus a guard,
     not the worst case (450 us + 4 us per byte of a full answer, kept as the bound). Answers are
     rarely full, so the cycle is shorter and every device is polled more often. Off: cmd 0x2D 0. */
  const uint32_t worst = 450u + 4u * max_resp;
  slot_us = adaptive && ex_budget ? (ex_budget + SLOT_GUARD_US < worst ? ex_budget + SLOT_GUARD_US : worst) : worst;
  ru_tick();
  sat_on_b = (uint8_t)sat_on; ru_on_b = ru_on;
  const int sat = (sat_on && ru_on) || sat_r;               /* once per cycle: the mask command can run mid-cycle */
  /* The join window (700 us) only in the cycles that send a JOIN beacon (every 20 ms): every cycle kept
     it, and at a 4.5 ms cycle three in four carried 700 us of nothing (15% of the cycle, longer between
     each device's polls). The others end JOIN_TAIL_US after their last slot. Radios take the period from
     each POLL and find their polls by address, so cycles of two lengths are no trouble to them. With
     repeaters every cycle keeps it: their SREPs use it, and the cycle has to stay steady (sat_reserve). */
  const int join = ms_ticks - last_join_ms >= 20;
  period_us = n * slot_us + (sat || join || test_join_always ? 700u : JOIN_TAIL_US);
  /* no device connected: safe to save */
  if (!plan) sat_save();
  if (sat) {                                                /* repeaters in use: room for NEEDs and SREPs */
    join_cyc = join_cyc || ms_ticks - last_join_ms + (period_us + 999u) / 1000u >= 20u;
    const uint32_t want_r = sat_on && ru_on ? sat_reserve() : 0u;   /* by <= 8 us a cycle, so devices don't drift */
    sat_r = want_r > sat_r + 8u ? sat_r + 8u : want_r + 8u < sat_r ? sat_r - 8u : want_r;
    /* at most PERIOD_MAX_US: the POLL sends period/50 in a byte (period_us alone is within it: max_resp's cap) */
    sat_len = period_us + sat_r <= PERIOD_MAX_US ? period_us + sat_r : PERIOD_MAX_US;
    const uint32_t pr = sat_next_t - t_c;                   /* keep the start the last NEED announced */
    if (sat_next_ok && pr > 0 && pr <= PERIOD_MAX_US) period_us = cycle_len = pr;
    else period_us = sat_len;
  } else sat_next_ok = 0;
  t_c += cycle_len;                                         /* the previous cycle's own length */
  cycle_len = period_us;
  late_tick(period_us, sat);                                /* off while relaying: early POLLs doubled the loss */
  if ((int32_t)(us_now() - t_c) > (int32_t)period_us || (int32_t)(t_c - us_now()) > (int32_t)period_us)
    t_c = us_now();                                         /* fell behind / first run: restart the plan */
  if (!hw || !n) wait_until(t_c);                           /* mode 3: the compare starts the first POLL */
  if (hx_usb_cost > 40) hx_usb_cost -= (hx_usb_cost - 40 + 15) / 16;   /* decays back toward 40 us */
  hop_cycle_start();
  int k = 0;
  uint32_t missed = 0;
  for (uint8_t i = 0; i < DML_SLOTS; i++) {
    if (!((plan >> i) & 1)) continue;
    const uint32_t target = t_c + (uint32_t)k * slot_us;
    if (!hw) wait_until(target);
    k++;
    if (slot_skipping(&sl[i])) continue;                    /* its place, left empty */
    uint32_t t0 = hw ? target : us_now();
    uint8_t ch = cur_ch;
    const slot_t *g = &sl[i];
    if (ghost && g->g_have && g->g_miss % 3 != 0 && t0 - g->g_t[0] < 1000000u) {
      const int e = g->g_miss % 3 == 2 && g->g_per[1] ? 1 : 0;   /* misses 1, 4, ..: last record; 2, 5, ..: the one before */
      uint32_t j = (t0 - g->g_t[e] + g->g_per[e] / 2u) / g->g_per[e];   /* the device's own rule */
      if (j < 1) j = 1;                                    /* it went to the next cycle's channel on that POLL */
      ch = dml_channel((uint8_t)(g->g_cyc[e] + j), g->g_mask[e]);
    }
    if (!poll_one(i, ch, t0, 1, target + slot_us)) missed |= 1u << i;   /* (mode 3: t0 is target) */
    if (hw) t0 = ex_t_tx - HX_RAMP_US;                      /* measured from its actual start */
    const uint32_t dur = us_now() - t0;
    ex_hist[dur / 50 < EX_BUCKETS ? dur / 50 : EX_BUCKETS - 1]++; ex_hn++;
    ex_sum += dur; ex_n++;
    if (dur > ex_max) ex_max = dur;
  }
  if (++ex_cycles >= 32) {                                  /* 90th percentile of the last 32 cycles */
    uint32_t c = 0, b = 0;
    while (b < EX_BUCKETS - 1 && (c += ex_hist[b]) * 10 < ex_hn * 9) b++;
    ex_budget = (b + 1) * 50u;
    memset(ex_hist, 0, sizeof ex_hist); ex_hn = 0; ex_cycles = 0;
  }
  if (sat) {                                                /* repeaters: NEEDs, then the join beacon (no re-polls) */
    const uint32_t tn = t_c + (uint32_t)k * slot_us;
    /* The last POLL got no answer, but a late one may still be on the air: start the NEEDs only after the
       longest possible answer (turnaround + full RESP) would have ended. */
    uint32_t tg = tn;
    const int last = 31 - __builtin_clz(plan | 1u);
    if ((missed >> last) & 1) {                              /* (only planned slots are in missed) */
      const uint32_t safe = NRF_TIMER3->CC[CC_TXRDY] + 4u * (9u + DML_POLL_HDR + sl[last].down_sent) + 400u + 4u * (9u + max_resp) - HX_RAMP_US;
      if ((int32_t)(safe - tg) > 0) tg = safe;
    }
    /* Wanted: the slots that lost this cycle's answer, and those holding an answer behind a lost one (the
       repeater may have the lost one from the cycle before). Not slots rejoining after a dup run. */
    uint16_t want = 0;
    for (int i = 0; i < DML_SLOTS; i++)
      if ((plan >> i) & 1 && ((missed >> i) & 1 || sl[i].held) && !slot_skipping(&sl[i])) want |= (uint16_t)(1u << i);
    if (need_round(want, sat_listen(missed), tg, tn)) join_cyc = 0;
    return;
  }
  /* A slot that missed its POLL is polled again in this cycle's spare time (cycles without a join
     window, while a whole slot still fits): its data waited a full cycle otherwise, and a weak link
     losing a few in a row piled those up. Off by default: with six devices only 106 of 1357 re-polls
     were answered (whatever lost the POLL was still there on the same channel). On: cmd 0x2F 1. */
  if (repoll && !join)
    for (uint8_t i = 0; i < DML_SLOTS && missed; i++) {
      if (!((missed >> i) & 1)) continue;
      missed &= ~(1u << i);
      const uint32_t tr = us_now();
      if ((int32_t)(t_c + period_us - (tr + slot_us)) < 0) break;
      repolls++;
      if (poll_one(i, cur_ch, tr, 0, 0)) repolls_ok++;
    }
  if (join) {
    wait_until(t_c + (uint32_t)k * slot_us);
    last_join_ms = ms_ticks; join_beacon();
  }
}

/* 0x80 [slot][n][whole [len][frame] items]; a frame too big for one report goes as consecutive
   0x81 [slot][n][raw bytes of [len][frame]] fragments */
/* Packed stream (cmd 0x37, the host asks for it): [0x82][slot][n][seq][first][n bytes], the slot's
   [len][frame] items back to back across reports, so every report goes out full: whole frames left LH2's
   ~35-byte frames one to a report, and two busy devices filled an interface's 1000 reports a second. seq
   counts the slot's reports (a gap: the host starts again), first is where the first item starting in this
   report begins (0xFF: none). */
static uint8_t usb_packed;
static int stream_pop_packed(uint8_t *rep, slot_t *s, uint8_t si)
{
  const int used = upq_used(s);
  if (!used) return 0;
  const int n = used < 59 ? used : 59;
  int first = s->pk_rest < n ? s->pk_rest : 0xFF;
  if (first != 0xFF) {                                     /* where the next report's first item begins */
    int pos = first;
    while (pos < n) pos += 1 + s->upq[(s->up_head + pos) & (UPQ - 1)];
    s->pk_rest = (uint16_t)(pos - n);
  } else s->pk_rest = (uint16_t)(s->pk_rest - n);
  for (int i = 0; i < n; i++) rep[5 + i] = s->upq[(s->up_head + i) & (UPQ - 1)];
  s->up_head = (s->up_head + n) & (UPQ - 1);
  rep[0] = 0x82; rep[1] = si; rep[2] = (uint8_t)n; rep[3] = s->pk_seq++; rep[4] = (uint8_t)first;
  return 1;
}
int link_stream_pop(uint8_t *rep, int itf)
{
  static uint8_t rrs[DONGLE_NIF];                /* round-robin position per interface */
  uint8_t rr = rrs[itf];
  for (int k = 0; k < DML_SLOTS; k++) {
    uint8_t si = (uint8_t)((rr + k) % DML_SLOTS);
    if (si % DONGLE_NIF != itf) continue;         /* each slot stays on one interface: fragments in order */
    slot_t *s = &sl[si];
    if (usb_packed) {
      if (!stream_pop_packed(rep, s, si)) continue;
      if (s->imu_q_t) { lat_hist(h_usb, us_now() - s->imu_q_t); s->imu_q_t = 0; }
      rrs[itf] = (uint8_t)(si + 1);
      return 1;
    }
    int n = 0;
    if (!s->frag_left) {
      while (upq_used(s)) {
        uint8_t fl = s->upq[s->up_head];
        if (n + 1 + fl > 61) break;
        for (int i = 0; i <= fl; i++) rep[3 + n + i] = s->upq[(s->up_head + i) & (UPQ - 1)];
        s->up_head = (s->up_head + 1 + fl) & (UPQ - 1);
        n += 1 + fl;
      }
      if (!n && upq_used(s)) s->frag_left = (uint16_t)(1 + s->upq[s->up_head]);
    }
    if (s->frag_left) {
      n = s->frag_left > 61 ? 61 : s->frag_left;
      for (int i = 0; i < n; i++) rep[3 + i] = s->upq[(s->up_head + i) & (UPQ - 1)];
      s->up_head = (s->up_head + n) & (UPQ - 1);
      s->frag_left = (uint16_t)(s->frag_left - n);
      rep[0] = 0x81; rep[1] = si; rep[2] = (uint8_t)n;
      rrs[itf] = (uint8_t)(si + 1);
      return 1;
    }
    if (!n) continue;
    rep[0] = 0x80; rep[1] = si; rep[2] = (uint8_t)n;
    rrs[itf] = (uint8_t)(si + 1);
    return 1;
  }
  return 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16_(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

int link_cmd(const uint8_t *in, uint8_t *d, uint8_t *st)    /* 0x20..0x2F, 0x32..0x3F; d = 61 bytes */
{
  uint8_t i = in[2] < DML_SLOTS ? in[2] : 0;
  slot_t *s = &sl[i];
  switch (in[0]) {
  case 0x20: if (in[2] != 0xFF) link_on = in[2]; d[0] = link_on; return 1;   /* radio_off happens in link_poll */
  case 0x21: {                                              /* down: [slot][len][frame] */
    uint8_t n = in[3];
    if (pairs.r.role) { *st = 2; return 1; }                  /* a repeater has no devices */
    if (n == 0 || n > 63 || s->down_len + 1u + n > sizeof s->down_buf) { *st = 2; return 1; }
    s->down_buf[s->down_len++] = n; memcpy(s->down_buf + s->down_len, in + 4, n); s->down_len += n;
    s->down_frames++;
    return 1;
  }
  case 0x23: {                                              /* per-slot stats */
    uint32_t v[15] = { s->polls, s->resp_ok, s->crc_err, s->timeouts, s->dup, s->frames_up, s->bytes_up,
                       s->drops, s->down_frames, s->down_resends, s->rssi_d16 / 16, s->rssi_t16 / 16,
                       s->t_missed, s->t_crc, ms_ticks - s->last_resp_ms };
    memcpy(d, v, sizeof v);
    return 1;
  }
  case 0x29: {                                              /* live: per slot 10 B, slots [first..first+5] */
    const int first = in[2] < DML_SLOTS ? in[2] : 0;
    for (int k = first; k < DML_SLOTS && k < first + 6; k++) {
      uint8_t *e = d + (k - first) * 10; slot_t *q = &sl[k];
      e[0] = (uint8_t)(((pairs.used >> k) & 1) | (q->active << 1));
      e[1] = q->rssi_last_d; e[2] = q->rssi_last_t; e[3] = q->rssi_min_d; e[4] = q->rssi_max_d;
      e[5] = cur_ch;
      put32(e + 6, ms_ticks - q->last_resp_ms);
      q->rssi_min_d = 0; q->rssi_max_d = 0;
    }
    return 1;
  }
  case 0x2A: memcpy(d, s->ident, 16); return 1;
  case 0x2C: if (in[2] != 0xFF) usb_in_radio = in[2]; d[0] = usb_in_radio; return 1;   /* USB inside radio exchanges */
  case 0x2F:                                                /* re-poll missed slots: [0/1, 0xFF keeps] */
    if (in[2] != 0xFF) repoll = in[2];
    d[0] = repoll; put32(d + 1, repolls); put32(d + 5, repolls_ok); return 1;
  case 0x32:                                                /* ghost-follow: [0/1, 0xFF keeps] */
    if (in[2] > 1 && in[2] != 0xFF) { *st = 2; return 1; }
    if (in[2] != 0xFF) ghost = in[2];
    d[0] = ghost; put32(d + 2, ghost_polls); put32(d + 6, ghost_ok); return 1;
  case 0x3C:                                                /* main: repeaters in use [mask, 0xFF: read][1: save now] */
    if (in[2] != 0xFF) {
      if (in[2] >> DML_SAT_MAX || pairs.r.role) { *st = 2; return 1; }   /* bad mask, or we're a repeater */
      /* in RAM at once; a newly used id starts with clear stats */
      if (in[2] != sat_on) {
        for (int k = 0; k < DML_SAT_MAX; k++) if ((in[2] >> k) & 1 && !((sat_on >> k) & 1)) memset(&sats[k], 0, sizeof sats[k]);
        sat_auto &= (uint8_t)~in[2];                        /* set by hand now */
        sat_on = (uint8_t)(in[2] | sat_auto);
      }
      /* setup: save to flash now */
      if (in[3] == 1) sat_save();
    }
    d[0] = sat_on; d[1] = pairs.r.sat_mask; return 1;
  case 0x3D:                                                /* repeater status [page]; counters are totals */
    if (in[2] == 1) {                                       /* per slot: RESPs heard (u32 from 1), -dBm (average, from 41) */
      d[0] = 1;
      for (int k = 0; k < DML_SLOTS; k++) { put32(d + 1 + 4 * k, sv_heard[k]); d[1 + 4 * DML_SLOTS + k] = (uint8_t)(sv_rssi16[k] / 16); }
      return 1;
    }
    if (in[2] == 2) {                                       /* mains heard: 8 x 7 B from 1 (base u32, -dBm, loss %, repeaters) */
      d[0] = 2;
      for (int k = 0; k < SV_MAINS; k++) {
        uint8_t *e = d + 1 + 7 * k;
        if (!sv_mains[k].base || ms_ticks - sv_mains[k].ms > 30000) { memset(e, 0, 7); continue; }
        put32(e, sv_mains[k].base); e[4] = (uint8_t)(sv_mains[k].rssi16 / 16); e[5] = sv_mains[k].loss; e[6] = sv_mains[k].reps;
      }
      return 1;
    }
    if (in[2]) { *st = 2; return 1; }
    d[0] = pairs.r.role; d[1] = link_led() > 0; d[30] = (uint8_t)link_led(); d[2] = sv_id; d[3] = sv_ch; put32(d + 17, sv_mbase);   /* (the main it follows: auto too) */
    put32(d + 4, sv_cycles); put32(d + 8, sv_needs); put32(d + 12, sv_sreps); d[16] = (uint8_t)(sv_mrssi16 / 16);
    return 1;
  case 0x3E:                                                /* main: relay stats [page][first]; totals */
    d[0] = in[2];
    if (in[2] == 0) {                                       /* per repeater: 16 B each from byte 1 */
      for (int k = 0; k < DML_SAT_MAX; k++) {
        uint8_t *e = d + 1 + 16 * k; const satst_t *q = &sats[k];
        put32(e, q->asks); put32(e + 4, q->sreps); put32(e + 8, q->acc);
        put32(e + 12, q->last_ms ? ms_ticks - q->last_ms : 0xFFFFFFFFu);
      }
      d[33] = sat_on; d[34] = (uint8_t)sat_r; d[35] = (uint8_t)(sat_r >> 8);
      d[36] = ru_on; d[37] = (uint8_t)(ru_backoff_s > 255 ? 255 : ru_backoff_s);
      d[38] = sat_auto; put32(d + 39, sat_rbase[0]); put32(d + 43, sat_rbase[1]); d[52] = (uint8_t)(sat_rssi16[0] / 16); d[53] = (uint8_t)(sat_rssi16[1] / 16);   /* auto repeaters, their bases; -dBm each repeater is heard at (0: not yet) */
    } else if (in[2] == 1) {                                /* 6 slots from [first 0-4], 9 B each */
      if (in[3] > DML_SLOTS - 6) { *st = 2; return 1; }
      for (int k = 0; k < 6; k++) {
        const int j = in[3] + k; uint8_t *e = d + 1 + 9 * k;
        put32(e, rly[j].relayed); put32(e + 4, rly[j].acc); e[8] = rly[j].rssi;
      }
    } else if (in[2] == 2) {                                /* repeaters that came on their own: 2 x 20 B from 1 */
      for (int k = 0; k < 2; k++) {
        uint8_t *e = d + 1 + 20 * k;
        memset(e, 0, 20);
        if (!((sat_auto >> k) & 1)) continue;
        put32(e, sat_rbase[k]); memcpy(e + 4, sat_info[k], sizeof sat_info[k]);   /* base, then SREQ bytes 9-21 */
        e[17] = (uint8_t)(sat_rssi16[k] / 16);              /* -dBm the main hears it at */
        put16_(e + 18, (uint16_t)((ms_ticks - sat_seen_ms[k]) / 100u > 0xFFFF ? 0xFFFF : (ms_ticks - sat_seen_ms[k]) / 100u));   /* 0.1 s since it asked */
      }
    } else *st = 2;
    return 1;
  /* role: [0 main/1 repeater, 0xFF: read][id][main's base u32] */
  case 0x3F:
    if (in[2] != 0xFF) {
      uint32_t mb; memcpy(&mb, in + 4, 4);
      /* the main's base must be a valid network base, and not our own */
      const int okb = (mb & 0x80808080u) == 0 && (mb & 0x01010101u) == 0x01010101u && mb != net_base;
      if (in[2] > 1 || (in[2] == 1 && mb && (in[3] >= DML_SAT_MAX || !okb))) { *st = 2; return 1; }   /* base 0: picks a main itself */
      const int same = pairs.r.role == in[2] && (in[2] == 0 || (pairs.r.sat_id == in[3] && pairs.r.main_base == mb));
      if (in[2] == 1) { pairs.r.sat_id = in[3]; pairs.r.main_base = mb; pairs.r.sat_mask = 0; sat_on = 0; }
      if (!same) { pairs.r.role = in[2]; pairs_save(); sv_started = 0; }   /* (another main, or auto: start over) */      /* link_poll switches to it */
    }
    d[0] = pairs.r.role; d[1] = pairs.r.sat_id; put32(d + 2, pairs.r.main_base); put32(d + 6, net_base);
    d[10] = sat_on; d[11] = role_run;
    return 1;
  case 0x33:                                                /* make a slot's radio rejoin: [slot] */
    /* Not polled past the radio's 1 s "dongle gone" timeout, as after a long dup run. The
       host asks for it when a connected device has sent nothing for seconds. */
    if (in[2] >= DML_SLOTS || !((pairs.used >> in[2]) & 1)) { *st = 2; return 1; }
    slot_skip(s);
    d[0] = in[2]; return 1;
  case 0x2E: if (in[2] != 0xFF) defer_decode = in[2]; d[0] = defer_decode; return 1;   /* deferred decode */
  case 0x2D:                                                /* timing: [mode 0, 1, 3; 0xFF keeps] */
    if (in[2] != 0xFF) {
      if (in[2] > 3 || in[2] == 2) { *st = 2; return 1; }
      tmode_req = in[2];
    }
    put32(d, period_us); put32(d + 4, slot_us); put32(d + 8, ex_budget);
    put32(d + 12, ex_n ? ex_sum / ex_n : 0); put32(d + 16, ex_max); d[20] = tmode_req; d[21] = max_resp;
    put32(d + 22, dec_n ? dec_sum / dec_n : 0); put32(d + 26, dec_max);
    d[30] = sizing.worst; d[31] = sizing.big; d[32] = (uint8_t)sizing_base(plan_n); d[33] = (uint8_t)plan_n;
    for (int k = 0; k < DML_SLOTS; k++) d[34 + k] = sizing.pct[k];   /* saturation: worst, large answers, normal size, devices, per slot */
    d[44] = tmode;                                          /* 20: mode requested; 44: in effect */
    ex_sum = ex_n = ex_max = dec_sum = dec_n = dec_max = 0;
    return 1;
  case 0x37:                                                /* packed stream reports on/off (0x82) */
    if (usb_packed != (in[2] ? 1 : 0)) {                    /* both kinds of report start clean */
      usb_packed = in[2] ? 1 : 0;
      for (int k = 0; k < DML_SLOTS; k++) { sl[k].up_head = sl[k].up_tail; sl[k].frag_left = 0; sl[k].pk_rest = 0; }
    }
    d[0] = usb_packed; return 1;
  case 0x2B: load_mask = in[2]; load_kbps = in[3]; d[0] = load_mask; d[1] = load_kbps; return 1;   /* test load */            /* identity ('H' frame) of a slot */
  case 0x28:                                                /* codec, and the radio's own drops */
    put32(d, s->air_bytes); put32(d + 4, s->bytes_up); put32(d + 8, s->bad_frames);
    put32(d + 12, s->dev_drops); put32(d + 16, s->dev_reply_drops);
    d[20] = (uint8_t)s->upq_hw; d[21] = (uint8_t)(s->upq_hw >> 8); s->upq_hw = 0;   /* USB queue high-water, reset on read */
    put32(d + 22, s->load_bytes);                                                    /* test load queued (0x2B) */
    put32(d + 26, s->resyncs); d[30] = s->check; d[31] = s->batch;                   /* codec resyncs, CRC-8 on, batches */
    put32(d + 32, s->held_set); put32(d + 36, s->held_used);                         /* selective ack: answers kept, delivered */
    return 1;
  case 0x24:                                                /* pair SECS [SLOT+1] [ANY] (0/absent: first free slot) */
    if (pairs.r.role) { *st = 2; return 1; }                  /* a repeater never opens a network */
    pairing_until_ms = in[2] ? ms_ticks + 1000u * in[2] : 0;
    pair_target = in[3] >= 1 && in[3] <= DML_SLOTS ? (uint8_t)(in[3] - 1) : 0xFF;
    pair_any = in[4] == 1 && pair_target < DML_SLOTS;       /* [4] 1: any unknown device, not only pairing ones */
    d[0] = in[2]; d[1] = pair_target; d[2] = pair_any; return 1;
  case 0x25: {                                              /* slots [first..first+5]: used, active, id x8 */
    const int first = in[2] < DML_SLOTS ? in[2] : 0;
    for (int k = first; k < DML_SLOTS && k < first + 6; k++) {
      uint8_t *e = d + (k - first) * 10;
      e[0] = (pairs.used >> k) & 1; e[1] = sl[k].active;
      memcpy(e + 2, pairs.id[k], 8);
    }
    d[60] = ms_ticks < pairing_until_ms ? (uint8_t)(1 | (pair_target < DML_SLOTS ? (pair_target + 1) << 1 : 0)) : 0;
    return 1;
  }
  case 0x26:                                                /* forget slot (0xFF = all) */
    if (in[2] == 0xFF) { pairs.used = 0; memset(pairs.name, 0, sizeof pairs.name); memset(pairs.radio, 0, sizeof pairs.radio); }
    else if (in[2] < DML_SLOTS) {
      pairs.used &= ~(1u << in[2]); memset(pairs.name[in[2]], 0, sizeof pairs.name[in[2]]); pairs.radio[in[2]] = 0;
    }
    pairs_save();
    return 1;
  case 0x34: {                                              /* set a slot's name: [slot][len][UTF-8] (len 0 clears) */
    const uint8_t n = in[3];
    if (in[2] >= DML_SLOTS || !((pairs.used >> in[2]) & 1) || n > DML_NAME_MAX) { *st = 2; return 1; }
    char name[64] = {0};
    memcpy(name, in + 4, n);
    if (memcmp(name, pairs.name[in[2]], sizeof name)) { memcpy(pairs.name[in[2]], name, sizeof name); pairs_save(); }
    d[0] = in[2]; return 1;
  }
  case 0x39: {                                              /* a slot's radio version: [slot][u32 LE] (the installer, */
    uint32_t v;                                             /* after updating it: the device is off, it can't say) */
    if (in[2] >= DML_SLOTS || !((pairs.used >> in[2]) & 1)) { *st = 2; return 1; }
    memcpy(&v, in + 3, 4);
    if (pairs.radio[in[2]] != v) { pairs.radio[in[2]] = v; pairs_dirty = 1; }
    d[0] = in[2]; return 1;
  }
  case 0x3B:                                                /* test switches: [what][a][b] -> join_always, skip u16, drop % */
    if (in[2] == 0) test_join_always = in[3] != 0;          /* 0: join window every cycle (a) */
    else if (in[2] == 1) test_skip = (uint16_t)(in[3] | in[4] << 8);   /* 1: slots left out (mask a|b<<8) */
    else if (in[2] == 2) test_drop_pct = in[3] > 100 ? 100 : in[3];    /* 2: answers dropped, % (a) */
    d[0] = test_join_always; d[1] = (uint8_t)test_skip; d[2] = (uint8_t)(test_skip >> 8); d[3] = test_drop_pct;
    return 1;
  case 0x3A:                                                /* latency counters: dec[8] usb[8], u16 each */
    memcpy(d, h_dec, 16); memcpy(d + 16, h_usb, 16); memset(h_dec, 0, 16); memset(h_usb, 0, 16);
    return 1;
  case 0x38:                                                /* radio versions: -> [n][u32 LE per slot] (0 unknown) */
    d[0] = DML_SLOTS;
    for (int k = 0; k < DML_SLOTS; k++) put32(d + 1 + 4 * k, ((pairs.used >> k) & 1) ? pairs.radio[k] : 0);
    return 1;
  case 0x35: {                                              /* a slot's name: [slot] -> [len][UTF-8] */
    if (in[2] >= DML_SLOTS) { *st = 2; return 1; }
    const char *name = pairs.name[in[2]];
    uint8_t n = 0;
    while (n < DML_NAME_MAX && name[n]) n++;
    d[0] = n; memcpy(d + 1, name, n); return 1;
  }
  case 0x36: {                                              /* home: [lo][hi], 0 only reads; 0xFFFF = all 16 */
    const uint16_t h = (uint16_t)(in[2] | in[3] << 8);
    int hn = 0;
    for (int k = 0; k < DML_NHOP; k++) hn += (h >> k) & 1;
    if (h && hn < HOME_MIN_SIZE) { *st = 2; return 1; }     /* fewer than 5 channels: refused, nothing changed */
    if (h && h != home) { home = h; home_set = 1; }         /* taken at the next cycle start; the same home again: nothing */
    int keep, least; home_floors(&keep, &least);
    d[0] = (uint8_t)home; d[1] = (uint8_t)(home >> 8); d[2] = (uint8_t)mask; d[3] = (uint8_t)(mask >> 8);
    d[4] = (uint8_t)keep; d[5] = (uint8_t)least; return 1;
  }
  case 0x27:                                                /* channels: mask, loss % per hop channel */
    d[0] = (uint8_t)mask; d[1] = (uint8_t)(mask >> 8);
    for (int k = 0; k < DML_NHOP; k++) { d[2 + k] = dml_hop_ch[k]; d[18 + k] = chs[k].loss_pct; }
    put32(d + 34, joins_heard); put32(d + 38, net_base);
    return 1;
  }
  return 0;
}

void link_rssi_scan(uint8_t first, uint8_t *out, int n)    /* -dBm per channel, 0xFF = no sample */
{
  uint8_t was_on = link_on;
  link_on = 0; radio_off(); sv_started = 0;                 /* a repeater restarts its receiver after */
  for (int i = 0; i < n; i++) {
    uint8_t ch = (uint8_t)(first + i);
    if (ch > 100) { out[i] = 0xFF; continue; }
    NRF_RADIO->FREQUENCY = ch;
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->EVENTS_READY = 0; fem_rx(); NRF_RADIO->TASKS_RXEN = 1;
    uint32_t t = us_now(); while (!NRF_RADIO->EVENTS_READY && us_now() - t < 2000) {}
    uint8_t best = 0xFF;
    for (int k = 0; k < 8; k++) {
      NRF_RADIO->EVENTS_RSSIEND = 0; NRF_RADIO->TASKS_RSSISTART = 1;
      uint32_t g = 0; while (!NRF_RADIO->EVENTS_RSSIEND && ++g < 2000) {}
      uint8_t v = (uint8_t)NRF_RADIO->RSSISAMPLE;
      if (v < best) best = v;
    }
    out[i] = best;
    radio_off();
  }
  link_on = was_on;
}
