# Use this stack on any ZMK keyboard

Totem is one consumer. The BLE policy, Studio log mux, and configurator work on Corne and other boards if you pin the same firmware and this module.

You need **two west.yml projects** and a few Kconfig flags. No Totem shield, no Totem keymap.

## 1. Pin firmware + this module

In your keyboard config `config/west.yml` (or repo-root `west.yml`):

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: rleyvasal
      url-base: https://github.com/rleyvasal
  projects:
    - name: zmk
      remote: rleyvasal
      repo-path: zmk-next
      revision: zmk-next-logs-batt
      import: app/west.yml
    - name: zmk-next-host
      remote: rleyvasal
      repo-path: totem-zmk-config
      revision: zmk-next-logs-batt
      path: modules/zmk-next-host
  self:
    path: config
```

`zmk-next` is the BLE + Runtime Config + Studio fork. `totem-zmk-config` is still the GitHub name of the **host-policy module** (exclusive-host, log mux). Only `boards/shields/totem` is Totem hardware; a Corne build does not use it.

Then `west update`.

## 2. Turn on the features you want

In `config/corne.conf` (or `boards/shields/corne/corne.conf`):

```
# Dual-computer BLE (optional)
CONFIG_ZMK_EXCLUSIVE_HOST=y
CONFIG_ZMK_RECONNECT_WATCH=y
CONFIG_TOTEM_ADV_THROTTLE=y
CONFIG_TOTEM_ADV_BOOST=y
CONFIG_TOTEM_RESELECT_RECONNECT=y

# Split battery in the configurator / logs (central half only)
CONFIG_ZMK_BATTERY_LOG=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y
```

Advertising throttle/boost symbols are still `CONFIG_TOTEM_ADV_*` inside zmk-next until that fork is renamed. They are not Totem-hardware-specific.

## 3. Studio + live log (optional)

Left/central half `build.yaml`:

```yaml
include:
  - board: nice_nano_v2
    shield: corne_left
    snippet: zmk-studio-one-cdc
    cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_STUDIO_LOCKING=n -DCONFIG_ZMK_RUNTIME_CONFIG=y
  - board: nice_nano_v2
    shield: corne_right
```

`zmk-studio-one-cdc` adds one CDC ACM and muxes printk onto Studio RPC (`'L'` log, `'B'` battery). It does **not** reference Totem UARTs.

**XIAO / boards with an extra console CDC:** keep using snippet `totem-studio-rpc` (disables the extra UART). nice!nano and most nRF52 boards should use `zmk-studio-one-cdc`.

## Configurator

1. Run [zmk-next-configurator](https://github.com/rleyvasal/zmk-next-configurator/tree/zmk-next-logs-batt) (`python3 apps/web/serve.py`).
2. Chrome → **Connect** (Web Serial) while the central half is on USB.
3. Geometry: put `zmk-map-layout.json` in your config repo, or **Load from GitHub**, or add `layouts/<board>.json` to the configurator.

Stock ZMK Studio firmware can change existing bindings only. Runtime Config (the cmake-args above) is what Apply uses for macros/combos/hold-taps.

## What stays keyboard-specific

| You still own | This stack does not pick |
|---|---|
| Board / shield (`nice_nano_v2` + `corne_left`) | Totem XIAO shield |
| Keymap | Colemak-DH Totem layout |
| TX power, split interval | Totem +8 dBm / 15 ms |
| Layout JSON | Totem 38-key geometry |

## Totem production

Unchanged: `config/totem.conf` still uses `CONFIG_TOTEM_*`, `build.yaml` still uses `totem-studio-rpc` + `xiao_ble//zmk` + `totem_left`.
