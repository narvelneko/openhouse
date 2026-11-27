# doorman protocol

Source of truth: `doorman/common/doorman_link.h` (air format, constants) and
`doorman/common/doorman_codec.h` (codec). The dongle side is in `dongle-fw/src/link.c` and
`main.c`, the device side in `radio-fw/src/main.c`.

## Air

* Nordic proprietary **2 Mbit**: 8-bit preamble, 4+1 byte address, 8-bit length, CRC16
  (0x11021, init 0xFFFF), no whitening.
* **The dongle leads.** 8 slots per dongle (firmware 0x000C; 6 before). Each cycle polls every
  paired, connected slot once, all on one channel: the k-th at `cycle_start + k * slot_us`. The
  plan is rebuilt every cycle from the connected slots, so a paired device that's off costs no
  airtime (it comes back through the JOIN beacons). Radios don't need their place in the plan:
  they listen for their own address and take the period from each POLL.
* Reply size by connected devices: 1: 250 B, 2: 160, 3-4: 96, 5-6: 120, 7-8: 176. Poll interval
  `n * (450 + 4 * size) + 700` us: 3.2 ms for 3, 6.3 ms for 6, 9.9 ms for 8.
* Every 20 ms there is also a JOIN beacon on channel 80 (2480 MHz), address "OHJN" / prefix
  0x5A.
