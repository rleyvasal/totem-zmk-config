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

### P1. Small fixes (no gate): cut 2026-10-16, local branch `p1-small-fixes`, not pushed
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

### P2. Passkey entry hardening
- Commits: `bdcf24d0` (passkey part + `pairing_failed`), `ebb7faff`, `01a193c7`, bluetooth.md part of `1011953a`.
- Kconfig: none new. It is under `ZMK_BLE_PASSKEY_ENTRY` (default n). `select RING_BUFFER` is dropped.
- Tests: bsim `ble/security/passkey-confirmed-entry` and the `ble/central` passkey handler.
- Note for users who already have passkey entry on: Enter now requires exactly 6 digits, and Escape cancels.

### P3. Battery and charging
- Commits: battery/BAS-proxy parts of `fd192e03`, battery part of `455194e8`, `106c7880`, `a87562a6`,
  `e1b99c0e`, `ef07c750`, `b7a00d5c`, battery.c part of `1011953a`.
- Kconfig (new or changed):
  - **New** `ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER` (bool, default **n**, depends on
    `ZMK_BATTERY_REPORTING`). It gates the last-cell memory, the `zmk/batt/cell` setting, the VBUS
    hold, the charger reading and the estimate. When off, battery.c must report live readings exactly as today.
  - `ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC` (default 0) becomes `depends on ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER`.
  - `ZMK_BATTERY_CHARGE_STATUS` stays default n, and should `imply` the hold option.
  - `ZMK_BATTERY_CHARGE_STATE_API` is now `def_bool y`. Change it to
    `def_bool ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER`, or keep it y only if `zmk_battery_reported_soc()`
    and `zmk_battery_charge_state()` exist (returning live SOC / unknown) when the feature is off.
    zmk-studio-log checks this symbol.
  - `ZMK_LOW_PRIORITY_THREAD_STACK_SIZE`: change the `default 1536 if …` condition to
    `if ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER && SETTINGS`.
  - **New** `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN` (default **n**) for `a87562a6`. When off,
    a disconnect still reports 0.
  - `xiao_ble_zmk.dts` keeps the `xiao_charger` node **without `status-gpios`**, only
    `charge-current-microamp = <50000>`. On the user's XIAO nRF52840 Sense, P0.17 read "not charging"
    (with `GPIO_ACTIVE_LOW | GPIO_PULL_UP`) while the cell charged from 4029 to 4213 mV on USB. So the earlier
    "full" readings were misreads, and the estimate never ran. The fix to zmk-totem's board file comes on top of
    `b7a00d5`; port that version.
  - `status-gpios` support in `zmk,battery-charger` stays in the binding, but only native tests cover it,
    never real hardware. Say so in the binding description and the PR.
- Tests: `battery/persistence`, `battery/usb_boot`, `battery/charge_status`, `battery/charge_tracking`,
  plus a new off case (live SOC on VBUS, no settings write).
- Docs: `docs/docs/config/battery.md` rows for the new options and the binding.

### P4. Split power
- Commits: `fc522b17`, `753ade55`.
- Kconfig: `ZMK_SPLIT_BLE_IDLE_INT` already defaults to 0 (off), and `ZMK_SPLIT_BLE_IDLE_LATENCY` only applies when it is above 0. No change needed.
- **Dependency:** the switch from idle back to fast drops the link without the Zephyr supervision
  countdown fix (`rleyvasal/zephyr` `ae038c05`). zmk-next's `app/west.yml` pins zmkfirmware's Zephyr,
  so the help text must say so. totem-zmk-config keeps its Zephyr override until the fix is upstream.
- Tests: `split/idle_interval`.

### P5. USB and BLE coexistence while cabled
- Commits: endpoints and ble.c parts of `fd192e03` (including the "always re-evaluate" change in
  `zmk_endpoint_set_preferred_transport`), `d6bc4563`.
- Kconfig (new, all default **n**):
  - `ZMK_USB_NO_BLE_FALLBACK_WHEN_POWERED` (depends on `ZMK_USB && ZMK_BLE`). When on, the keyboard
    waits for USB HID instead of falling back to BLE. The help text must warn that typing over BLE
    while charging from a power bank needs `&out OUT_BLE`.
  - `ZMK_BLE_HOST_ADV_OFF_ON_USB` (depends on `ZMK_USB && ZMK_BLE`). Host advertising stays off while VBUS is present,
    the profile switch applies 50 ms later instead of on the key path, and re-select is skipped. Keep the
    current `ZMK_SPLIT_ROLE_CENTRAL` restriction for the first PR, because only split centrals have been tested.
- Tests: none exist yet. **Write** `endpoints/usb_powered` (transport choice with the option on and off)
  and `ble/usb_adv` (no advertising while powered when on; unchanged when off).

### P6. BLE advertising and multi-host robustness (largest)
- Commits: `9c4a396d` (minus P1 parts), `d54734f2`, `4d476144`, `25907404` + `429d7242`, the ble.c
  `zmk_ble_advertising_observed` hook.
