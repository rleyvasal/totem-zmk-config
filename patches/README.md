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

**Reselect soft-reconnect** (`CONFIG_TOTEM_RESELECT_RECONNECT`, requires the throttle):

- Pressing `&bt BT_SEL n` when profile `n` is **already active** disconnects that
  host (if connected) and re-advertises with the boost window. Soft recovery for
  half-dead links (macOS “Connected but mute”) without Forget + re-pair when the
  bond itself is still good.

**Post-evict advertising cooldown** (`CONFIG_TOTEM_EVICT_ADV_COOLDOWN_MS`):

- After a **non-active** host disconnects while the selected host is still away,
  delay open advertising by this many ms. Breaks the exclusive-host thrash loop
  (wrong host reconnects immediately and starves the selected one). Profile
  switch and active-host disconnect re-advertise immediately.

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
