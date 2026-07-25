# ZMK patch — advertising throttle + idle-disconnect

`zmk-ble.patch` is a small change to ZMK's `app/src/ble.c`, central-half only,
gated behind `CONFIG_TOTEM_*` flags (defined in this repo's `Kconfig`):

**Advertising throttle** (`CONFIG_TOTEM_ADV_THROTTLE`):

- When the **selected** profile's device has been disconnected for
  `CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN` minutes, **stop advertising** to save
  power. A key press on either half resumes it (first press or two may be lost).
- The idle timer uses `k_work_schedule` (not `reschedule`), so a nearby *other*
  device's connect/disconnect churn cannot keep resetting it.
- Because it stops advertising, it also stops cross-talk for free — nothing can
  connect while the keyboard isn't advertising. No address-based filtering (that's
  what broke the earlier accept-list / drop-module reconnect).

**Idle-disconnect** (`CONFIG_TOTEM_IDLE_DISCONNECT`, requires the throttle):

- After `CONFIG_TOTEM_IDLE_DISCONNECT_MIN` minutes with no keypress, **disconnect
  the active host and immediately go dark** (pause advertising) so it can't
  instantly reconnect and wake the host. A key press resumes advertising and the
  host reconnects. This lets an asleep host — deep *or* light sleep (plugged in /
  external monitor) — stay gone so drain drops to the throttled rate, without the
  wake storm a plain disconnect caused.
- The idle countdown **arms when the active host connects** (not only on keypress),
  and cancels when that host disconnects. Keypresses still reset the countdown
  while typing.

**Advertising boost** (`CONFIG_TOTEM_ADV_BOOST`, requires the throttle):

- For `CONFIG_TOTEM_ADV_BOOST_SEC` after a **profile switch** (or wake from dark),
  advertise at GAP fast interval 1 (**30–60 ms**) instead of ZMK’s default fast
  interval 2 (**100–150 ms**), then return to normal. Speeds host discovery when
  switching computers.
- Profile select also **raises the profile-changed event before advertising**, so
  exclusive-host can drop the previous computer first.

**Directed-then-open** (`CONFIG_TOTEM_DIR_THEN_OPEN`, requires the throttle):

- After profile switch, for `CONFIG_TOTEM_DIR_ADV_SEC` seconds advertise
  **directed** (low-duty) to the active bonded peer (try `DIR_ADDR_RPA` for
  privacy centrals), then switch to **open undirected** + boost.
- Exclusive-host still disconnects the previous host (no multi-link).
- Directed start failure → immediate open undirected (fail-open).

**Reselect soft-reconnect** (`CONFIG_TOTEM_RESELECT_RECONNECT`, requires the throttle):

- Pressing `&bt BT_SEL n` when profile `n` is **already active** disconnects that
  host (if connected) and re-advertises with the boost window. Soft recovery for
  half-dead links (macOS “Connected but mute”) without Forget + re-pair when the
  bond itself is still good.

**Self-recovery without power switches** (`CONFIG_TOTEM_RPA_DISCONNECT`,
`CONFIG_TOTEM_ADV_RECONCILE`, `CONFIG_TOTEM_RECOVERY_REBOOT`):

Fixes the 2026-07-24 failure: Mac away 6 h, returned greyed out in macOS
Bluetooth; keypresses, repeated `BT_SEL 0` and toggling macOS Bluetooth all did
nothing, and only power-cycling both halves recovered it.

- **Root cause.** `zmk_ble_prof_disconnect()` looked the host up by stored
  *identity* address only, so it returned `-ENODEV` for an RPA-connected macOS,
  while `zmk_ble_profile_is_connected()` *did* resolve the RPA and answered
  "connected". A stale conn object that nothing could tear down: `update_advertising()`
  then computed `desired_adv = ZMK_ADV_NONE` and stayed dark **on purpose**, and the
  keypress listener, `zmk_ble_totem_kick_open_adv()` and reconnect_watch all
  short-circuited on "the active profile is connected".
- `RPA_DISCONNECT` gives the disconnect the same IRK-aware conn lookup
  (`totem_profile_conn`). Side effect: `BT_SEL` on the already-active profile now
  really drops a live macOS link instead of silently failing.
- `ADV_RECONCILE` stops trusting `advertising_status`. It could claim
  `ZMK_ADV_CONN` while the controller was not advertising — and `update_advertising()`
  cannot leave that state, because `desired == current` matches no `switch` case.
  On user intent (keypress with the selected host down, or `BT_SEL`) it cancels the
  throttle/retry work, clears the throttle, stops advertising accepting `-EALREADY`
  (the only available "were we actually advertising?" probe), resets the status and
  restarts. Rate-limited by `TOTEM_ADV_RECONCILE_COOLDOWN_MS` (restarting ads resets
  the advertising interval, so per-keystroke reconciles would hurt discovery). A
  keypress **never** disconnects anything; only the `BT_SEL` path may.
- `CHECKED_OPEN_ADV` keeps its fail-soft `err = 0` but now records the real error in
  `totem_adv_start_err`, so "the stack refuses to advertise" is distinguishable from
  "the host is absent".
- `RECOVERY_REBOOT` is the last resort, on objective evidence only: genuine
  `bt_le_adv_start()` errors across the whole 25 × 400 ms budget *after* user
  intent, or a conn still reporting connected `TOTEM_ZOMBIE_VERIFY_MS` after a
  `BT_SEL` disconnect. Cold reboot of the central — bonds, profiles and settings in
  NVS survive (same as `[ + Z` / `&sys_reset`, **not** a settings reset). Loop
  breakers: `TOTEM_RECOVERY_REBOOT_MIN_UPTIME_SEC` floor, and any peripheral
  disconnect cancels the zombie verify so a host that reconnects fast (macOS often
  does, sometimes under an unresolved RPA) can never be mistaken for a wedged conn.