- Split into **P6a** (advertising worker: serialized owner, versioned target, directed advertising
  with the `DIR_ADDR_RPA` retry, `4d476144`) and **P6b** (policies).
  P6a changes how advertising runs internally, not what the keyboard does, and is the risky refactor. Review it on its own.
- Kconfig: rename `TOTEM_*` to `ZMK_BLE_*` in a new `menu "BLE multi-host policies"`, all default
  **n** or 0, `depends on ZMK_BLE && (!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)`:

  | totem-zmk-config today | zmk-next | Totem value |
  |---|---|---|
  | `TOTEM_ADV_THROTTLE` + `_TIMEOUT_MIN` | `ZMK_BLE_ADV_IDLE_TIMEOUT_MIN` (0 = off) | 5 |
  | `TOTEM_ADV_BOOST` + `_SEC` | `ZMK_BLE_ADV_BOOST_SEC` (0 = off) | 20 |
  | `TOTEM_ADV_POST_SWITCH_DARK` | `ZMK_BLE_ADV_POST_SWITCH_DARK` | n |
  | `TOTEM_DIR_THEN_OPEN` + `_DIR_ADV_SEC` | `ZMK_BLE_DIRECTED_ADV_SEC` (0 = off) | 0 |
  | `TOTEM_ACTIVE_ADV_FILTER` | `ZMK_BLE_ADV_FILTER_ACTIVE_PROFILE` (selects `BT_FILTER_ACCEPT_LIST`) | y |
  | `TOTEM_RESELECT_RECONNECT` | `ZMK_BLE_RESELECT_RECONNECT` | y |
  | `TOTEM_IDLE_DISCONNECT` + `_MIN` | `ZMK_BLE_IDLE_DISCONNECT_MIN` (0 = off) | 0 |
  | `TOTEM_EVICT_ADV_COOLDOWN_MS` | `ZMK_BLE_EVICT_ADV_COOLDOWN_MS` | 0 |
  | `TOTEM_EXCLUSIVE_HOST` / config's `ZMK_EXCLUSIVE_HOST` (handoff part only) | `ZMK_BLE_PROFILE_HANDOFF` | y |
  | `TOTEM_EXCLUSIVE_DISCONNECT_REASON` | `ZMK_BLE_PROFILE_HANDOFF_DISCONNECT_REASON` (0x13) | 0x13 |
  | (weak hooks, ungated) | `ZMK_BLE_DIAGNOSTIC_HOOKS` | y |

  Several of these currently need `TOTEM_ADV_THROTTLE`. In zmk-next each option should stand alone,
  or state its dependency with `depends on`.
- API renames: `zmk_ble_totem_ads_suppressed` → `zmk_ble_host_adv_suppressed`,
  `_adv_state` → `zmk_ble_adv_state`, `_adv_boost_rearm` → `zmk_ble_adv_boost_rearm`,
  `_kick_open_adv` → `zmk_ble_adv_kick`, `_wake_ads` → `zmk_ble_adv_wake`. Keep the old names as
  `static inline` wrappers in totem-zmk-config, not in zmk-next.
- Tests: `ble/advertising_owner`, `ble/profile_notifications`, `ble/keyboard_ccc`, plus an off case
  that checks `update_advertising()` makes the same choices as upstream.

### P7. Studio raw frames
- Commits: `31be6fdc` (gated part) with `dfdcdef3` folded in, `6848616d`, and the `rpc.h` part of `fd192e03`.
- Kconfig: **new** `ZMK_STUDIO_RPC_RAW_FRAMES` (default n, depends on `ZMK_STUDIO_RPC`). When off,
  `rpc_read_cb` has no prefix check and the linker section is not added.
- Follow-up in zmk-studio-log: delete `patches/0001-studio-raw-frames.patch`, and have the module's
  Kconfig `select ZMK_STUDIO_RPC_RAW_FRAMES`.
- Tests: `studio/raw_tx`, extended with the off case.

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
  `ZMK_BATTERY_SETTLE_AFTER_CHARGE_SEC=300`, `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_RETAIN=y`,
  `ZMK_USB_NO_BLE_FALLBACK_WHEN_POWERED=y`, `ZMK_BLE_HOST_ADV_OFF_ON_USB=y`, and
  `ZMK_BLE_DIAGNOSTIC_HOOKS=y`.
- `boards/shields/totem/totem.dtsi`: the 110 mAh capacity, already there.
- `Kconfig` and `src/`: the Totem modules (journal, watchdog, reconnect watch, exclusive host,
  lazy host, the diagnostic logs). Remove the `TOTEM_*` symbols that moved to zmk-next.
- The `west.yml` Zephyr override (`ae038c05`) until the supervision fix is upstream.

## 5. Retiring zmk-totem

1. **Pause the weekly bump now.** `.github/workflows/zmk-bump.yml` merges each ZMK Next release
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