* Slot address: BASE = a per-dongle network base (from the dongle's FICR), prefix
  `0xA0 + slot`. A device only hears its own polls.
* The device answers with a **hardware turnaround**: its RESP is prepared before the poll
  arrives and sent by the END→DISABLE→TXEN shortcuts (~150 µs).
* Timing adapts to the number of joined devices n (`link.c`):
  `max_resp = n<=1 ? 250 : n<=2 ? 160 : n<=4 ? 96 : 120` bytes,
  `slot_us = 450 + 4*max_resp`, `period = n*slot_us + 700 µs`.
  * 5–6 devices: 120 B responses, ~6.3 ms cycle, ~18 kB/s per device.
  * Each POLL carries the period (in 50 µs units) and the max RESP length.
* Hopping: 16 channels (2404–2479 MHz, 5 MHz apart). The channel for a cycle is the
  (cycle mod n_allowed)-th allowed channel. Each POLL announces the mask for the *next* cycle.
  * The dongle measures loss per channel over 2 s windows. A channel losing more than 8 points
    above the median channel (and with at least 20 polls) is turned off for 30 s, doubling for
    repeat offenders up to 5 min, keeping at least 8 on. Judging against the median matters: a
    device with a weak link loses on every channel, and with an absolute threshold it struck
    clean channels until only 4 were left (firmware 0x0005 and older).
  * A device that misses polls keeps hopping on its own clock: `cycle = last + round(dt/period)`.
  * Ghost-follow (dongle only): a device that missed the POLL announcing a new mask or period keeps
    hopping on the old one, so while a slot misses polls the dongle polls it where that rule puts it,
    from what it sent in the device's last two answered POLLs (in turn with the cycle's own channel,
    within 1 s). Without it, such a device was lost for up to ~1 s, or rejoined. Cmd 0x32.
  * Home (cmd 0x36, RAM only, default all 16): the channels a dongle may hop on, at least 5 (8
    before 0x0030); bit k
    is hop channel k, 2404 + 5k MHz (0x00FF = 2404-2439 MHz, 0xFF00 = 2444-2479 MHz).
    Every dongle steps through its channels in the same order, so two dongles side by side whose
    cycles drift slowly against each other sit on the same channel for many cycles in a row and
    lose polls in runs. On disjoint homes (0x00FF / 0xFF00) they don't hop on the same channel.
    Two exceptions: the join channel (2480 MHz) stays shared, and for up to 1 s after a home
    change ghost-follow may still poll a device that missed the change on a channel of its old
    mask. Devices follow a home through the mask (any radio).
    Adaptive hopping inside a home (any home but 0xFFFF, a 15-channel one too): a new home starts
    with all its channels on and a fresh 2 s round. A channel is bad when it loses more than the
    typical channel (the lower median of the home's channels in use) + 8 points. Bad channels go
    worst first down to 4 on; below that (to 3) only one losing at least twice the typical loss.
    Two limits:
    - With more than half of the channels in use lossy, the typical channel is a lossy one itself:
      then no channel is far enough above it, or the typical loss is 20 % or more (the "loss
      everywhere" rule, as on all 16), and nothing is dropped. Example: Wi-Fi on channels 1 and 6
      covers 7 of 0x00FF's 8 channels, and that dongle keeps hopping on all 8. On all 16 it would
      still have the upper channels to hop on.
    - One 2 s round takes 8 channels down to 4 at most; 3 needs another round.
    Back to all 16: at least 8 on at once; strikes and time-outs carry over. A dongle never leaves
    its home, so pick homes from channels that are clean where the dongles are (`dongle.py scan`,
    then `dongle.py channels` for the loss per channel once it runs).
    The driver sets homes itself (src/transport/usb/doorman_homes.cpp): only dongles carrying a device
    share the channels out (one: all 16, two: 8 + 8, three: 5 + 5 + 6), and from their 0x27 reports a
    dongle left with fewer than 4 good channels takes a good spare from the one with the most, giving
    back its worst (one swap per 30 s at most). Told again on every open.
* Reliability:
  * Uplink: go-back-N, window 2, 3-bit seq; the POLL carries the ack.
  * Downlink: alternating bit (dseq/dack).
  * A device with no poll for 1 s goes back to joining.

### Packets
```
POLL  [01][slot][flags: ack 0-2, dseq 3, down 4][cycle][mask lo][mask hi][period/50us][max resp][frames]
RESP  [02][slot][flags: seq 0-2, dack 3][rssi -dBm][missed u8][crc_err u8][frames]
JOIN  [03][net base x4][flags: pairing 0][assign id x8][assign slot]
JREQ  [04][device id x8][flags]
NEED  [06][sat id][next cycle][mask lo][mask hi][period/50us][us to next start u16][listen u16][wanted u16][max len][flags][seqs]
SREP  [07][sat id][cycle][heard u16][-dBm per heard slot][records: [slot | 0x80 if older][n][RESP]...]
frames = repeated [len][frame]
```
NEED / SREP are dongle to dongle only (repeater dongles, firmware 0x0031; `doorman_link.h`), at base
`net base ^ 0x80808080`, prefix `0xC0 + sat id`. A repeater follows the main's hop and keeps the last two
RESPs it heard from each slot; after the device slots the main asks each repeater in use for the answers it's
missing (NEED) and takes the relayed ones (SREP) through the same path as direct ones, so it stays the only
one that acks and decodes. Relaying works with the selective ack (`DML_F_SACK`):
* An answer lost at the main and relayed in the same cycle is taken before the next POLL: the device never
  resends it, and the selective ack never comes into play.
* A slot the main holds an answer for (the one after a lost one) is still wanted: the repeater can send the
  lost one from the cycle before (record flag 0x80), and the main then passes on both in order.
* For each wanted slot the repeater sends the answer with the expected seq and, after it, the next one. If it
  only has the next one, the main keeps it as the held answer, and its POLL asks the device for the lost one
  only.
* A record from the cycle before fills the gap only: the main takes the device's hop timing, telemetry and
  downlink ack from answers to this cycle's POLL.

The main relays only while some device loses answers, as the main alone (what a repeater took back counts as
lost): on once one device's loss averaged over ~2 s reaches 4 %, off once every device's averaged over ~10 s is
under 2.5 %. A repeater that took back under a tenth of what was lost over 10 s (at least 50 lost) isn't asked
for 15 s. While it isn't relaying the main sends no NEEDs and its cycles are as without repeaters (idle
repeaters cost the devices nothing); the repeater keeps time from the main's POLLs and JOIN beacons. Late
answers are kept off while it relays (its cycles shift a little, and early POLLs doubled those devices' loss).

Automatic repeaters (dongle 0x0032). Every dongle starts as a normal one. With no device connected it
listens on the join channel for 22 ms once a second; hearing a JOIN from another main with devices (byte 22
bit 0x40) it becomes a repeater (RAM only). It's a dongle again after 15 s without hearing such a main, when
pairing opens on it (0x24), or when one of its own paired devices answers the short JOIN it sends every 100 ms
while it has pairings. Two dongles without devices both stay dongles. A repeater set by hand (0x3F with a main)
stays one; 0x3F with main base 0 makes one that picks its main itself.

A repeater on its own picks the main losing the most (JOIN byte 21), then the one with fewer repeaters, then
the strongest, and asks it for a place with an SREQ in reply to its JOIN. The main gives it a free repeater id
and names it in its JOINs (bytes 23-27, the auto repeaters in turn); no place within 2 s and it tries another
main (that one is skipped for 30 s). It sends the SREQ again every 4 s from the JOIN, with its status (serial,
board, firmware, how it hears the main, LED state), and the main drops it 12 s after its last SREQ or SREP.
Once a minute it moves to a main losing 10 points more than its own. None of this is saved to flash.

JOIN from 0x0032: 28 bytes. [21] worst device loss % as the main alone; [22] repeaters in use (bits 0-1),
0x40 devices connected, 0x80 relaying now; [23] an auto repeater's id | 0x80 (0: none), [24..27] its net base.
Radios take any JOIN of at least 21 bytes.

LED (link_led): relaying solid (green on RGB), attached and idle a slow 2 s pulse (orange), weak signal to
its main (-75 dBm or worse) a fast flash (orange), looking for a main a short blink twice a second (blue). All
LEDs are on PWM; the Holyiot and PCA10059 RGB LED is red P0.08, green P1.09, blue P0.12. Kuroshin and the
Index board drive no LED.

While relaying every cycle is longer by a fixed reserve: 220 us per repeater plus what a full SREP
needs beyond the join window, and the answer size is capped so the cycle stays within the 12.75 ms a POLL can
announce. Re-poll (0x2F) is skipped while repeaters are in use. A repeater has 7 addresses for slots: with
more than 7 devices the main names the 7 to listen to in each NEED, the ones that lost the most answers.

### Joining and pairing
* An unjoined device answers about half of the JOIN beacons with a JREQ carrying its radio
  chip's DEVICEID.
* The dongle sends the slot assignment in the next 5 beacons when the ID is already in its
  paired table (a flash page), or when pairing is open.
* Pairing target (firmware 0x0004): `pair SECS [SLOT+1]`. With a slot given, a device that was
  paired elsewhere moves there; with 0 or none given, it takes the first free slot. The target
  is saved and pairing closes.
* A device in pairing mode only joins on a beacon with the pairing flag set. For 35 s after a
  device is saved (firmware 0x0008), beacons assigning its slot keep the flag set: before, it
  ignored its own assignment until its pairing timed out (~35 s, 100% loss meanwhile).
* With pairing open, a device in pairing mode is paired whether it is new, already paired here or
  already in the target slot (firmware 0x000A). A device in pairing mode while pairing is closed here
  gets no slot assignment: it would ignore it, shown as connected at 100% loss.
* `forget SLOT|0xFF` removes devices from the table.
* The device starts pairing on its own pairing gesture (Tracker 1.0: hold the power button
  2 s; Index: the stock gesture). The main MCU tells the radio over SPI exactly as it does with
  the stock radio.

### Air codec (lossless, `doorman_codec.h`)
* Bit-packed, one frame at a time: no batching, no added latency. The radio codes, the dongle
  decodes, so USB carries the original frames.
* IMU `p` (18 B) -> `0xC1`: each axis predicted from the last two samples (2 x1 - x2), the
  timestamp from the last interval; seq only if it isn't previous+1.
* LH1 `q` -> `0xC2`: per record sensor (5 bits), b1 flag, width, time since the last record.
* LH2 `s` -> `0xC3`: per 0x28 word the ts24 delta and the 5-6 flag/sensor bits; channel bytes
  in 6 bits.
* Residuals use Exp-Golomb codes with k adapted per field (running mean of bit lengths), so big
  values (a new sweep) cost a few extra bits and nothing ever overflows.
* Both ends update their state from the original frames (coded or sent raw); reset on join.
* Measured on a 60 s Doorman recording (fast swings, 2 base stations) and Watchman LH2
  recordings: IMU 4.75 -> 2.1 kB/s, LH1 light 8.4 -> 5.2 kB/s, LH2 8-10.7 -> 77 %. The old
  byte-varint codec reached 77 % on IMU+LH1; this one 54-59 %.

### Frame check, batches and answer size (dongle 0x0028, radios 0x6A000025 / 0x0A000016)
* Agreed at join: the radio's JREQ flags say what it can (bit1 CRC-8, bit2 batches, Index-family
  radios only), the dongle's JOIN flags (for the assigned id only) say what to do. Old radios or
  an old dongle keep the plain codec.
* CRC-8 (poly 0x07, table-driven): ends each coded frame, over the original. A mismatch, or any
  frame the dongle can't decode, skips the slot 1.2 s so the radio rejoins and both ends reset;
  one corrupted frame that passed the radio's CRC-16 otherwise left the codec out of step for good.
* Batches `0xC7` (`doorman_lh2p.h`): every frame queued for one answer, `[count-1:3]`, per frame
  `[type:2]` + its walk (0 IMU, 1 LH1, 2 LH2 predicted, 3 raw), one CRC-8. The radio codes each
  frame into the open batch as it comes; building the answer closes it, sized to the answer the
  dongle announced. A frame that doesn't fit goes raw into the next (state already moved, the
  dongle observes it). The dongle decodes a frame a step and passes them on once the CRC passes.
