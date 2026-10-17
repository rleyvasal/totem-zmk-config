# Moving zmk-totem into zmk-next: inventory and plan

Written 2026-10-16. Nothing has been pushed or opened. Every step below needs approval.

Goal: totem-zmk-config pins `rleyvasal/zmk-next` directly and `rleyvasal/zmk-totem` is retired.
Rule: each feature goes into zmk-next behind Kconfig, off by default, so a ZMK Next user who sets
nothing gets today's zmk-next behavior.

## What zmk-totem actually carries

Compared against zmk-next `main` at `9036dec4` (GitHub; the local `~/zmk-next` has not fetched the
last 8 workflow-only commits) and zmk-totem `totem` at `b7a00d5c`:

- **29 non-merge commits** and **3 merge commits** are on `totem` and not in zmk-next main. The
  earlier estimate of "about 129" counted the runtime-config history that is already in zmk-next.
- **No upstream ZMK sync commits remain.** The sync merges (`dd819daa` Merge tag v0.2.1,
  `43e7a0ca` and `21b0d567`, zmk-totem's own PR merges) bring in nothing that is missing from zmk-next.
- zmk-next main is 8 commits ahead of `totem`. All 8 are Totem workflow commits that zmk-next has
  since moved out (PR #16 to #19), so they don't conflict.
- Net app diff: 43 files, +4368 / −136. `ble.c` alone accounts for +1370.

### The main problem: `ble.c` reads Totem's own Kconfig

`app/src/ble.c` on `totem` tests **15 `CONFIG_TOTEM_*` symbols** (`TOTEM_ADV_THROTTLE` ×25,
`TOTEM_ADV_BOOST`, `TOTEM_DIR_THEN_OPEN`, `TOTEM_IDLE_DISCONNECT`, `TOTEM_ACTIVE_ADV_FILTER`,
`TOTEM_RESELECT_RECONNECT`, `TOTEM_EVICT_ADV_COOLDOWN_MS`, `TOTEM_EXCLUSIVE_HOST`,
`TOTEM_EXCLUSIVE_DISCONNECT_REASON`, …). These symbols are defined only in totem-zmk-config's `Kconfig`.
In zmk-next they would be undefined, so `IS_ENABLED()` is 0 and the code would be dead. Two of them,
`CONFIG_TOTEM_EXCLUSIVE_DISCONNECT_REASON` and `CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN`, are used as
values, so any build that turns the feature on would fail without them. ble.c also exports a
`zmk_ble_totem_*` API that the config's modules call. These need new names and real Kconfig
definitions in zmk-next.

### Ungated behavior changes that would reach every zmk-next user

As written today, these change behavior with no Kconfig set:

| Where | Change | Who is affected |
|---|---|---|
| `endpoints.c` (`fd192e03`) | USB preferred + VBUS present + HID not ready → `ZMK_TRANSPORT_NONE` instead of falling back to BLE | **Anyone charging from a power bank or a charge-only cable: no typing at all.** |
| `ble.c` (`d6bc4563`) | Split central on VBUS: host advertising forced off, the profile switch deferred 50 ms, re-select skipped | Every split keyboard on USB power |
| `battery.c` (`fd192e03`, `106c7880`, `ef07c750`, `b7a00d5c`) | On VBUS, report the remembered cell level, not the live reading. Writes `zmk/batt/cell` to flash. | Every battery keyboard with USB |
| `Kconfig` (`455194e8`) | `ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` default 768 → 1536 on USB+battery+settings | +768 B RAM for most wireless keyboards |
| `split/bluetooth/central.c` (`a87562a6`) | Disconnected peripheral keeps its last battery level instead of 0 | Split users, including anything that shows 0 as "disconnected" |
| `studio/rpc.c` (`dfdcdef3`, `31be6fdc`) | Frames starting with `C`/`D` are diverted to raw handlers | All Studio users (low risk: protobuf never starts with these bytes, but still a protocol change) |
| `xiao_ble_zmk.dts` (`ef07c750`) | Chosen `zmk,battery-charger` node (CHG pin P0.17 removed, see P3) | All XIAO BLE users; harmless if battery.c only acts when the feature is on |

Plan: every row gets a gate, except the XIAO node. It describes the hardware and stays in the board file.

## 1. Inventory

Labels: **G** = generic, belongs in zmk-next. **T** = Totem-specific, belongs in totem-zmk-config.
**Fix** = generic bug fix that needs no gate. **Drop** = nothing to move. "PR" refers to section 2.

| # | Commit | Subject | Label | PR | Why |
|---|---|---|---|---|---|
| 1 | `9c4a396d` | Integrate Totem BLE reconnect support | **G**, after renames | P6 | Advertising throttle, boost, directed-then-open, active-profile accept-list filter, re-select reconnect, idle disconnect, exclusive-host handoff, plus the RPA-safe `zmk_ble_profile_is_connected()` fallback. Nothing in it depends on Totem hardware; any multi-host keyboard benefits. It is gated by `CONFIG_TOTEM_*` symbols that zmk-next doesn't define, so these become `CONFIG_ZMK_BLE_*` symbols and the `zmk_ble_totem_*` API is renamed. The RPA fallback is a **Fix**. |
| 2 | `34ff30a3` | Initialize Studio keymap reset tracking | **Drop** | none | Already in zmk-next main (`= {0}` is there). Found while cutting P1. |
| 3 | `b46c97fa` | Document generic ZMK Next integration | **Drop** | none | `docs/quick-start.md` is already in zmk-next main with the same content. The README paragraph can go into P1 if it is still wanted. |
| 4 | `bdcf24d0` | Harden BLE pairing and connection tracking | **G** + **Fix** | P2 / P1 | Passkey entry rewrite (no ring buffer; Enter submits exactly 6 digits, Backspace, Escape cancels; `pairing_failed` callback), plus a bsim test. It is already behind `CONFIG_ZMK_BLE_PASSKEY_ENTRY` (default n), so it needs no new gate. The connected-state/role check in `zmk_ble_profile_is_connected()` is a **Fix**. |
| 5 | `3a9990fc` | Do not advertise to computers while the central half is on USB | **Drop** | none | Reverted by #6. Net zero. |
| 6 | `e4e2188e` | Revert "Do not advertise…" | **Drop** | none | See #5. |
| 7 | `fd192e03` | Keep USB HID while cabled and publish last-cell battery | **G**, split up | P5 + P3 + P7 | Three features in one commit. (a) No BLE fallback while cabled: the macOS problem is generic, but this is a policy and must be gated. (b) Last-cell battery while on USB (battery.c, BAS proxy): generic, gated. (c) Studio CDC log mux: superseded by `31be6fdc`. The `set_preferred_transport` "always re-evaluate" change only matters together with (a), so it moved to P5. |
| 8 | `d6bc4563` | Suppress host BLE while USB HID is active | **G**, gated | P5 | nRF52 radio scheduling between USB HID and host BLE is not Totem hardware. Today it only runs on a split central. Gate it as `ZMK_BLE_HOST_ADV_OFF_ON_USB`. See section 3. |
| 9 | `dfdcdef3` | Accept diagnostic control frames | **G** (fold) | P7 | `D` prefix for raw frames. `31be6fdc` replaces this mechanism, so squash it into that. |
| 10 | `a87562a6` | Retain split battery level across disconnects | **G**, gated | P3 | Upstream sends 0 on disconnect, and some displays and hosts may read that as "gone". Gate it as `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN`. |
| 11 | `1011953a` | Fix battery and Bluetooth documentation formatting | **G** (fold) | P2 / P3 | Prettier reflow of `bluetooth.md` and one battery.c comment. Fold each part into its PR. |
| 12 | `6848616d` | Make diagnostic RPC transmission non-blocking | **G** | P7 | Raw TX must never block the keymap thread. Comes with test `studio/raw_tx`. |
| 13 | `d42fe223` | Reject stale BLE connections before sending HID reports | **Fix** | P1 | `zmk_ble_active_profile_conn()` returned a disconnected `bt_conn`, so reports were lost. Test `ble/active_connection`. |
| 14 | `455194e8` | Prevent settings saves from blocking Bluetooth startup | **G** + gate | P3 (+ hook in P1) | Weak `zmk_settings_loaded()` hook after `settings_load()` (generic; inert by default, goes in P1). Battery saves offloaded to the low-priority queue. The 1536-byte stack default must only apply when the battery feature is on. Tests `settings/startup`, `battery/persistence`. |
| 15 | `4d476144` | Avoid restarting advertising for unchanged BLE profiles | **Fix** / P6 | P6 | Link-state notifications for the same profile no longer restart advertising. This is a fix, but it only matters together with the P6 advertising worker, so it ships there. Test `ble/profile_notifications`. |
| 16 | `d0a9e378` | Discover only simulator cases in BLE test workflow | **Fix** (CI) | P1 | `ble-test.yml` now finds cases by `nrf52_bsim.keymap`. Needed once tests that aren't bsim live under `app/tests/ble`. |
| 17 | `d54734f2` | Serialize BLE advertising and coordinate profile-switch handoff | **G** | P6 | Single advertising owner (work-queue worker, versioned target, spinlock) plus the exclusive-host handoff (disconnect the old host before advertising). The worker removes HCI races and is generic. The handoff is a policy and gets a gate. Test `ble/advertising_owner` (390 lines). |
| 18 | `25907404` | Preserve managed keyboard CCC handling with subscription diagnostics | **G**, gated hooks | P6 | Weak `zmk_hog_*` observer hooks, a CCC write callback and notify completion. Totem's `hid_connection_log.c` and `hid_report_diagnostics.c` use them. Put them behind `ZMK_BLE_DIAGNOSTIC_HOOKS` so hog.c compiles exactly as before when off. Test `ble/keyboard_ccc`. |
| 19 | `bfb2f582` | Describe the Totem patches in the README | **T** / Drop | none | zmk-totem's README. It retires with the fork; its content belongs in this repo's `STACK.md`. |
| 20 | `429d7242` | Apply clang-format to the managed keyboard CCC definition | fold | P6 | Squash into #18. |
| 21 | `ebb7faff` | Register a cancel callback in the BLE test central's passkey handler | **G** (test) | P2 | Test fix for #4. |
| 22 | `01a193c7` | Match the passkey test's log lines with extended regex syntax | **G** (test) | P2 | Test fix for #4. |
| 23 | `31be6fdc` | Replace the Totem Studio log hooks with generic raw frames | **G**, gated | P7 | `zmk_rpc_send_raw_frame()` and the iterable `ZMK_RPC_RAW_FRAME_HANDLER` (the same API as zmk-studio-log's `patches/0001-studio-raw-frames.patch`). Gate it as `ZMK_STUDIO_RPC_RAW_FRAMES`. Its UART pieces are **Fix**es for P1: the RX re-arm in `refresh_selected_transport()` (XIAO USB boot race), break instead of spin on a full TX buffer, TX irq disable when empty. The exported `zmk_studio_uart_rearm()` stays with P7. |
| 24 | `106c7880` | Keep the remembered cell level when booting on USB | **G** | P3 | Part of the last-cell feature. Test `battery/usb_boot`. |
| 25 | `e1b99c0e` | Report the peripheral's charging state over the split link | **G** | P3 | `ZMK_BATTERY_CHARGE_STATUS` (already default n) uses BAS Battery Level Status 0x2BED and the new `zmk_peripheral_battery_charging_changed` event. Test `battery/charge_status`. |
| 26 | `fc522b17` | Slow the split link while the keyboard is idle | **G** | P4 | `ZMK_SPLIT_BLE_IDLE_INT` (default 0, off). Test `split/idle_interval`. |
| 27 | `753ade55` | Add an idle peripheral latency for the split link | **G** | P4 | `ZMK_SPLIT_BLE_IDLE_LATENCY`, only used when IDLE_INT > 0. |
| 28 | `ef07c750` | Report charging, full and an estimated level while on USB | **G** | P3 | `zmk,battery-charger` binding, XIAO charger node, charge estimate, `ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC` (default 0). Test `battery/charge_tracking`. The Totem part (`charge-full-design-microamp-hours = <110000>`) is already in `boards/shields/totem/totem.dtsi`. |
| 29 | `b7a00d5c` | Report 100 only after a charge completes | **G** | P3 | Refinement of #28. |

**Count:** 0 upstream sync · 20 generic or fix (several split across PRs) · 4 fold into another commit ·
3 drop · 1 Totem (README) · plus the 3 merges.

**Nothing in the 29 commits is Totem hardware.** The Totem-specific parts are the *values*:
`totem.conf` (idle interval 48 with latency 4, settle 300 s, throttle 5 min, boost 20 s, disconnect
reason 0x13…), the 110 mAh capacity in `totem.dtsi`, and the config's own modules in `src/`
(journal, watchdog, reconnect watch, exclusive host, lazy host, …). Those already live in totem-zmk-config.

## 2. Themed PRs to zmk-next

Order: P1 → P7 → P3 → P4 → P5 → P6, with P2 at any time. Each PR is cut fresh from zmk-next `main`.
Cherry-pick the hunks rather than whole commits where the inventory says "split", then apply the
Kconfig gates. Each PR adds its tests to `.github/workflows/pre-commit.yml`, the same way `totem`
does now. The tests are Python files that compile a C harness against the real source.
**Every gated PR also adds an "option off" case** that checks the source compiles to upstream
behavior. The current tests only cover the "on" path.

### P1. Small fixes (no gate): merged as rleyvasal/zmk-next#21 (`85e6b607`)
Six commits on `9036dec4`:
1. `bd4c932c` Discover only simulator cases in BLE test workflow (`d0a9e378`).
2. `0f6509ae` Ignore stale BLE connections when checking and using a profile's host (`9c4a396d`, `bdcf24d0`, `d42fe223`).
3. `0190a9e8` Add a weak hook that runs after settings finish loading (`455194e8`, main.c and settings.h only).
4. `0eb97c0a` Stop the Studio UART TX loop when the host isn't draining (`fd192e03`, `31be6fdc`).
5. `b698f25c` Re-arm Studio RX when the selected transport is selected again (`fd192e03`).
6. A workflow commit that runs the four native tests in `pre-commit.yml`.

- Left out: `34ff30a3` and the runtime-config `request_id`/mutex fix are already in zmk-next main.
  The endpoint "always re-evaluate" change moved to P5.
- Kconfig: none.
- Tests: `ble/active_connection` (from totem), plus two new ones, `ble/profile_connected` and `studio/uart_tx`.
  Both new tests fail against the old code. Also `settings/startup`.
- Checked: all four tests pass, clang-format is clean, and a nice_nano//zmk + corne_left build with
  Studio over USB and split battery fetching compiles with no warnings in the changed files. Not checked on hardware.
- Upstream candidates for zmkfirmware too: stale conn, UART TX spin.

### P2. Passkey entry hardening: merged as rleyvasal/zmk-next#26 (`3187bf8d`)
One commit on zmk-next main `afdec700` (after P4 #25): `010c8dac` Harden BLE passkey entry (`bdcf24d0` passkey part, `ebb7faff`, `01a193c7`).
- No new Kconfig. Everything is under the existing `ZMK_BLE_PASSKEY_ENTRY` (default n). The `pairing_failed`
  callback is registered only with that option on, so builds without it are unchanged. `select RING_BUFFER` is dropped.
- Escape-cancels was already upstream. New: a six-digit buffer, Enter submits only a full passkey, Backspace, a seventh digit
  ignored, a failed submit kept for retry, and no connection reference leak on a repeated request.
- Tests: **new** native `ble/passkey_entry` (real key handler). bsim `ble/security/passkey-confirmed-entry` and the
  test central's `-fixed_passkey`, which run in the BLE test workflow on CI only and were not run locally.
- Checked: all 12 native tests pass and clang-format is clean. nice_nano + corne_left builds with passkey entry off and on.
  Not tested on hardware.

### P3. Battery and charging: merged as rleyvasal/zmk-next#23 (`4961a928`)
Merged as #23 (`4961a928`). Three commits on `62e9fa8a`:
1. `0333ec86` Optionally keep a split peripheral's battery level while disconnected (`a87562a6`).
   **New** `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN`, default n. When it is off, a disconnect still reports 0.
2. `394df7e6` Optionally hold the battery level on external power and report charging (`fd192e03`,
   `455194e8`, `106c7880`, `e1b99c0e`, `ef07c750`, `b7a00d5c`).
   - **New** `ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER`, default n. It gates the cell memory, the `zmk/batt/cell`
     setting, VBUS detection, the charger reading, the estimate, the USB listener and the 1536-byte
     low-priority stack. When it is off, battery.c behaves as upstream: live levels, no settings writes.
   - `ZMK_BATTERY_CHARGE_STATUS`, default n, now **selects** the hold, because charge state tracking lives there.
     `ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC` depends on the hold.
   - `ZMK_BATTERY_CHARGE_STATE_API` stays `def_bool y`. The four functions always exist; with the hold off
     they return the live level, "discharging", -1 and the millivolts.
   - `zmk,battery-charger` binding. The XIAO board node has `charge-current-microamp = <50000>` and
     **no `status-gpios`**. The binding and docs say status-pin support has not been verified on hardware.
   - The split central subscribes to the peripheral's 0x2BED and keeps the state for
     `zmk_split_central_get_peripheral_battery_charge_state()`.
3. `0c4a315d` Optionally report the peripheral in the primary Battery Service.
   **New** `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP`, default n, one peripheral only.
   - **Found while porting:** zmk-totem's `central_bas_proxy.c` was not generic. It swapped the two
     Battery Services for Mighty Mitts on macOS: the primary BAS reported the right half, and the extra BAS was the left
     half, labeled "Left"/"main", with notifications rate-limited to one per second. It also ignored peripheral indices.
   - That layout is now this option. With the option off, the proxy and battery.c's primary-BAS writes are upstream's.
     Totem needs it on.
- Tests: `battery/persistence`, `battery/usb_boot`, `battery/charge_status`, `battery/charge_tracking` (from
  zmk-totem; persistence and charge_status now define the hold option). Also a new `battery/hold_off`, which compiles the
  real battery.c with the hold off and with settings and USB enabled but not mocked. It checks live levels and
  BAS writes, that no event fires on an unchanged reading, and that sampling stops when idle. With the hold on, the same harness fails to build.
  `pre-commit.yml` runs all five.
- Docs: rows in `battery.md` (three options, the chosen node, the binding note) and `split.md` (swap, retain).
- Checked on the final tree: all 9 native tests pass and clang-format is clean. Builds passed:
  - nice_nano//zmk + corne_left with fetching and the proxy, options off: no hold, BLS or swap symbols, stack 768.
  - corne_right, options off.
  - xiao_ble//zmk + totem_left with hold, charge status, settle 300, fetching, proxy, swap and retain on, using
    the shield from totem-zmk-config with its 110 mAh capacity.
  - totem_right with hold and charge status.
  Only the final tree was built, not the two intermediate commits. Not tested on hardware.
- Totem settings to add when it moves to ZMK Next: `CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER=y` (also selected by
  charge status), `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP=y` and
  `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN=y`.
- Any later fix to zmk-totem's battery code, such as the fix-left-battery-reading session's work, must be ported here too.

### P4. Split power: merged as rleyvasal/zmk-next#25 (`afdec700`)
One commit on zmk-next main `4961a928` (after P3 #23): `274d3058` Optionally slow the split link while the keyboard is idle (`fc522b17`, `753ade55`).
- Kconfig: `ZMK_SPLIT_BLE_IDLE_INT` defaults to 0 (off). `ZMK_SPLIT_BLE_IDLE_LATENCY` depends on IDLE_INT > 0. All the code,
  including the activity includes and subscription, is under `#if CONFIG_ZMK_SPLIT_BLE_IDLE_INT > 0`.
  The help text names the Zephyr supervision-countdown fix (`rleyvasal/zephyr` `ae038c05`).
- Tests: `split/idle_interval` from zmk-totem (4 cases), plus a new off check. It strips the idle blocks and asserts that no
  idle code or activity reference remains in central.c. `pre-commit.yml` runs it.
- Docs: two rows in `split.md`.
- Checked: all 11 native tests pass and clang-format is clean. Builds passed: nice_nano//zmk + corne_left with
  the option off (no idle symbols), and xiao_ble//zmk + totem_left with 48 and 4 (idle work present). Not tested on hardware.

### P5. USB and BLE coexistence while cabled: merged as rleyvasal/zmk-next#27 (`653b3c9c`)
One commit on zmk-next main `3187bf8d` (after P2 #26): `1bfc92f8` Optionally keep keys on USB and stop host advertising while cabled (`fd192e03`, `d6bc4563`).
- **Written fresh against zmk-next's current ble.c, not cherry-picked.** zmk-totem's version lives inside the P6 advertising
  worker and the throttle code.
- **New** `ZMK_USB_NO_BLE_FALLBACK_WHEN_POWERED`, default n, under `if ZMK_USB`, depends on ZMK_BLE. With USB preferred, VBUS
  present and HID not ready, the endpoint is none instead of BLE. It also re-evaluates when the same transport is selected again.
- **New** `ZMK_BLE_HOST_ADV_OFF_ON_USB`, default n, under `if ZMK_USB`, depends on ZMK_BLE. `update_advertising()` picks none
  while VBUS is present, and a listener on `zmk_usb_conn_state_changed` re-runs it. ZMK_USB only exists on a central or a
  unibody, so split peripherals are unaffected.
- Left out: Totem's 50 ms profile-switch deferral on USB. It isn't needed with advertising off on USB.
  The re-select skip and the throttle and boost interplay belong to P6.
- **For P6:** the advertising worker must keep this check in its target choice, the `adv_target_is_current()` VBUS term in Totem,
  and keep the USB listener.
- Tests: **new** native `usb/cabled`. It runs the real `get_selected_transport()` and `update_advertising()` with each option
  off and on. Docs: two rows in `system.md`.
- Checked: all 13 native tests pass and clang-format is clean. nice_nano + corne_left builds with the options off (no
  listener symbol) and both on. Not tested on hardware.

### P6. BLE advertising and multi-host: merged as rleyvasal/zmk-next#28 (`3193ba76`)
Two commits on zmk-next main `653b3c9c` (after P5 #27):
1. `394673c5` **P6a**, Run BLE advertising as one worker and drop stale targets (`9c4a396d`, `d54734f2`, `d42fe223`).
   This is not gated: `update_advertising()` runs only on the system work queue, uses a versioned target (profile, address and USB
   changes), re-checks the target around `bt_le_adv_start()`, and handles `ADV_LINK_COMPLETE` and `ADV_RESTART` requests. It adds
   RPA-aware `zmk_ble_profile_index()` and `is_conn_active_profile()`, the directed-to-open fallback (directed is still unused by
   default), and a weak `zmk_ble_advertising_observed()`. Start errors are returned as before.
   **It was produced mechanically:** zmk-totem's ble.c was evaluated with every policy option set to 0, then hand-trimmed.
2. `8c2d1d84` **P6b**, Add optional multi-host advertising policies for split centrals (`9c4a396d`, `d54734f2`, `4d476144`,
   `25907404`, `429d7242`). It adds a new menu, "Multi-host advertising (split central)", `depends on ZMK_SPLIT_ROLE_CENTRAL`, with
   everything default n or 0:

   | zmk-totem / totem-zmk-config | zmk-next | Totem value |
   |---|---|---|
   | `TOTEM_ADV_THROTTLE` / `_TIMEOUT_MIN` | `ZMK_BLE_ADV_TIMEOUT` / `_MIN` | y / 5 |
   | `TOTEM_ADV_BOOST` / `_SEC` | `ZMK_BLE_ADV_BOOST` / `_SEC` | y / 20 |
   | `TOTEM_ADV_POST_SWITCH_DARK` | `ZMK_BLE_ADV_POST_SWITCH_DARK` | n |
   | `TOTEM_DIR_THEN_OPEN` / `TOTEM_DIR_ADV_SEC` | `ZMK_BLE_DIRECTED_ADV` / `_SEC` | n / 4 |
   | `TOTEM_ACTIVE_ADV_FILTER` | `ZMK_BLE_ADV_FILTER_ACTIVE_PROFILE` (selects BT_FILTER_ACCEPT_LIST) | y |
   | `TOTEM_RESELECT_RECONNECT` | `ZMK_BLE_RESELECT_RECONNECT` | y |
   | `TOTEM_IDLE_DISCONNECT` / `_MIN` | `ZMK_BLE_IDLE_DISCONNECT` / `_MIN` | n |
   | `TOTEM_EVICT_ADV_COOLDOWN_MS` | `ZMK_BLE_EVICT_ADV_COOLDOWN_MS` | 0 |
   | `TOTEM_EXCLUSIVE_HOST` (ble.c handoff part) | `ZMK_BLE_PROFILE_HANDOFF` | y |
   | `TOTEM_EXCLUSIVE_DISCONNECT_REASON` | `ZMK_BLE_PROFILE_HANDOFF_DISCONNECT_REASON` | 0x13 |
   | (ungated hog.c hooks) | `ZMK_BLE_DIAGNOSTIC_HOOKS` | y |

   - API renames: `zmk_ble_totem_ads_suppressed` → `zmk_ble_host_adv_suppressed`, `_adv_state` → `zmk_ble_adv_state`, `_wake_ads` →
     `zmk_ble_adv_wake`, `_adv_boost_rearm` → `zmk_ble_adv_boost_rearm`, `_kick_open_adv` → `zmk_ble_adv_kick`.
   - USB suppression, which was unconditional on a split central in Totem, now follows P5's `ZMK_BLE_HOST_ADV_OFF_ON_USB` (target
     staleness, handoff skip, keypress and re-select skip, suppressed API).
   - **Fixed while porting:** Totem's open-advertising macro swallowed start errors ("will retry") even when nothing retries.
     That now happens only with `ZMK_BLE_ADV_TIMEOUT`; otherwise the error is returned as upstream does.
- Tests: **new** `ble/advertising_worker`. It covers the real worker with every option off and runs on the P6a and P6b files both.
  `usb/cabled`'s advertising case moved there. From zmk-totem: `ble/advertising_owner`, `ble/profile_notifications` and
  `ble/keyboard_ccc`. keyboard_ccc needs `ZEPHYR_BASE` and is skipped in CI. All 17 native tests pass.
- Builds: P6a alone and the final tree, nice_nano + corne_left with options off, show only the worker symbols. corne_right
  builds. Totem left builds with Totem's set (timeout, boost 20, filter, re-select, handoff, hooks, both USB options) and with
  the rest (post-switch dark, directed, idle disconnect, cooldown 200). Not tested on hardware.
- **totem-zmk-config changes when it moves:** in `totem.conf`, rename to the table above. In `Kconfig`, `TOTEM_EXCLUSIVE_HOST` selects
  `ZMK_BLE_PROFILE_HANDOFF`, and drop the moved symbols. In `src/`: `reconnect_watch.c` (suppressed, rearm, kick, `ADV_BOOST(_SEC)`),
  `totem_studio_extras.c` (adv_state), `totem_studio_usb.c` (wake), `exclusive_host.c` (disconnect reason). Then update
  `verify-zmk-patch.yml`'s symbol list.

### P7. Studio raw frames: merged as rleyvasal/zmk-next#22
Two commits on `9036dec4`. The branch is independent of P1. The source files merge cleanly. `pre-commit.yml` conflicts, because both add a test step at the same place. Whichever PR lands second keeps both steps.
1. `37eb6042` Export `zmk_studio_uart_rearm()` for the Studio UART transport (`fd192e03`). It is not
   gated: the helper does nothing unless a module calls it. Totem's `totem_studio_usb.c` calls it.
2. `3640a015` Add Studio raw frames for modules behind `ZMK_STUDIO_RPC_RAW_FRAMES` (`dfdcdef3`,
   `6848616d`, `31be6fdc`).

- Kconfig: **new** `ZMK_STUDIO_RPC_RAW_FRAMES` (default n, inside `if ZMK_STUDIO_RPC`). When it is off,
  `rpc_read_cb()` and `rpc_main()` behave exactly as before, `zmk_rpc_send_raw_frame()` and
  `ZMK_RPC_RAW_FRAME_HANDLER()` are not declared, and the linker section is not added.
  zmk-studio-log's existing `#ifndef ZMK_RPC_RAW_FRAME_HANDLER` → `#error` therefore still catches a missing option.
- Tests: `studio/raw_tx`, from zmk-totem, now has three cases. The send and receive tests run with the option on.
  The receive test also runs with it off: `C` frames reach the protobuf decoder unchanged. A
  preprocessor check confirms the sender is not compiled when the option is off. `pre-commit.yml` runs it.
- Checked: clang-format is clean. Two nice_nano//zmk + corne_left Studio builds pass. Off: no raw-frame
  symbols in the ELF. On, with zmk-studio-log `531e4e7` and `CONFIG_ZMK_STUDIO_LOG=y`: links without the module's patch
  (`zmk_rpc_send_raw_frame`, `zmk_studio_log_control` present). Not checked on hardware.
- Follow-up in zmk-studio-log, after P7 lands: delete `patches/0001-studio-raw-frames.patch`, add
  `select ZMK_STUDIO_RPC_RAW_FRAMES` to `ZMK_STUDIO_LOG`, and change the `#error` text to name the option.

## 3. Decisions on the core BLE/USB patches

| Patch | Decision | Reason |
|---|---|---|
| Suppress host BLE while USB HID is active (`d6bc4563`) | **Kconfig-gated generic** (P5) | Small, self-contained, and every `zmk_usb_is_powered()` check sits at a few clear points. A gate costs little. As a labeled patch it would conflict with P6's advertising worker on every bump. |
| Keep USB HID while cabled (`fd192e03` endpoints) | **Kconfig-gated generic** (P5), with a warning | It is a one-branch change in `get_selected_transport()`. As it stands it is unsafe as a default (power-bank charging gives no typing), so it must be opt-in. |
| Integrate Totem BLE reconnect support (`9c4a396d`) | **Kconfig-gated generic** (P6b), after the rename | Already gated, though on the wrong symbols. The work is renaming and defining the Kconfig, not rewriting code. Keeping it as a patch would mean 800 lines of ble.c to rebase each week. |
| Harden BLE pairing (`bdcf24d0`) | **Generic**, no new gate (P2) | Already under `ZMK_BLE_PASSKEY_ENTRY` (default n), and it has a bsim test. |
| Serialize BLE advertising (`d54734f2`, worker part) | **Generic, ungated** (P6a), **or a small labeled patch if review stalls** | Only this one cannot be gated cheaply: it restructures how `update_advertising()` runs. It changes behavior only in races. If P6a stalls, keep it as `patches/zmk-adv-worker.patch` in totem-zmk-config, applied by the build, rather than keep a fork. |

Today, nothing else needs to remain a patch.

## 4. What stays in totem-zmk-config

- `config/totem.conf`: the values in the P6 table, `ZMK_SPLIT_BLE_IDLE_INT=48`,
  `ZMK_SPLIT_BLE_IDLE_LATENCY=4`, `ZMK_BATTERY_CHARGE_STATUS=y`, `ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER=y`,
  `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY_SWAP=y`,
  `ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC=300`, `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN=y`,
  `ZMK_USB_NO_BLE_FALLBACK_WHEN_POWERED=y`, `ZMK_BLE_HOST_ADV_OFF_ON_USB=y`, and
  `ZMK_BLE_DIAGNOSTIC_HOOKS=y`.
- `boards/shields/totem/totem.dtsi`: the 110 mAh capacity, already there.
- `Kconfig` and `src/`: the Totem modules (journal, watchdog, reconnect watch, exclusive host,
  lazy host, the diagnostic logs). Remove the `TOTEM_*` symbols that moved to zmk-next.
- The `west.yml` Zephyr override (`ae038c05`) until the supervision fix is upstream.

## Status (2026-10-17)
All seven themed PRs are merged into zmk-next main (`3193ba76`). totem-zmk-config branch `zmk-next-direct` pins it.
Builds for left (Studio), right and left logging pass locally. On the left half the options match zmk-totem's behavior
(see the switch PR). Next: flash, check on hardware, merge, then archive zmk-totem.

## 5. Retiring zmk-totem

1. **Pause the weekly bump now** (done locally on totem-zmk-config branch `pause-zmk-bump`, `ac547de`, not pushed). `.github/workflows/zmk-bump.yml` merges each ZMK Next release
   into zmk-totem `totem`. Once P1 lands, that merge conflicts with `totem`'s own copies of the same
   hunks, and the conflicts get worse with each PR after it. Change the schedule to `workflow_dispatch` only for the migration.
   (Another session committed `c721484` to `zmk-bump.yml` today, which retired the `persistent-diagnostic-log` trial branch. Start from `main` after that commit.)
2. Land P1, P7, P3, P4, P5, P6 (and P2) in zmk-next and cut a ZMK Next release.
3. On a config branch (for example `zmk-next-direct`):
   - `west.yml`: the `zmk` project gets `repo-path: zmk-next` and the release commit.
   - `totem.conf`: the renamed symbols. `Kconfig`: remove the moved `TOTEM_*` symbols.
     `src/`: switch to the new API names.
   - `verify-zmk-patch.yml`: change the required-symbol list to the new names.
   - Build both halves in CI, and check that `git diff zmk-next/<release> zmk-totem/totem -- app` contains only
     the renames and gates.
4. Hardware check with the bump PR's checklist (typing, profile switch, single active host,
   reconnect, USB stable, logs, idle drain 0.5 to 0.9 %/h). Also check the 'K' line and charge "full".
5. Merge into `main`. Rewrite `zmk-bump.yml` to move the pin to the latest ZMK Next release (no
   fork merge, no `ZMK_FORK_TOKEN`), run the zmk-next tests, open the PR, and restore the weekly schedule.
6. Move zmk-totem's README content into `STACK.md`, archive `rleyvasal/zmk-totem` as read-only
   (keep `totem` and `b7a00d5` as the rollback point), and delete the `ZMK_FORK_TOKEN` secret.

## 6. Open questions

- Is it OK that P1 and P6a change behavior with no gate? They are bug fixes and a refactor, not features.
  The alternative, `ZMK_BLE_ADV_WORKER` default n, means keeping two advertising code paths.
- P5 has no tests today. Writing them is part of the PR.
- Should P6b's policies be allowed on unibody keyboards (`!ZMK_SPLIT`)? This plan allows it, but only split centrals have been tested.
