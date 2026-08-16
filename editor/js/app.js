import {
  parseKeymap,
  applyBindings,
  bindingLabel,
  bindingHoldHint,
  PROTECTED,
} from "./keymap.js";
import { parsePhysicalLayout, layoutBounds } from "./layout.js";
import { LETTERS } from "./palette.js";

const KEY_COUNT = 38;
const LAYOUT_URL = "../boards/shields/totem/totem.dtsi";
const KEYMAP_URL = "../config/totem.keymap";

const $ = (id) => document.getElementById(id);

const state = {
  original: "",
  keymapPath: "totem.keymap",
  layers: [],
  keys: [],
  layer: 0,
  selected: new Set(),
  dirty: false,
  drag: null,
};

function setStatus(msg) {
  $("status").textContent = msg;
}

function setDirty(d) {
  state.dirty = d;
  $("dirty").hidden = !d;
}

function currentBindings() {
  return state.layers[state.layer]?.bindings ?? [];
}

function confirmProtected(index, action) {
  const why = PROTECTED[index];
  if (!why) return true;
  return window.confirm(
    `Position ${index} is protected (${why}).\n${action} anyway?`
  );
}

function selectOnly(index) {
  state.selected.clear();
  if (index != null) state.selected.add(index);
  renderKeyboard();
  renderInspect();
}

function renderLayers() {
  const wrap = $("layers");
  wrap.replaceChildren();
  state.layers.forEach((layer, i) => {
    const btn = document.createElement("button");
    btn.textContent = layer.id.replace(/_layer$/, "");
    btn.className = i === state.layer ? "active" : "";
    btn.addEventListener("click", () => {
      state.layer = i;
      renderLayers();
      renderKeyboard();
      renderInspect();
    });
    wrap.appendChild(btn);
  });
}

function renderInspect() {
  const el = $("inspect");
  if (state.selected.size === 0) {
    el.innerHTML = "<div>Click a key, or drag a letter onto one.</div>";
    return;
  }
  const idx = [...state.selected][0];
  const b = currentBindings()[idx];
  const prot = PROTECTED[idx];
  el.innerHTML = `
    <div>Selected: <strong>P${idx}</strong>${state.selected.size > 1 ? ` (+${state.selected.size - 1})` : ""}</div>
    <div>Current: <code>${b?.text ?? ""}</code></div>
    ${prot ? `<div class="warn">Protected: ${prot}</div>` : ""}
  `;
}

function renderPalette() {
  const q = $("search").value.trim().toLowerCase();
  const items = LETTERS.filter((it) => it.label.toLowerCase().includes(q));
  const wrap = $("palette");
  wrap.replaceChildren();
  for (const item of items) {
    const sw = document.createElement("div");
    sw.className = "swatch";
    sw.textContent = item.label;
    sw.title = item.binding;
    sw.dataset.binding = item.binding;
    sw.addEventListener("pointerdown", (ev) => startPaletteDrag(ev, item));
    sw.addEventListener("click", (ev) => {
      if (state.drag) return;
      ev.preventDefault();
      applyToSelected(item.binding);
    });
    wrap.appendChild(sw);
  }
}

function renderKeyboard() {
  const svg = $("keyboard");
  const keys = state.keys;
  const bindings = currentBindings();
  if (keys.length === 0) {
    svg.replaceChildren();
    return;
  }
  const box = layoutBounds(keys);
  svg.setAttribute("viewBox", `${box.minX} ${box.minY} ${box.width} ${box.height}`);
  svg.replaceChildren();

  keys.forEach((k, i) => {
    const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
    const cx = k.x + k.w / 2;
    const cy = k.y + k.h / 2;
    if (k.r) g.setAttribute("transform", `rotate(${k.r} ${k.rx || cx} ${k.ry || cy})`);

    const rect = document.createElementNS("http://www.w3.org/2000/svg", "rect");
    rect.setAttribute("x", k.x);
    rect.setAttribute("y", k.y);
    rect.setAttribute("width", k.w);
    rect.setAttribute("height", k.h);
    rect.setAttribute("class", "key-hit");
    if (state.selected.has(i)) rect.classList.add("selected");
    if (PROTECTED[i]) rect.classList.add("protected");
    rect.dataset.index = String(i);

    const label = document.createElementNS("http://www.w3.org/2000/svg", "text");
    label.setAttribute("x", cx);
    label.setAttribute("y", cy + 2);
    label.setAttribute("class", "key-label");
    label.textContent = bindingLabel(bindings[i]?.text ?? "");

    const hold = bindingHoldHint(bindings[i]?.text ?? "");
    if (hold) {
      const ht = document.createElementNS("http://www.w3.org/2000/svg", "text");
      ht.setAttribute("x", cx);
      ht.setAttribute("y", k.y + 22);
      ht.setAttribute("class", "key-hold");
      ht.textContent = hold;
      g.append(rect, ht, label);
    } else {
      g.append(rect, label);
    }

    const idx = document.createElementNS("http://www.w3.org/2000/svg", "text");
    idx.setAttribute("x", k.x + 10);
    idx.setAttribute("y", k.y + k.h - 10);
    idx.setAttribute("class", "key-idx");
    idx.textContent = String(i);
    g.append(idx);

    rect.addEventListener("pointerdown", (ev) => startKeyDrag(ev, i));
    svg.appendChild(g);
  });
}

function keyIndexFromPoint(clientX, clientY) {
  const el = document.elementFromPoint(clientX, clientY);
  if (el?.dataset?.index != null) return Number(el.dataset.index);
  return null;
}

function startPaletteDrag(ev, item) {
  ev.preventDefault();
  ev.currentTarget.setPointerCapture(ev.pointerId);
  state.drag = { kind: "palette", binding: item.binding, label: item.label, ghost: makeGhost(item.label, ev) };
  window.addEventListener("pointermove", onDragMove);
  window.addEventListener("pointerup", onDragUp, { once: true });
}