* LH2 predicted: each hit from the same sensor and beam's last two rotations, each beam word's
  sync from the last two; only the differences are sent. Measured on two LH2 captures: all
  traffic -31 to -35 % at 8 devices (176 B answers, ~10 ms cycle), every frame rebuilt exactly.
* RESP flags bit4 "more": frames still queued after this answer. The dongle counts it per slot
  (older radios: answers within 70 B of the normal size); a device with it on half its answers
  over 2 s turns on large answers (250 B) until none is over a fifth for 10 s (`sizing.h`).

### Ten slots, wide batches, selective ack (dongle 0x002A, radios 0x6A000026 / 0x0A000017)
* `DML_SLOTS` is 10. Slot addresses are `DML_SLOT_PREFIX` + slot as before; radios before 0x6A000026 /
  0x0A000017 reject slots 8-9 in a JOIN, and the dongle fills the lowest free slot. The info reply (cmd 0x01)
  byte 60 is the slot count (0 from older dongles: 8); the driver, dongle manager, installer and tools use it.
  The pairing page moved to a 10-slot layout (magic "OHP1"); an 8-slot page ("OHPR") is converted at boot.
* The cycle goes out in one byte of 50 us (POLL p[6], JOIN p[18]), so at most 12.75 ms: answers are capped
  to keep it there (10 devices 188 B, 9 222 B, 8 or fewer unchanged). Longer cycles wrapped the byte and every
  device hopped on a wrong clock.
