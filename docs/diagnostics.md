# Diagnostics reference

The left half keeps a journal of Bluetooth, USB, boot and power events in RAM
and in rotating flash blocks. This page explains how to read it and what each
event code (`k=...`) means. The daily-driver behavior doesn't depend on any of it.

## Reading the journal

**Host event log:** enabled on the production left image. Recent events are kept in RAM and copied into rotating, CRC-checked settings blocks. Writes are coalesced and rate-limited; profile-change callbacks do not write flash directly. With the Studio USB port connected, run `python tools/read_diag_dump.py /dev/cu.usbmodem101` (requires `pyserial`) to request a `D1` dump. The reader accepts it only when every numbered line and the matching end marker arrive. Replace the port path for your system. The `[` + `X` combo also requests a dump; it does not clear saved logs.

The production RAM history holds 1,024 events (previously 128), preserving more
of a handoff before a USB dump. This adds 17,920 bytes across the ring and its
static dump snapshot. The flash journal remains eight 16-event blocks with a
10-second minimum write interval; its format and existing saved logs are unchanged.
The extra RAM history is lost on reset unless already copied into the journal;
RAM and flash histories can overlap and their capacities must not be added as
unique events. Retention is event-based, not a guaranteed number of minutes.

Boot-only diagnostics add `k=40..46` without changing
the journal format or transport policy. Each USB dump also prints `totem_boot`
summaries, even when the rolling ring has overwritten the boot events. `raw` is
the nRF52840 reset register captured at PRE_KERNEL_1 (after the bootloader);
`read` is the signed hwinfo API result. `ram=1` means the retained boot marker
survived; it does not prove that all RAM survived. `fault=0/1/2/3` means absent,
bad magic, bad CRC, or valid crash/watchdog record. Zero raw flags or an absent
record cannot rule out power loss or bootloader clearing.

The CRC-checked `tboot/count` counter is saved once per boot through the existing
serialized diagnostic writer, never at early init or in an interrupt. A boot
that loses power before this write may not increment the saved count. `counter`
is 0 on first installation, 1 for a valid restored counter, or 2 for an invalid
value. Previous uptime/profile/endpoint describe only the available saved journal
tail, not the exact failure instant; profile/endpoint are -1 if not retained.
Boot evidence joins the usual rate-limited rotating journal. No new per-key or
per-radio-event logging is enabled.

Heartbeat `k=47` adds a power sample to the journal one minute after boot and then
every `CONFIG_TOTEM_DIAG_HEARTBEAT_MIN` (default 30) minutes: `i` selected profile,
`a` right SoC (-1 unknown), `r` left SoC, and `w`/`x` the low/high bytes of a 16-bit
word whose bits 0..14 are battery mV and bit 15 is USB power. After an unexplained
reset, the last heartbeat bounds when it happened and shows whether the battery was
sagging. A reset with `raw=00000000`, `ram=0` and a falling heartbeat voltage points
to power loss; the bootloader preserves RESETREAS (a pin reset reads `00000001`).

Timing diagnostics on the left half use the same uptime clock: `k=26/27` mark
settings-write entry/return (match `r` category and `w` pair ID), `k=28` marks
controller disconnect reception before host cleanup, and `k=1` is the later
application callback. For `k=28`, reconstruct the handle from the unsigned low
`i` and high `a` bytes; `k=29` maps handles to profiles using low `r`/high `x`.
`k=30` reports lost timing records. Timing capture never prints or writes flash
in the controller path. These records retain their original timestamps when
drained later, so sequence order need not be timestamp order. They join normal
rolling saves but do not request saves themselves; a flash write does not cause
a chain of diagnostic-only writes. Only the selected settings categories are
logged, not setting contents or keystrokes.

