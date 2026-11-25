/* Host test of the dongle's hopping: the real doorman/dongle-fw/src/link.c built on the PC (stub/nrf.h), its
 * hop step (hop_cycle_start: the mask taken at a cycle start, the 2 s adaptive round, a new home from cmd 0x36)
 * driven directly with made-up per-channel loss. Nothing here runs the radio.
 *   1. No home, and an explicit 0x36 0xFFFF: the mask equals master's rule round for round (ref_eval below is
 *      master's channels_eval as of f4269bb, copied), over thousands of random rounds.
 *   2. Homes 0x00FF and 0xFF00: every mask and every channel hopped on is inside the home; never fewer than
 *      HOME_MIN on; a new home starts with all its channels on.
 *   3. Fixed cases: floors inside a home, a home mostly hit by Wi-Fi, refused homes, back to all 16 (at least 8
 *      on, time-outs carried over, and one read the wrong way round after 2^31 ms treated as over).
 *   hop_test            exit status 0 when every check passed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dongle-fw/src/link.c"

volatile uint32_t ms_ticks;
NRF_RADIO_Type stub_radio;
NRF_TIMER_Type stub_timer1, stub_timer2, stub_timer3;
NRF_PPI_Type stub_ppi;
NRF_CLOCK_Type stub_clock;
NRF_FICR_Type stub_ficr;
NRF_NVMC_Type stub_nvmc;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails <= 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rs = 0x9E3779B9u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
/* popc: link.c's own (included above) */

/* ---- master's rule (f4269bb link.c, channels_typical_loss / channels_eval), on its own state ---- */
static struct { uint16_t polls, lost; uint32_t off_until_ms; uint8_t loss_pct, strikes; } rc[DML_NHOP];
static uint16_t ref_mask = 0xFFFF, ref_next = 0xFFFF;
static uint32_t ref_eval_ms;
static uint8_t ref_typical(uint16_t m)
{
  uint8_t v[DML_NHOP]; int n = 0;
  for (int k = 0; k < DML_NHOP; k++)
    if ((m >> k) & 1 && rc[k].polls >= 20) {
      uint8_t x = rc[k].loss_pct; int i = n++;
      while (i > 0 && v[i - 1] > x) { v[i] = v[i - 1]; i--; }
      v[i] = x;
    }
  return n ? v[n / 2] : 0;
}
static void ref_eval(void)
{
  if (ms_ticks - ref_eval_ms < 2000) return;
  ref_eval_ms = ms_ticks;
  uint16_t m = ref_mask;
  int on = 0;
  for (int k = 0; k < DML_NHOP; k++) {
    rc[k].loss_pct = rc[k].polls ? (uint8_t)(100u * rc[k].lost / rc[k].polls) : 0;
    if ((m >> k) & 1) {
      on++;
      if (rc[k].polls >= 20 && rc[k].loss_pct <= 2 + ref_typical(m) && rc[k].strikes && ms_ticks - rc[k].off_until_ms > 120000)
        rc[k].strikes--;
    }
  }
  const unsigned typical = ref_typical(m), bad = CH_BAD_PCT + typical;
  const int everywhere = typical >= CH_EVERYWHERE_PCT;
  for (;;) {
    int worst = -1;
    for (int k = 0; k < DML_NHOP; k++)
      if ((m >> k) & 1 && rc[k].polls >= 20 && rc[k].loss_pct > bad &&
          (worst < 0 || rc[k].loss_pct > rc[worst].loss_pct)) worst = k;
    if (worst < 0 || everywhere || on <= CH_MIN_ON) break;
    m &= (uint16_t)~(1u << worst); on--;
    uint32_t off = 30000u << (rc[worst].strikes < 4 ? rc[worst].strikes : 4);
    if (off > 300000u) off = 300000u;
    rc[worst].off_until_ms = ms_ticks + off;
    if (rc[worst].strikes < 8) rc[worst].strikes++;
  }
  int back = -1;
  for (int k = 0; k < DML_NHOP; k++)
    if (!((m >> k) & 1) && (int32_t)(ms_ticks - rc[k].off_until_ms) >= 0 &&
        (back < 0 || (int32_t)(rc[k].off_until_ms - rc[back].off_until_ms) < 0)) back = k;
  if (back >= 0 || on == 0) { if (back < 0) back = 0; m |= (uint16_t)(1u << back); rc[back].off_until_ms = ms_ticks; }
  for (int k = 0; k < DML_NHOP; k++) { rc[k].polls = 0; rc[k].lost = 0; }
  ref_next = m;
}