* Wide batches: JREQ/JOIN flag `DML_JF_BATCH32` (0x08, with `DML_JF_BATCH`) makes the radio send type 0xC8,
  the frame count in 5 bits (up to 32) instead of 0xC7's 3 bits. Eight frames capped a batch below a Tundra's
  frames per cycle once cycles were ~12 ms. The dongle decodes both.
* POLL flag `DML_F_SACK` (0x20): the dongle holds the answer that came after a lost one (seq ack+2) and
  delivers both in order when the lost one arrives; a radio that knows the flag resends only the lost one.
  Older radios resend both (the dongle drops the repeat).
* Radio queue: batches queue up to 4 answers' worth; over that, light is dropped and IMU may still use the
  whole 1 KB. An answer closes the open batch only when nothing closed is waiting.
* Light is also dropped once the oldest queued batch is older than 3 cycles (25 ms at least), so a lossy
  link can't build a backlog that delays everything behind it; IMU always goes.

### Late answers (radios 0x6A000029, dongle 0x0030)
* An Index-family radio can build its answer just before the POLL it expects instead of right after
  the last one (air `0xF0 'A' [1 on / 0 off / 2 just ask]`, replying `[on][lead u16]`): the answer
  carries newer data, by up to a cycle. A POLL that comes earlier than it expected gets no answer.
  Radios boot with them on; HTC radios keep them off (they cost those 4-5 points of loss).
* The dongle decides: on while cycles are 8 ms or longer, off below 7 ms, each held 2 s before it
  switches, and sent again to every device that joins (one that rebooted has them on). Measured with
  6 devices: at ~5 ms cycles they cost clean devices 1-1.5 points of loss and saved no delay; at
  ~9 ms about even. A radio whose version the dongle doesn't know yet is asked with `0xF0 'X'`.

