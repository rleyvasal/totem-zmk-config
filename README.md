# Totem ZMK Configuration

Custom ZMK firmware for the [GEIGEIGEIST Totem](https://github.com/GEIGEIGEIST/totem) split keyboard, tuned for **two computers (macOS + Windows)**, **battery life**, and a Colemak-DH daily-driver layout.

## Features

- **Dual host Bluetooth** — exclusive-host keeps only the selected profile connected, preventing inactive-host wake and dual-link drain
- **Wake from sleep** — the selected BLE link stays connected through keyboard idle so a key can wake a supported host
- **Battery saving** — advertising stops after five minutes when the selected host is genuinely absent
- **Faster profile switch** — dense advertising boost after `&bt BT_SEL`
- **Split-link reliability** — 15 ms connection interval, 4 s supervision, +8 dBm TX; homerow mods decide on release so a delayed split hop cannot latch a modifier
- **Colemak-DH** layout with homerow mods, mouse layer, and combos
- **ZMK Studio** — **disabled** (`CONFIG_ZMK_STUDIO=n`); re-enable in `config/totem.conf` if you want live keymap editing

## Dual computer + battery (overview)

| Piece | Role |
|---|---|
| `src/exclusive_host.c` | Disconnects non-active hosts so only the selected profile stays linked |
| `src/lazy_inactive_host.c` | Optional experimental multi-link mode; disabled by default |
| `patches/zmk-ble.patch` on fork `rleyvasal/zmk` | Finite advertising throttle and profile-switch boost (in ZMK `ble.c`) |
| `config/west.yml` | Pins a **commit SHA** of the patched fork (reproducible builds) |

Human-readable fork branch names look like `zmk-optimized-<base-sha>`; west tracks the **SHA** of the applied tip. See `patches/README.md` and `.github/workflows/zmk-bump.yml` (stable-release bumps).

### Switching computers

On the ADJ layer, press the target profile’s `&bt BT_SEL`. Exclusive-host disconnects the previous computer and connects the selected profile. Only one computer should remain connected.

**Soft recovery:** press the **same** `BT_SEL` again if the target shows Connected but won’t type (common macOS half-dead link). That forces disconnect + re-advertise without a full re-pair. If it still won’t type: Forget on the host → `BT_CLR` on that profile → re-pair.

**Profile switch time:** Every switch requires a real BLE reconnect. macOS often takes a few seconds and Windows can take longer because the host controls scanning. The firmware advertises densely for the first 20 seconds, then normally for up to the five-minute throttle limit.

**Wake behavior:** The selected host remains connected while the keyboard is idle, allowing a keypress to wake hosts whose Bluetooth radio is armed for wake. The inactive host is disconnected. If the selected computer powers down its Bluetooth radio during sleep, no disconnected keyboard can guarantee wake until that computer scans and reconnects.

**Host event log:** **off** on the production image (flash writes were a hang suspect). The `[` + `X` combo is a no-op until `CONFIG_TOTEM_HOST_EVENT_LOG=y`. Prefer the USB console boot lines (`Loaded … address`, `active=`) on a `totem_left_logging` flash if you need a dump; do **not** stay on the logging image while testing profile switches.

**Active-host isolation (FAL):** **off.** Field failure 2026-07-21 (Mac→Win OK, return to Mac broken). Exclusive-host eviction is the dual-host policy that survived a week of daily use. Do not enable `CONFIG_TOTEM_ACTIVE_ADV_FILTER` until Mac↔Win↔Mac is proven.

### Tuning timers (after a week of real use)

Defaults in `config/totem.conf` — change only after you’ve lived with them. The
2026-08-15 baseline already includes a week of daily use; see `STABLE-BASELINE.md`.

| Setting | Default | What it does |
|---|---|---|
| `CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN` | 5 | Pause advertising after host away this long |
| `CONFIG_TOTEM_ADV_BOOST_SEC` | 20 | Dense 30–60 ms advertising after profile switch |

- A keypress after advertising has throttled resumes advertising; that first key may be lost while reconnecting.
- Windows reconnect is often slower than macOS (host stack); firmware boost helps discovery only.
- Battery is about **3 days** of daily use at +8 dBm / 15 ms split (Tuesday 100% → Friday 0%, week of 2026-08-11). That is the accepted cost of the split-link fix. Step TX down only after another clean week, and measure.

## Layers

### BASE - Colemak-DH
Main typing layer with homerow mods (GUI/Alt/Shift/Ctrl).

### CODE
Symbol layer for Python/JavaScript (brackets, numpad-style numbers, common symbols).

### NAV
Navigation and mouse control.

### MOD
Media, lock macros (Mac/Win), volume/brightness.

### ADJ
Function keys and Bluetooth profile select / clear. (No Studio unlock — Studio is off.)

## Keymap Visualization

![Totem Keymap](totem-keymap.svg)

## Installation

1. Fork this repository  
2. Enable GitHub Actions  
3. Edit `config/totem.keymap` / `config/totem.conf` as needed  
4. Push to build firmware  
5. Download artifacts and flash — **order matters** (below)  

CI also runs **Verify ZMK patch symbols** so a bad/unpatched west pin fails before a useless flash.

## Flashing & Re-pairing the Split Halves

> [!IMPORTANT]
> **Reset BOTH halves *before* flashing EITHER half.**  
> Resetting one half at a time can leave the left bonded to the right’s old identity.

All `.uf2` files must come from the **same** GitHub Actions run.

### Re-pairing (first setup, or after firmware/BLE changes)

1. **Reset BOTH halves** (bootloader, copy settings reset UF2 to each)  
2. **Flash firmware** left then right from the same build  
3. **Power both on together** so the left (central) bonds to the right  

### Routine keymap-only update

Skip settings reset; flash both halves with the new left/right UF2s.

> [!NOTE]
> **Left = central** (talks to the computer). **Right = peripheral** (relays keys over BLE; does not type over its own USB).

## Hardware

- **Keyboard:** GEIGEIGEIST Totem (38-key split)  
- **Controller:** Seeeduino XIAO BLE (nRF52840)  
- **Firmware:** ZMK (patched fork for dual-host / battery behavior)  

## Combos & macros

- **Q + W:** ESC  
- **N + M:** Dictation (Alt+Space)  
- **U + Y:** ñ  
- **[ + Z** (left half only): soft reset  
- **[ + X** (left half only): dump multi-profile host event log over USB serial  
- **Mac Lock / Win Lock** on MOD layer  

## Changing the Keyboard Name

1. Set `CONFIG_ZMK_KEYBOARD_NAME` in `config/totem.conf`  
2. Build, then **settings-reset both halves** before flashing new firmware  
3. Clear host bonds with `&bt BT_CLR_ALL` if needed  

The name is stored in settings; reset is required for a clean rename.