Handoff-only controller diagnostics (`k=33/34/35`) use unsigned `i/a` as the
connection handle, mapped to a profile by `k=29`. Parameters (`k=33`, `r=0/1/2`)
are the negotiated interval (1.25 ms units), latency and supervision timeout
(10 ms units), encoded as `w | (x << 8)`. Termination (`k=34`) stages are
0 disconnect request (`x` requested reason), 1 API return (`w` error class,
`x` absolute result), 2 termination control packet queued, 3 link-layer ACK,
4 confirmed local termination timer expiry (`w=0`, `x` controller reason).
Stage 4 observes `ull_cp_prt_elapse` returning `-ETIMEDOUT` for the local
`PROC_TERMINATE` timer during an armed handoff; other procedure timeouts and
remote timer expiry are excluded. The pinned controller can report local
disconnect (`0x16`) after either an ACK or termination timer expiry, so the
disconnect reason alone cannot distinguish them. The observer passes through
the original return value, output reason and timer updates without changing them.
The termination timer uses the negotiated supervision timeout (`k=33,r=2`).
Link supervision expiry is a separate earlier controller path, reported as
`k=28,r=8` (`0x08`); it does not emit stage 4. Absence of stage 4 alone is not
proof of an ACK: use the explicit stage 3 marker and check for dropped records.
Queuing is not proof of radio transmission. Radio summaries (`k=35`, `r=0/1/2/3`)
count completed connection events, events without valid non-aborted RX, aborted
events and skipped events; counts saturate at 65535. Stages 4/5 carry the last
valid RX event timestamp before the request/at disconnect (`x=1` known;
`x=0` unknown, timestamp is the snapshot instead). Stage 6 captures the first
event without valid RX during handoff (`x` aborted flag). Valid RX includes empty
link-layer packets, not necessarily keyboard data; skipped events include normal
peripheral latency, and aborted events do not by themselves prove radio contention.
Additional `k=35` summaries use `w | (x << 8)`: `r=7` sums the controller's
completed receives (`trx_cnt`, including invalid packets; not TX or ACK count),
`r=8` counts non-aborted events with no completed receive, and `r=9` counts
non-aborted events with a receive but no valid RX. The latter does not identify
CRC corruption alone: encryption rejection can also prevent valid RX.
`r=10/11` count events whose controller MIC status reports pass/fail; these are
event summaries, not authenticated-packet counts. MIC_NONE means no reported
integrity result, not a failed check; empty packets need not produce MIC_PASS.
MIC counters require `CONFIG_BT_CTLR_LE_ENC` (enabled on this build); without it
they remain zero/unavailable. All counts saturate at 65535 and reset per handoff.
No completed receive does not by itself distinguish peer silence from RF loss
or a missed receive window. The receive observers read existing event-done
metadata and never change it.
On nRF52840, handoff-only termination TX summaries (`k=39`) separate radio
packet setups (`r=0`), observed hardware ENDs (`r=1`), and setups canceled or
replaced without END (`r=2`); counts use `w | (x << 8)` and saturate at 65535.
Stages 3/4/5 retain first setup, first END and last END timestamps (`x=1`
known, or `x=0` unknown with snapshot time). Multiple ENDs indicate repeated
transmissions of the termination packet; multiple setups alone do not.
The observer associates the shared radio with an armed peripheral connection
and inspects only the termination control opcode before encryption. It never
retains key material or HID payloads. END is read before the controller clears
it, including the private final-TX/done path; canceled setups and later RX ENDs
are not counted as successful TX. END confirms local radio completion, not
reception by the computer or acknowledgement (use `k=34,r=3` for ACK).
The radio interrupt only updates RAM counters/timestamps; it does not print,
submit log work or write flash. Summaries are captured at disconnect. The
timing inbox holds 24 records so the expanded disconnect burst fits without
draining inside the controller. Existing radio callbacks, packets, return
values and hardware-event clearing are passed through unchanged.
Only known non-selected host disconnects with USB unplugged arm these observers.
Radio activity is accumulated in RAM, not logged per event; bounded captures
drain through the existing timing inbox and rotating journal. Disconnect behavior
is unchanged. These private hooks target the pinned Zephyr software controller.

Security timing uses `k=36` at encryption-event arrival in the host RX queue
(`r=0`) and dequeue before processing (`r=1`). Encryption Key Refresh events
use `r=2/3`. Unsigned `i/a` encode the handle; `w` is HCI status, `x` the
encryption-enabled byte (255 for key refresh). This is host receipt, not the
controller's on-air completion time. Split encryption events are included so
the receive hook does not need a connection lookup. Match host handles to `k=29`.
`k=37` records LE host security processing entry (`r=0`), after L2CAP encryption
callbacks (`r=1`) and return (`r=2`); `w` is HCI status, `x` security level.
Compare these with application security success/failure (`k=4/5`) to distinguish
negotiation, RX queue, L2CAP and application-callback delays. The hooks do not
change security policy, buffer contents or ownership. Only timing and compact
status metadata are retained, never keys, addresses or packet bodies.

Security exchange timing (`k=38`, unsigned `i/a` = handle low/high) records
SMP Security Request submission (`r=0`) and return (`r=1`, `w` error class:
0 success, 1 negative errno, 2 positive result; `x` absolute result).
Controller control-PDU processing (`r=2`) and TX queuing (`r=3`) retain only
the encryption opcode in `w`: 3 ENC_REQ, 4 ENC_RSP, 5 START_ENC_REQ,
6 START_ENC_RSP. These are controller processing/queuing boundaries, not
on-air timestamps or proof that the peer received a packet. Key reply entry
(`r=4`) and return (`r=5`, `w` HCI result) use `x=1` for a positive reply,
0 for a negative reply; the key itself is never read by the observer.
Host LTK Request arrival/dequeue use `r=6/7` (including split events without
a connection lookup). Match reused handles against the latest `k=29`.
These markers separate waiting for a host encryption request, host key lookup,
and the subsequent controller handshake. Their absence is not proof of no
radio activity; check timing-inbox overflow (`k=30`) and retention first.

HID pipeline summaries (`k=31`) are coalesced into five-second activity windows.
`i` is the profile observed at that stage, `r` is the stage below, and
`w | (x << 8)` is a cumulative 16-bit count, wrapping at 65536 and resetting on
boot. `t` is the last observation time; `k=32` adds the last positive errno in
`w` for failed stages. Stages: 0 logical release accepted, 1 release failed,
2 queue accepted, 3 queue put failed (including full/retry), 4 worker dequeued,
5 producer evicted a report, 6 dequeued report dropped without a host,
7 notification API accepted, 8 notification API failed, 9 no notification
subscription at attempt, 10 all-up queued, 11 all-up notification API accepted,
12 all-up evicted, 13 all-up notification failed. All-up means zero keys and
modifiers; it does not cover every individual release. Logical releases include
synthetic releases, not raw physical switch events. Queue stages use the active
profile at observation, not a stored destination. API acceptance is not proof of
host delivery. No key identities or report contents are retained. Summaries join
the existing rate-limited journal; the latest unflushed RAM counts can be lost
on a reset. Queue, retry, security and notification behavior are unchanged.
