# Totem keymap editor

Visual editor for `config/totem.keymap`. Physical key positions come from
`boards/shields/totem/totem.dtsi` and do not move. Dragging changes only the
binding tokens.

This does **not** talk to ZMK Studio. Saving writes a `.keymap` file. A
firmware rebuild (or Restore Stock Settings, then flash) is what lasts on the
board. Studio remains the live try-path on `cu.usbmodem104`.

## Run

From the repo root:

```bash
python3 editor/serve.py
```

Open http://127.0.0.1:8765/editor/

## Milestone actions

- Layer tabs
- Drag A–Z from the palette onto a key → `&kp A`
- Click a palette letter with a key selected → same
- Drag one key onto another → swap bindings
- ⌘/Ctrl-drag a key onto another → copy binding
- Download the patched `totem.keymap` (comments and formatting kept)
- Download the current SVG view (quick preview)

Replace `config/totem.keymap` with the download, then:

```bash
editor/export-svg.sh
```

that regenerates `totem-keymap.svg` via keymap-drawer when it is installed.

Combo / recovery positions (`[`+`Z`, `[`+`X`, Q+W, …) ask before overwrite.

## Not in this milestone

GitHub login, hold-tap constructors, combos, live Studio apply, other keyboards.