/* ---- driving link.c ---- */
static void reset_all(uint32_t ms)
{
  memset(chs, 0, sizeof chs); memset(rc, 0, sizeof rc);
  mask = next_mask = 0xFFFF; home = 0xFFFF; home_set = 0; ch_eval_ms = 0; cycle = 0;
  ref_mask = ref_next = 0xFFFF; ref_eval_ms = 0;
  ms_ticks = ms;
}
static int cmd_home(uint16_t h, uint16_t *home_out, uint16_t *mask_out)
{
  uint8_t in[64] = { 0x36, 1, (uint8_t)h, (uint8_t)(h >> 8) }, d[61] = { 0 }, st = 0;
  CHECK(link_cmd(in, d, &st) == 1, "0x36 not handled");
  if (home_out) *home_out = (uint16_t)(d[0] | d[1] << 8);
  if (mask_out) *mask_out = (uint16_t)(d[2] | d[3] << 8);
  return st;
}
/* One round's statistics on the channels in use (what the dongle counts over 2 s), the same for both. */
static void make_round(uint16_t polls[DML_NHOP], uint16_t lost[DML_NHOP])
{
  const uint32_t kind = rnd() % 100;
  int lo = 0, hi = -1;
  if (kind >= 50 && kind < 70) { lo = (int)(rnd() % 12); hi = lo + 3 + (int)(rnd() % 7); }   /* a Wi-Fi block */
  for (int k = 0; k < DML_NHOP; k++) {
    unsigned pct;
    if (kind < 50) pct = rnd() % 6 + (rnd() % 100 < 25 ? 15 + rnd() % 45 : 0);   /* clean, a few bad */
    else if (kind < 70) pct = (k >= lo && k <= hi) ? 20 + rnd() % 30 : rnd() % 5;
    else if (kind < 85) pct = 18 + rnd() % 25;                                     /* the link full: loss everywhere */
    else pct = rnd() % 101;
    polls[k] = (uint16_t)(rnd() % 100 < 10 ? rnd() % 20 : 20 + rnd() % 230);
    lost[k] = (uint16_t)(polls[k] * pct / 100);
  }
}
/* One 2 s round: the time moves on (sometimes minutes, so time-outs end), both rules judge the same loss. */
static void round_both(int with_ref)
{
  uint16_t p[DML_NHOP], l[DML_NHOP];
  make_round(p, l);
  ms_ticks += rnd() % 100 < 3 ? 60000 + rnd() % 300000 : 2000 + rnd() % 400;
  for (int k = 0; k < DML_NHOP; k++) {
    const int fw_on = (next_mask >> k) & 1, ref_on = (ref_next >> k) & 1;
    chs[k].polls = fw_on ? p[k] : 0; chs[k].lost = fw_on ? l[k] : 0;
    if (with_ref) { rc[k].polls = ref_on ? p[k] : 0; rc[k].lost = ref_on ? l[k] : 0; }
  }
  hop_cycle_start();                                       /* this cycle: mask taken, the round judged */
  if (with_ref) { ref_mask = ref_next; ref_eval(); }
}
/* Cycles between rounds: every channel hopped on is in the mask (and in the home). */
static void cycles(int n, uint16_t within)
{
  for (int i = 0; i < n; i++) {
    hop_cycle_start();
    int ok = 0;
    for (int k = 0; k < DML_NHOP; k++) if (((mask & within) >> k) & 1 && dml_hop_ch[k] == cur_ch) ok = 1;
    CHECK(ok, "cycle %u: channel %u outside mask %04x / home %04x", cycle, cur_ch, mask, within);
  }
}