- Reachability matters as much as the logic: `BT_SEL` now also lives on the **MOD
  layer's left half** (`Tab`+`Z/X/C`, `Tab`+`D`/`V` for clear), so recovery does not
  depend on the split link to the right half. See `config/totem.keymap`.

**RPA-aware profile matching + open-adv retry** (always on with the throttle patch):

- `zmk_ble_profile_index` maps a live RPA to a profile by matching against
  connections looked up via the stored identity (no private `keys.h`).
- `zmk_ble_active_profile_conn` / `profile_is_connected` fall back to scanning live
  host connections when identity lookup by stored address misses.
- Open advertising soft-fails and retries when a background host is holding a
  connection slot, instead of going dark for the selected host.

**Active-host filter accept list** (`CONFIG_TOTEM_ACTIVE_ADV_FILTER`, needs
`CONFIG_BT_FILTER_ACCEPT_LIST`):

- When the active profile is bonded, advertising uses `BT_LE_ADV_OPT_FILTER_CONN`
  and the accept list contains **only** that profile's peer. Background bonded
  hosts cannot complete a connection (primary multi-host thrash isolation).
  Open/empty profile → unfiltered ads for pairing. Fail-open: FAL setup/start
  failure → unfiltered open advertising. Enabled in `totem.conf` (2026-07-21);
  set `=n` if advertising vanishes on your hardware.

**Post-evict advertising cooldown** (`CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS`):

- Optional delay before open advertising after a non-active host disconnects.
  **Keep at 0** — a non-zero value also blinds the selected host and regressed
  dual-host switching (2026-07-17). Profile switch re-advertises immediately.

**Reconnect watch** (config module `src/reconnect_watch.c`, not this patch):

- After `BT_SEL`, recovery ladder if the active host stays down (kick ads, evict
  background, soft-drop zombie). Never clears bonds.
- With `TOTEM_RECONNECT_WATCH_ON_ACTIVE_DOWN`, also arms a **light** ladder when
  the active peer disconnects mid-session (peer-mapped only; never on background
  thrash; skips when ads are suppressed for go-dark).
- Uses patch helpers: `zmk_ble_totem_ads_suppressed`,
  `zmk_ble_totem_adv_boost_rearm` (densify via stop+restart),
  `zmk_ble_totem_kick_open_adv`.

ZMK's build has no patch hook, so the patch is hosted on a thin fork that
`config/west.yml` points at (`rleyvasal/zmk`). The patch file here is the source of
truth; the fork is just where the applied result lives. Each ZMK base gets its own
branch **`zmk-optimized-<zmk-sha>`** (= that upstream commit + this patch).
**`config/west.yml` pins the full commit SHA** of that tip (reproducible builds;
force-pushes cannot move a validated pin). The human branch name is still
`zmk-optimized-484a054` (and friends).

## Updating to a newer ZMK — automated (preferred)

`.github/workflows/zmk-bump.yml` implements **policy A**:

| Trigger | Target |
|---|---|
| **Schedule** (Monday 06:00 UTC) | Latest *stable* `zmkfirmware/zmk` GitHub release only |
| **`workflow_dispatch` → release** | Same as schedule (default) |
| **`workflow_dispatch` → main** | `zmkfirmware/zmk` main HEAD (manual / bleeding-edge) |
| **`workflow_dispatch` → ref** | Explicit tag, branch, or SHA |

It applies this patch and:

- **already current / already past a release** → no-op (will **not** downgrade a
  post-release main pin such as `zmk-optimized-484a054` when the latest release
  is still `v0.3.0`)
- **applies cleanly** (+ post-apply symbol checks for go-dark/throttle/boost) →
  pushes `zmk-optimized-<new-sha>` to the fork and opens a **config PR** that
  pins `config/west.yml` to the **new fork tip SHA**. CI builds flashable
  artifacts — flash, run the PR checklist, then **merge to accept**. Nothing
  lands on `main` until you merge.
- **`verify-zmk-patch.yml`** on every push/PR greps the pinned revision for the
  same symbols so an unpatched tip cannot silently build.
- **conflicts or sanity-check failure** → opens an **issue** (deduped); rebase by
  hand (below).
- **open PR/issue already exists** for that short SHA → no-op

Requires the `ZMK_FORK_TOKEN` secret (fine-grained PAT, Contents:write on the fork).

## Updating / fixing by hand

Only needed for the first-time branch, or when the workflow reports a conflict:

```sh
git clone https://github.com/rleyvasal/zmk.git ~/zmk        # or reuse existing
cd ~/zmk
git remote add upstream https://github.com/zmkfirmware/zmk.git   # skip if present
git fetch upstream
git checkout -B zmk-optimized-<new-short-sha> <new-upstream-sha>
git apply ~/totem-zmk-config/patches/zmk-ble.patch            # fix hunks if it fails
git commit -am "ZMK optimized (throttle + idle-disconnect + boost) on <new-short-sha>"
git push -f origin zmk-optimized-<new-short-sha>
TIP=$(git rev-parse HEAD)
```

If you fixed hunks by hand, regenerate the patch:
`git -C ~/zmk diff <new-upstream-sha> -- app/src/ble.c > patches/zmk-ble.patch`.
Then set `config/west.yml` `revision:` to **`$TIP` (full SHA)**, not only the
branch name, and push the config repo.
