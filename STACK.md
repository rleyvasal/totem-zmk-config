# The Totem's firmware stack

This repo builds firmware for the Totem only. To use ZMK Next on another
keyboard, follow ZMK Next's
[quick start](https://github.com/rleyvasal/zmk-next/blob/main/docs/quick-start.md).
You don't need anything from this repo: the extras below that other keyboards
can use are ZMK Next options, and the rest is Totem-specific.

## What the Totem builds on

| Layer | Where it comes from | Can other keyboards use it? |
|---|---|---|
| Firmware | [ZMK Next](https://github.com/rleyvasal/zmk-next), pinned to an exact commit in `config/west.yml` | Yes, through the quick start |
| Bluetooth, battery and USB options | ZMK Next options, all off by default, turned on in `config/totem.conf` and `config/totem_left.conf` | Yes. See the quick start's "More ZMK Next options" |
| Keyboard log and battery levels over USB | [zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log) | Yes, quick start step 5 |
| Zephyr | [rleyvasal/zephyr](https://github.com/rleyvasal/zephyr) `v4.1.0+zmk-fixes` plus split-link controller fixes | Yes. Its supervision-reset fix is required for `CONFIG_ZMK_SPLIT_BLE_IDLE_INT` |
| Exclusive host, reconnect watch, host event journal, diagnostics | The Totem module in this repo (`src/`, `CONFIG_TOTEM_*`) | No. Tuned and tested only on the Totem |
| Studio on the XIAO's USB port | `snippets/totem-studio-rpc` | No. Other keyboards use ZMK Next's `zmk-next` snippet |

## History

The Totem used to build on [zmk-totem](https://github.com/rleyvasal/zmk-totem),
a ZMK fork carrying its patches. Those patches became ZMK Next options;
[docs/zmk-next-upstreaming-plan.md](docs/zmk-next-upstreaming-plan.md) records
how each one moved.
