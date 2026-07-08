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

The patch also carries the retired disconnect-on-profile-switch code behind its own
(off) `CONFIG_TOTEM_DISCONNECT_ON_PROFILE_SWITCH` flag.

ZMK's build has no patch hook, so the patch is hosted on a thin fork that
`config/west.yml` points at (`rleyvasal/zmk`, branch `totem`). The patch file here
is the source of truth; the fork is just where the applied result lives.

## One-time fork setup

1. Fork `zmkfirmware/zmk` to `rleyvasal/zmk` on GitHub (web UI) if it doesn't exist.
2. Create/refresh the `totem` branch = pinned upstream + this patch, and push it:

   ```sh
   git clone https://github.com/rleyvasal/zmk.git ~/zmk        # or reuse existing
   cd ~/zmk
   git remote add upstream https://github.com/zmkfirmware/zmk.git   # skip if present
   git fetch upstream
   git checkout -B totem 484a0547078228f6957ed164523823e39fbedec4
   git apply ~/totem-zmk-config/patches/totem-ble.patch
   git commit -am "Totem BLE: advertising throttle"
   git push -f origin totem
   ```

Then push this config repo — CI builds against `rleyvasal/zmk@totem`.

## Updating to a newer ZMK

```sh
scripts/update-zmk.sh <new-upstream-sha>
```

Re-applies the patch onto the new SHA and force-pushes `totem`. If upstream moved
the code the patch touches, `git apply` fails — fix the hunks by hand, then
regenerate: `git -C ~/zmk diff app/src/ble.c > patches/totem-ble.patch`.
