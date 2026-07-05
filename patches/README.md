# ZMK patches

`totem-ble.patch` is a small change to ZMK's `app/src/ble.c`, gated behind Kconfig
flags defined in this repo's `Kconfig` (so it only builds when enabled, and only
on the central/left half):

- **`CONFIG_TOTEM_ACTIVE_PROFILE_ONLY`** — advertise with a BLE *accept list* of just
  the active profile's bonded device, so other previously-paired computers can't
  connect at all (ignored at the radio level).
- **`CONFIG_TOTEM_ADV_THROTTLE`** — pause advertising after the host has been away
  for `CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN` minutes; a keypress resumes it.

ZMK's build has no patch hook, so the patch is hosted on a thin fork that
`config/west.yml` points at (`rleyvasal/zmk`, branch `totem`). The patch file here
is the source of truth; the fork is just where the applied result lives.

## One-time fork setup

1. Fork `zmkfirmware/zmk` to `rleyvasal/zmk` on GitHub (web UI).
2. Create the `totem` branch = pinned upstream + this patch, and push it:

   ```sh
   git clone https://github.com/rleyvasal/zmk.git ~/zmk
   cd ~/zmk
   git remote add upstream https://github.com/zmkfirmware/zmk.git
   git fetch upstream
   git checkout -B totem 484a0547078228f6957ed164523823e39fbedec4
   git apply ~/totem-zmk-config/patches/totem-ble.patch
   git commit -am "Totem BLE: accept-list + advertising throttle"
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