function startKeyDrag(ev, index) {
  ev.preventDefault();
  ev.currentTarget.setPointerCapture(ev.pointerId);
  if (ev.shiftKey) {
    if (state.selected.has(index)) state.selected.delete(index);
    else state.selected.add(index);
  } else if (!state.selected.has(index)) {
    selectOnly(index);
  } else {
    renderKeyboard();
    renderInspect();
  }
  const text = currentBindings()[index]?.text ?? "";
  state.drag = {
    kind: ev.metaKey || ev.ctrlKey ? "copy" : "swap",
    from: index,
    label: bindingLabel(text),
    ghost: makeGhost(bindingLabel(text), ev),
  };
  window.addEventListener("pointermove", onDragMove);
  window.addEventListener("pointerup", onDragUp, { once: true });
}

function makeGhost(label, ev) {
  const g = document.createElement("div");
  g.className = "ghost";
  g.textContent = label;
  g.style.left = `${ev.clientX + 8}px`;
  g.style.top = `${ev.clientY + 8}px`;
  document.body.appendChild(g);
  return g;
}

function onDragMove(ev) {
  if (!state.drag) return;
  state.drag.ghost.style.left = `${ev.clientX + 8}px`;
  state.drag.ghost.style.top = `${ev.clientY + 8}px`;
  const over = keyIndexFromPoint(ev.clientX, ev.clientY);
  for (const rect of document.querySelectorAll(".key-hit")) {
    rect.classList.toggle("drop-target", Number(rect.dataset.index) === over);
  }
}

function onDragUp(ev) {
  window.removeEventListener("pointermove", onDragMove);
  const drag = state.drag;
  state.drag = null;
  drag?.ghost.remove();
  for (const rect of document.querySelectorAll(".key-hit")) rect.classList.remove("drop-target");
  const over = keyIndexFromPoint(ev.clientX, ev.clientY);
  if (over == null || !drag) return;
  if (drag.kind === "palette") {
    assignBinding(over, drag.binding);
    selectOnly(over);
    return;
  }
  if (drag.from === over) {
    selectOnly(over);
    return;
  }
  if (drag.kind === "copy") {
    const src = currentBindings()[drag.from];
    if (src) assignBinding(over, src.text);
  } else {
    swapBindings(drag.from, over);
  }
  selectOnly(over);
}

function assignBinding(index, text) {
  if (!confirmProtected(index, `Replace with ${text}`)) return;
  const b = currentBindings()[index];
  if (!b) return;
  b.text = text;
  setDirty(true);
  renderKeyboard();
  renderInspect();
  setStatus(`P${index} → ${text}`);
}

function applyToSelected(text) {
  if (state.selected.size === 0) {
    setStatus("Select a key first, or drag the letter onto one.");
    return;
  }
  for (const i of state.selected) assignBinding(i, text);
}

function swapBindings(a, b) {
  if (!confirmProtected(a, "Swap") || !confirmProtected(b, "Swap")) return;
  const layer = currentBindings();
  const tmp = layer[a].text;
  layer[a].text = layer[b].text;
  layer[b].text = tmp;
  setDirty(true);
  renderKeyboard();
  setStatus(`Swapped P${a} and P${b}`);
}

function downloadText(name, text, type = "text/plain") {
  const blob = new Blob([text], { type });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  URL.revokeObjectURL(a.href);
}

function saveKeymap() {
  const text = applyBindings(state.original, state.layers);
  downloadText(state.keymapPath, text);
  setStatus(`Downloaded ${state.keymapPath}. Replace config/totem.keymap, then run editor/export-svg.sh for the drawing.`);
}

function svgMarkup() {
  const svg = $("keyboard");
  return `<?xml version="1.0" encoding="UTF-8"?>\n${svg.outerHTML}`;
}

async function loadText(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  return res.text();
}

function loadKeymapText(text, name = "totem.keymap") {
  const parsed = parseKeymap(text, KEY_COUNT);
  if (parsed.layers.length === 0) {
    throw new Error(`No layer with exactly ${KEY_COUNT} bindings found.`);
  }
  state.original = text;
  state.keymapPath = name;
  state.layers = parsed.layers;
  state.layer = 0;
  state.selected.clear();
  setDirty(false);
  renderLayers();
  renderKeyboard();
  renderInspect();
  setStatus(`Loaded ${name}: ${parsed.layers.length} layers, ${KEY_COUNT} keys.`);
}

async function loadRepo() {
  const [dtsi, keymap] = await Promise.all([loadText(LAYOUT_URL), loadText(KEYMAP_URL)]);
  state.keys = parsePhysicalLayout(dtsi);
  if (state.keys.length !== KEY_COUNT) {
    throw new Error(`Layout has ${state.keys.length} keys, expected ${KEY_COUNT}.`);
  }
  loadKeymapText(keymap, "totem.keymap");
}

$("search").addEventListener("input", renderPalette);
$("download").addEventListener("click", saveKeymap);
$("svg").addEventListener("click", () => downloadText("totem-keymap-editor.svg", svgMarkup(), "image/svg+xml"));
$("reload").addEventListener("click", () => {
  loadRepo().catch((err) => setStatus(err.message));
});
$("open-file").addEventListener("change", async (ev) => {
  const file = ev.target.files?.[0];
  if (!file) return;
  try {
    loadKeymapText(await file.text(), file.name);
  } catch (err) {
    setStatus(err.message);
  }
});

renderPalette();
loadRepo().catch((err) => {
  setStatus(`${err.message} Serve from the repo root: python3 editor/serve.py`);
});