static void test_master_equal(int explicit_ffff, int rounds)
{
  reset_all(1000);
  if (explicit_ffff) {
    uint16_t h = 0;
    CHECK(cmd_home(0xFFFF, &h, 0) == 0 && h == 0xFFFF, "0x36 0xFFFF refused");
    CHECK(!home_set, "0x36 0xFFFF on a dongle on all 16 should change nothing");
  }
  int diverged = 0;
  for (int r = 0; r < rounds && !diverged; r++) {
    round_both(1);
    if (mask != ref_mask || next_mask != ref_next) diverged = 1;
    CHECK(mask == ref_mask && next_mask == ref_next, "%s round %d: mask %04x next %04x, master %04x next %04x",
          explicit_ffff ? "0xFFFF" : "no home", r, mask, next_mask, ref_mask, ref_next);
    CHECK(popc(next_mask) >= CH_MIN_ON, "all 16: fewer than %d on (%04x)", CH_MIN_ON, next_mask);
    cycles(3, 0xFFFF);
  }
}

static void test_home_random(uint16_t h, int rounds)
{
  reset_all(5000);
  for (int r = 0; r < 50; r++) round_both(0);              /* some history on all 16 first */
  uint16_t hh = 0, mm = 0;
  CHECK(cmd_home(h, &hh, &mm) == 0 && hh == h, "home %04x refused", h);
  hop_cycle_start();                                       /* taken at the next cycle start */
  CHECK(next_mask == h, "home %04x: the new home starts with all its channels on, next mask %04x", h, next_mask);
  hop_cycle_start();
  CHECK(mask == h, "home %04x: mask %04x one cycle later", h, mask);
  int minon = 16;
  for (int r = 0; r < rounds; r++) {
    round_both(0);
    CHECK((next_mask & (uint16_t)~h) == 0 && next_mask, "home %04x round %d: next mask %04x leaves it", h, r, next_mask);
    if (popc(next_mask) < minon) minon = popc(next_mask);
    CHECK(popc(next_mask) >= HOME_MIN, "home %04x round %d: %d on", h, r, popc(next_mask));
    cycles(3, h);
  }
  printf("  home %04x: %d rounds, fewest on %d\n", h, rounds, minon);
}

/* fixed loss on the channels in use, then one round and the cycle start that takes its result */
static void fixed_round(const uint8_t pct[DML_NHOP])
{
  ms_ticks += 2000;
  for (int k = 0; k < DML_NHOP; k++) {
    const int on = (next_mask >> k) & 1;
    chs[k].polls = on ? 200 : 0; chs[k].lost = on ? (uint16_t)(200 * pct[k] / 100) : 0;
  }
  hop_cycle_start();
  hop_cycle_start();
}

