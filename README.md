# Totem ZMK Configuration

ZMK firmware for the [GEIGEIGEIST Totem](https://github.com/GEIGEIGEIST/totem)
split keyboard (38 keys, Seeed XIAO BLE), set up for two computers
(macOS + Windows), long battery life and a Colemak-DH layout.

It builds on **[ZMK Next](https://github.com/rleyvasal/zmk-next)**, a ZMK fork
with live keymap editing and the Bluetooth, battery and USB options this
keyboard turns on. Nothing is patched: the firmware is ZMK Next with options
set in `config/totem.conf`, plus the small Totem module in this repo.

## What's in this repo

| Path | Contents |
|---|---|
| `config/totem.keymap` | Keymap: layers, combos, macros |
| `config/totem.conf`, `config/totem_left.conf` | Settings for both halves, and for the left (central) half only |
| `config/west.yml` | Pins ZMK Next, [zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log) and a Zephyr fix branch to exact commits |
| `build.yaml` | The four images GitHub Actions builds |
| `boards/shields/totem/` | Totem shield definition |
| `src/`, `include/`, `Kconfig`, `CMakeLists.txt` | Totem module: exclusive host, reconnect watch, host event journal and diagnostics (`CONFIG_TOTEM_*`) |
| `snippets/totem-studio-rpc/` | ZMK Studio on the left half's USB port, with the log stream |
| `tools/read_diag_dump.py` | Reads the host event journal over USB |
| `.github/workflows/` | Firmware build, ZMK Next option checks, and a weekly ZMK Next update PR |
| `docs/` | [Diagnostics reference](docs/diagnostics.md) and how the old zmk-totem patches became ZMK Next options |

## Features

- **Two computers, one at a time.** Only the selected Bluetooth profile stays
  connected; the other computer is disconnected and can't reconnect in the
  background. Switching sends a short burst of fast advertising.
- **Wake from sleep.** The selected computer stays connected while the keyboard
  is idle, so a key press can wake it.
- **Battery saving.** Advertising stops after five minutes if the selected
  computer is away. While USB is plugged in, the keyboard doesn't advertise and
  types over USB.
- **Battery levels for both halves**, including charging state, sent to the
  computer over Bluetooth and to the configurator over USB.
- **Stable split link.** 15 ms connection interval with a 4 s timeout while
  typing, 60 ms when idle, and +8 dBm transmit power.
- **Live keymap editing.** ZMK Studio and Runtime Config on the left half, for
  the [ZMK Next Configurator](https://github.com/rleyvasal/zmk-next-configurator).
  The configurator also shows the keyboard's log and battery levels.
- **Diagnostics.** A journal of connections, disconnects, boots and battery
  survives restarts and power loss; read it over USB. See
  [docs/diagnostics.md](docs/diagnostics.md).

Battery life is about three days of daily use with these settings.

## Built on

| Project | Pinned in `config/west.yml` |
|---|---|
| [ZMK Next](https://github.com/rleyvasal/zmk-next) | Exact commit |
| [zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log) | Exact commit |
| [Zephyr](https://github.com/rleyvasal/zephyr) | `v4.1.0+zmk-fixes` plus two split-link controller fixes |

`.github/workflows/zmk-bump.yml` checks for new ZMK Next and zmk-studio-log
releases every Monday and opens a PR that moves the pins. The PR builds
firmware to test; merge it after a hardware check. The config pins exact
commits rather than ZMK Next's `v0.2-branch` release line, so the firmware
only changes when you merge one of these PRs.

Using another keyboard? Follow ZMK Next's
[quick start](https://github.com/rleyvasal/zmk-next/blob/main/docs/quick-start.md);
you don't need anything from this repo.

## Layers

| Layer | Contents |
|---|---|
| BASE | Colemak-DH with homerow mods (GUI, Alt, Shift, Ctrl) |
| CODE | Symbols and numbers |
| NAV | Arrows, navigation and mouse |
| MOD | Media, volume, brightness, Mac/Windows lock; Tab + Z/X/C select Bluetooth profiles 0/1/2 from the left half alone |
| ADJ | Function keys and Bluetooth profile select |

![Totem Keymap](totem-keymap.svg)

## Combos

| Keys | Action |
|---|---|
| Q + W | Esc |
| Q + B | Unlock ZMK Studio |
| K + H | Dictation (Alt+Space) |
| Y + ; | ñ |
| [ + Z | Reset the left half |
| [ + X | Dump the host event journal over USB |

Studio locking is off in this build (`CONFIG_ZMK_STUDIO_LOCKING=n`), so Q + B
only matters if you turn it on.

The keymap has no bond-clear keys on purpose. To clear a profile, assign
`&bt BT_CLR` to a key temporarily in the configurator, or use the settings
reset image.

## Switching computers

Press the target profile's `&bt BT_SEL` (ADJ layer, or Tab + Z/X/C). The
previous computer disconnects and the selected one connects; macOS usually
takes a few seconds, Windows longer. If the target shows Connected but doesn't
type, press the same `BT_SEL` again to force a reconnect.

## Building and flashing

Push to GitHub; Actions builds the firmware. All `.uf2` files you flash must
come from the **same** run.

| File | Flash to | Use |
|---|---|---|
| `totem_left-xiao_ble__zmk-zmk.uf2` | Left | Daily driver, with ZMK Studio and Runtime Config over USB |
| `totem_right-xiao_ble__zmk-zmk.uf2` | Right | Daily driver |
| `settings_reset-xiao_ble__zmk-zmk.uf2` | Both | Wipes settings and Bluetooth bonds; only for re-pairing |
| `totem_left_logging.uf2` | Left | Debugging only: logs over USB. Don't judge Bluetooth behavior on it |

To flash, double-tap the half's reset button and copy the file to the drive that
appears.

**Keymap or firmware update:** flash both halves with the new left and right files.

**Re-pairing** (first setup, or after Bluetooth changes):

1. Flash the settings reset file to **both** halves before flashing either one.
2. Flash the left, then the right firmware, from the same run.
3. Power both halves on together so the left half pairs with the right.

The left half is the central: it talks to the computer. The right half sends
its keys to the left over Bluetooth and doesn't type over its own USB.

To rename the keyboard, change `CONFIG_ZMK_KEYBOARD_NAME` in `config/totem.conf`,
re-pair with the steps above, and forget the old entry on each computer.
