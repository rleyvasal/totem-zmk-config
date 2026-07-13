# ZMK patch — advertising throttle + idle-disconnect

`totem-ble.patch` is a small change to ZMK's `app/src/ble.c`, central-half only,
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

ZMK's build has no patch hook, so the patch is hosted on a thin fork that
`config/west.yml` points at (`rleyvasal/zmk`). The patch file here is the source of
truth; the fork is just where the applied result lives. Each ZMK base gets its own
branch **`totem-optimized-<zmk-sha>`** (= that upstream commit + this patch), and
`config/west.yml`'s `revision:` tracks the current one. The current branch is
`totem-optimized-484a054`.

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
  post-release main pin such as `totem-optimized-484a054` when the latest release
  is still `v0.3.0`)
- **applies cleanly** (+ post-apply symbol checks for go-dark/throttle) → pushes
  `totem-optimized-<new-sha>` to the fork and opens a **config PR** that repoints
  `config/west.yml`. CI builds flashable artifacts — flash, run the PR checklist,
  then **merge to accept**. Nothing lands on `main` until you merge.
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
git checkout -B totem-optimized-<new-short-sha> <new-upstream-sha>
git apply ~/totem-zmk-config/patches/totem-ble.patch            # fix hunks if it fails
git commit -am "Totem optimized (throttle + idle-disconnect) on ZMK <new-short-sha>"
git push -f origin totem-optimized-<new-short-sha>
```

If you fixed hunks by hand, regenerate the patch:
`git -C ~/zmk diff <new-upstream-sha> -- app/src/ble.c > patches/totem-ble.patch`.
Then set `config/west.yml` `revision:` to the new branch and push the config repo.
