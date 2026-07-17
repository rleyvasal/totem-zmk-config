# Totem ZMK Configuration

Custom ZMK firmware for the [GEIGEIGEIST Totem](https://github.com/GEIGEIGEIST/totem) split keyboard, tuned for **two computers (macOS + Windows)**, **battery life**, and a Colemak-DH daily-driver layout.

## Features

- **Dual host Bluetooth** — exclusive-host module keeps only the active profile connected (no cross-talk)
- **Battery saving** — advertising throttle when the host is away; idle disconnect + “go dark” overnight without wake storms
- **Faster profile switch** — dense advertising boost after `&bt BT_SEL`
- **Dual battery monitoring** — reports both halves to the host
- **Colemak-DH** layout with homerow mods, mouse layer, and combos
- **ZMK Studio** — **disabled** (`CONFIG_ZMK_STUDIO=n`); re-enable in `config/totem.conf` if you want live keymap editing

## Dual computer + battery (overview)

| Piece | Role |
|---|---|
| `src/exclusive_host.c` | Evict non-active host on connect / profile switch |
| `patches/zmk-ble.patch` on fork `rleyvasal/zmk` | Throttle, idle go-dark, adv boost (in ZMK `ble.c`) |
| `config/west.yml` | Pins a **commit SHA** of the patched fork (reproducible builds) |

Human-readable fork branch names look like `zmk-optimized-<base-sha>`; west tracks the **SHA** of the applied tip. See `patches/README.md` and `.github/workflows/zmk-bump.yml` (stable-release bumps).

### Switching computers

On the ADJ layer, press the target profile’s `&bt BT_SEL`. The previous machine is disconnected automatically; the new one reconnects (typically a few seconds).

**Soft recovery:** press the **same** `BT_SEL` again if the target shows Connected but won’t type (common macOS half-dead link). That forces disconnect + re-advertise without a full re-pair. If it still won’t type: Forget on the host → `BT_CLR` on that profile → re-pair.

### Tuning timers (after a week of real use)

Defaults in `config/totem.conf` — change only after you’ve lived with them:

| Setting | Default | What it does |
|---|---|---|
| `CONFIG_TOTEM_ADV_THROTTLE_TIMEOUT_MIN` | 5 | Pause advertising after host away this long |
| `CONFIG_TOTEM_IDLE_DISCONNECT_MIN` | 20 | Drop host + go dark after no keypresses |
| `CONFIG_TOTEM_ADV_BOOST_SEC` | 12 | Dense 30–60 ms advertising after profile switch |

- First key(s) after idle/dark may be lost while reconnecting — expected.
- Windows reconnect is often slower than macOS (host stack); firmware boost helps discovery only.

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
- **Mac Lock / Win Lock** on MOD layer  

## Changing the Keyboard Name

1. Set `CONFIG_ZMK_KEYBOARD_NAME` in `config/totem.conf`  
2. Build, then **settings-reset both halves** before flashing new firmware  
3. Clear host bonds with `&bt BT_CLR_ALL` if needed  

The name is stored in settings; reset is required for a clean rename.