static void test_fixed(void)
{
  uint8_t clean[DML_NHOP], two[DML_NHOP], wifi[DML_NHOP];
  for (int k = 0; k < DML_NHOP; k++) { clean[k] = 2; two[k] = 2; wifi[k] = 2; }
  two[1] = 40; two[5] = 35;
  for (int k = 0; k <= 6; k++) wifi[k] = 35;                /* 7 of 0x00FF's 8 lossy, as Wi-Fi 1 + 6 hits that home */

  /* refused / read-only */
  reset_all(1000);
  CHECK(cmd_home(0x000F, 0, 0) == 2 && home == 0xFFFF && !home_set, "4 channels must be refused, nothing changed");
  CHECK(cmd_home(0x0000, 0, 0) == 0 && home == 0xFFFF && !home_set, "0 only reads");
  uint16_t hh, mm;
  CHECK(cmd_home(0x00FF, &hh, &mm) == 0 && hh == 0x00FF && home_set, "0x00FF taken");
  CHECK(cmd_home(0x00FF, 0, 0) == 0 && home == 0x00FF, "the same home again: nothing");
  hop_cycle_start(); hop_cycle_start();
  CHECK(mask == 0x00FF, "home 0x00FF: mask %04x", mask);

  /* inside a home: clean keeps all 8; two bad ones go */
  for (int i = 0; i < 3; i++) fixed_round(clean);
  CHECK(mask == 0x00FF, "home, clean: mask %04x", mask);
  fixed_round(two);
  CHECK(mask == (0x00FF & ~0x0022), "home, two bad (bits 1, 5): mask %04x", mask);

  /* half of the home bad: judged on the lower median (the upper one is a bad channel), all four go */
  uint8_t half[DML_NHOP], mild[DML_NHOP];
  for (int k = 0; k < DML_NHOP; k++) { half[k] = 2; mild[k] = 10; }
  half[0] = 30; half[2] = 35; half[4] = 40; half[6] = 45;
  mild[3] = 19; mild[7] = 19;                               /* bad (over 10 + 8), not twice the typical */
  reset_all(1000);
  cmd_home(0x00FF, 0, 0); hop_cycle_start(); hop_cycle_start();
  fixed_round(half);
  CHECK(mask == 0x00AA, "home, 4 of 8 bad: mask %04x (the four bad ones dropped)", mask);
  /* above HOME_KEEP every bad channel goes, clearly bad or not */
  reset_all(1000);
  cmd_home(0x00FF, 0, 0); hop_cycle_start(); hop_cycle_start();
  fixed_round(mild);
  CHECK(mask == 0x0077, "home, typical 10 %%, two at 19 %%: mask %04x (both dropped above keep)", mask);

  /* a home mostly hit (7 of 8 lossy): the typical channel is a lossy one, nothing is dropped (the doc says so) */
  reset_all(1000);
  cmd_home(0x00FF, 0, 0); hop_cycle_start(); hop_cycle_start();
  for (int i = 0; i < 5; i++) fixed_round(wifi);
  CHECK(mask == 0x00FF, "home 0x00FF with 7 of 8 hit: mask %04x (stays on all 8; documented limit)", mask);

  /* all 16 with those 7 lossy: master's rule drops the 7 (7 of 16 is under half; Wi-Fi 1 + 6 hits 8 of 16,
     and then master drops nothing) */
  reset_all(1000);
  for (int i = 0; i < 5; i++) fixed_round(wifi);
  CHECK(mask == 0xFF80, "all 16, 7 hit: mask %04x (the 7 dropped, the 9 clean ones kept)", mask);

  /* back to all 16: at least 8 on; a fresh time-out carries over */
  reset_all(1000);
  cmd_home(0x00FF, 0, 0); hop_cycle_start(); hop_cycle_start();
  for (int i = 0; i < 3; i++) fixed_round(two);           /* bits 1 and 5 timed out (30 s+) */
  cmd_home(0xFFFF, 0, 0); hop_cycle_start(); hop_cycle_start();
  CHECK(popc(mask) >= CH_MIN_ON && !(mask & 0x0022), "back to 16 at once: mask %04x (1 and 5 still timed out)", mask);

  /* back to all 16 after a home held for more than 2^31 ms: time-outs from before read as over */
  reset_all(1000);
  chs[12].off_until_ms = ms_ticks + 30000;                  /* struck on all 16, then a home without it */
  cmd_home(0x00FF, 0, 0); hop_cycle_start(); hop_cycle_start();
  ms_ticks += 0x80000000u + 60000u;                         /* 24.8 days and a minute on */
  chs[3].off_until_ms = ms_ticks + 30000;                   /* and one struck just now, inside the home */
  cmd_home(0xFFFF, 0, 0); hop_cycle_start(); hop_cycle_start();
  CHECK(mask == 0xFFF7, "after 2^31 ms: old time-outs (12) and never-struck channels read as over, the fresh one (3) "
        "stays: mask %04x", mask);
}

int main(void)
{
  test_master_equal(0, 20000);
  test_master_equal(1, 20000);
  printf("  all 16: 2 x 20000 rounds against master's rule\n");
  test_home_random(0x00FF, 5000);
  test_home_random(0xFF00, 5000);
  test_home_random(0x5555, 2000);
  test_home_random(0x7FFF, 2000);
  test_home_random(0x001F, 3000);   /* the three-dongle split: 5 + 5 + 6 */
  test_home_random(0x03E0, 3000);
  test_home_random(0xFC00, 3000);
  test_fixed();
  printf("hop_test: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