### Feature reads and drop counts
* The host reads a feature with `[0xC5][tag][id]`; the radio asks the main MCU (`P id`) and
  answers `[0xC4][tag][answer]`. A late answer can't be taken for the next request. Untagged
  `P` reads (tools) still pass through.
* Once a second the radio sends `D [queue drops u16][reply drops u16]` (totals). The dongle
  keeps them per slot (cmd 0x28 bytes 12-19) and doesn't pass them on.

## Dongle USB interface (VID:PID 6767:0727, "Doorman Dongle")

**Five HID interfaces** (firmware 0x002B; four, endpoints 0x01/0x81 .. 0x04/0x84, from 0x000D),
64-byte reports, 1 ms interval. Interface 0 has IN 0x81 and OUT 0x01; interfaces 1-4 only send
(IN 0x82 .. 0x85): the host writes on interface 0 only, and six endpoints instead of ten leave room in
a USB 2.0 hub's periodic budget (eight on a hub chip shared with other full-speed devices failed with
Code 10). **Slot s streams on interface s % 5** (s % 4 on four-interface firmware); interface 0 also
carries every command and reply (replies go first). Hosts take the count from the USB bcdDevice
(the firmware version): five from 0x002B. An interface tops out at 59.7 kB/s of stream (measured
with the dongle's test load); a Tracker 3.0 on 2.0 base stations streams ~18 kB/s, so ten devices on
four interfaces put three on two of them, ~90 % full: those slots dropped 6-48 frames/s and their IMU
queued 28-43 ms (median). Five keep every interface at two. One interrupt IN endpoint sends at most one 64-byte report
per 1 ms frame (~61 kB/s). With two interfaces the even slots' endpoint ran near that limit with
five devices, and their data waited in the dongle: a controller in slot 2 arrived 12-27 ms older
at the median (p90 31-61 ms) than one in slot 1. With four, at most two devices share an
endpoint (both controllers then 5-6 ms median, p90 ~11.5 ms). Hosts open all of them (grouped by USB
serial) and read them all: the driver (`DoormanDongle::devs_`), the dongle manager, the installer
and `doorman_link.py`. Older two-interface firmware isn't supported.

* Commands go out on interface 0 as `[cmd][seq][args]`. Replies come back as
  `[cmd][seq][status][61 B data]`. Every program on the dongle sees every reply, so seq ranges
  are split: **the driver uses 1..127, tools (dongle manager, installer, scripts) 128..255**.
  Reusing one range let the driver take the manager's slots-6-7 live reply as its own and drop
  every device.
* Stream reports are `[0x80][slot][n][whole [len][frame] items]`. Every report stands alone,
  so readers never desync.
* A frame too big for one report (feature replies, up to 64 B) comes as consecutive
  `[0x81][slot][n][raw bytes of [len][frame]]` fragments. A `0x80` report for the same slot
  discards an unfinished one.
* Command replies can be interleaved with stream reports. Keep the stream reports, don't drop
  them.

| cmd | meaning |
|---|---|
| 0x01 | info: magic, fw version, variant (sd/nosd), WDT, app base, RESETREAS, uptime, device id, crash record, quick crashes, SoftDevice present |
| 0x02 | USB counters |
| 0x03 | echo |
| 0x10 / 0x11 | enter the UF2 / serial DFU bootloader |
| 0x12 | reset |
| 0x13 | crash test (deliberate HardFault) |
| 0x14 | clear the crash record |
| 0x20 | link on/off (0xFF = query) |
| 0x21 | downlink `[slot][len][frame]` |
| 0x22 | RSSI scan of 2400–2500 MHz (pauses the link) |
| 0x23 | per-slot stats (15 u32) |
| 0x24 | pair `[secs][slot+1]` |
| 0x25 | slots `[first]`: 10 B per slot for slots first..first+5 (0 and 6): used, active, id x8. `d[60] = pairing \| (target+1) << 1`. The driver reads it every second: a slot whose id changes (re-paired to another device with no gap) is identified afresh |
| 0x26 | forget slot (0xFF = all) |
| 0x27 | channels: mask, loss % per hop channel, joins, net base |
| 0x28 | codec: air bytes, frame bytes, bad frames; the radio's queue drops, reply drops (u32 each); bytes 20-21: most bytes waiting for USB since the last read (u16); 22-25: test-load bytes queued (u32); 26-29 codec resyncs (u32), 30 CRC-8 on, 31 batches on |
| 0x2B | test load `[slot mask][kB/s]`: filler `'Z'` frames queued for USB on those slots (empty ones too), as if devices sent them. `0 0` stops it. `doorman/tools/load.py dongle` |
| 0x2C | USB inside radio exchanges `[bits, 0xFF keeps]`: 1 while the POLL goes out, 2 while listening, 4 between decoded frames. Default 3 (4 delays the next POLL: loss ~18%). Timing modes 0/1 only |
| 0x2D | timing mode `[0, 1, 3; 0xFF keeps]` (other values: status 2): 0 fixed slots, 1 adaptive slots, 3 (default, firmware 0x0031) adaptive slots with each POLL started by a timer compare, at most 64 B of downlink per POLL; reply period, slot, slot budget, exchange avg/max (u32 us), mode requested, max resp, decode avg/max (u32 us at 22/26); 0x0028+: 30 worst saturation %, 31 large answers, 32 normal answer size, 33 devices in the plan, 34-43 saturation % per slot; 0x0031+: 44 mode in effect; counters reset on read. Older firmware takes only 0/1 (adaptive off/on) |
| 0x2E | deferred decode `[0/1, 0xFF keeps]` (default 1) |
| 0x2F | re-poll missed slots in the same cycle `[0/1, 0xFF keeps]`, then re-polls and answered (u32). Default 0 |
| 0x32 | ghost-follow `[0/1, 0xFF keeps]` (other values: status 2): on (0), ghost polls sent and answered (u32 at 2 / 6, since start). Default 1 |
| 0x33 | rejoin `[slot]`: the slot is not polled (for 1.5 s at most), so its radio rejoins (status 2: not paired). The host sends it when a connected device has sent nothing for 6 s |
| 0x34 | set a slot's name `[slot][len][UTF-8, up to 59 B]`, len 0 clears (status 2: not paired or too long). Kept in flash with the pairings; forgetting or re-pairing the slot clears it |
| 0x35 | a slot's name `[slot]` -> `[len][UTF-8]` |
| 0x36 | home (fw 0x002F+) `[lo][hi]` (0 only reads; 0xFFFF = all 16, the default; fewer than 5 channels, 8 before 0x0030: status 2, nothing changed): home u16, mask in use u16, channels kept on, fewest for a clearly bad channel. RAM only: a replug or restart clears it. Older firmware: status 1 |
| 0x38 | radio versions (fw 0x002C+) -> `[n][u32 LE per slot]`: each paired device's radio as it last answered `0xF0 'X'`, kept in flash (0 unknown) |
| 0x39 | set a slot's remembered radio version (fw 0x002D+) `[slot][u32 LE]` -> `[slot]`: the installer, after updating a device it then turns off. A committed radio update (`0xF0 '['` status 0 on its way up) clears it to unknown |
| 0x29 | live `[first]` (6 slots per reply, as 0x25): per slot `[used\|active<<1][rssi last rx][rssi last tx][min][max][channel][ms since reply u32]`. Min/max reset on read |
| 0x2A | identity: the slot's last `H` frame (16 B, zeros if none) |
| 0x3C | main: repeaters in use `[mask, 0xFF reads][1: also save to flash now]` -> `[mask in use][mask saved]`. Applied in RAM; saved with byte 3 = 1, or by the dongle once no device is connected (a page erase halts the radio ~85 ms). Status 2 on a repeater |
| 0x3D | repeater status `[page]`: 0 `[role running][synced][sat id][channel]`, u32 from 4: cycles, NEEDs, SREPs; `[main -dBm at 16]`, from 0x0032 `[17 main followed u32][30 LED state]`. 2: mains heard, 8 x 7 B from 1 (base u32, -dBm, loss %, JOIN byte 22). 1: per slot RESPs heard (u32 x10 from 1), -dBm (x10 from 41). Totals |
| 0x3E | main: relay stats `[page][first]`: 0 per repeater (16 B from 1+16k: asked, SREPs, taken, ms since its last SREP; u32), `[33 mask][34 reserve us u16][36 relaying now][37 s before a useless repeater is asked again][38 auto repeaters
mask][39 and 43 their net bases u32][52, 53 -dBm the main hears each repeater at]`. Page 2 (0x0032): per auto
repeater 20 B from 1+20k: net base u32, SREQ bytes 9-21 (device id x8, board, firmware u16, -dBm of the main,
LED state), -dBm at the main, 0.1 s since its last SREQ u16. 1 `[first 0-4]`: 6 slots of 9 B: relayed, taken (u32), -dBm at the repeater. Totals; 0x23 counts an answer relayed in its own cycle as an answer |
| 0x3F | role `[0/1, 0xFF reads][sat id 0-1][main net base u32]` -> `[role][sat id][main base u32][own net base u32][mask in use][role running]`. 1: a repeater of that main (kept in the pair page after the radio versions, applied at once; status 2 for an invalid base or its own). A repeater refuses 0x21 and 0x24. Firmware without repeaters: status 1 |

## Device frames (uplink, inside RESP)

| Frame | Meaning |
|---|---|
| `p` + 17 B | one IMU sample, same as a wired IMU report slot |
| `q` + n×8 B | light records, same as the wired light report |
| `H` `[1][device_id u32][gyro range][accel range][hw_version u32][fw build u32]` | identity, on every link-up; serial = `LHR-%08X` of device_id |
| `B` `[percent \| charging << 7]` | battery, every 5 s and on link-up |
| `P` + bytes | reply to a downlink `P` |
| `0xF0` ... | the radio's own updater (`radio-fw.md`) |
| `0x52` / `0x53` | link up / down (also reported to the main MCU over SPI) |

Downlink frames (`0x21` command):
* `P [id]` with id < 0x80 reads feature report `id` (the reply is `P` + its bytes, without the
  id). `P [id][data]` writes it.
* `P [cmd][len][data]` with cmd ≥ 0x80 runs a Valve-style device command:
  * haptic 0x8F;
  * **power off 0x9F with `off!`**: `P 9f 04 'o' 'f' 'f' '!'`. Our tools power devices off
    after an update instead of rebooting them; the user turns them back on;
  * host reboot 0x95.
* `0xF0` frames: radio updater, handled by the radio itself (never reaches the main MCU).

The **sensor type** (the hardware ID's low byte, e.g. tracker 4, Index 9) decides how openhouse
decodes OOTX. A stock-LPC device doesn't put it in its config, so the host reads feature 0x05
(byte 41) over the downlink.

## Dongle firmware 0x0010-0x0018: throughput and latency (measured with six devices)

* **Replies are never crowded out.** A slot's USB queue keeps its last 256 bytes for replies (`P`,
  `H`, tagged 0xC4, radio-update 0xF0): IMU, light and filler may only fill the rest. With the queue
  full, config and update replies used to be dropped like data; config reads then failed and were
  retried for minutes (eight 3.0 trackers on one dongle), and air updates stalled.
* **USB serviced during radio exchanges** (while the POLL goes out and while listening): each interface
  went from ~470 to 1021 reports/s (the full-speed limit) with the link busy. Measured with the link
  off: 1021/s, so the radio code had been starving USB.
* **Answers decoded off the critical path**: kept per slot and decoded one frame at a time while the
  radio receives the next answer (and in idle time); a slot's answer is always finished before it's
  polled again. Exchanges 912 -> ~620 us.
* **Adaptive slots**: each slot's share is the 90th percentile of the last 32 cycles' exchanges plus
  120 us, bounded by the old worst case (450 + 4 x max resp). Six devices: ~160 -> 164-168 polls/s.
* **Load tests** (`doorman/tools/load.py`): dongle filler (0x2B) loads queues, USB and the host;
  radio filler (air op `'L'` [kB/s], radio 0x6A000020 / 0x0A000011) loads the air. With six devices
  at ~23 kB/s each (air saturated) plus 20 kB/s on slots 6-7, all six read their configs in 0.55-1.3 s
  with 0 drops; dongle 0x000D under the same load: 19 failed config reads in a minute, 170-300 drops/s.

