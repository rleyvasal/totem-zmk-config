import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parseKeymap, applyBindings, parseBindingsBlock } from "./js/keymap.js";
import { parsePhysicalLayout } from "./js/layout.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const keymap = readFileSync(join(root, "config/totem.keymap"), "utf8");
const dtsi = readFileSync(join(root, "boards/shields/totem/totem.dtsi"), "utf8");

const keys = parsePhysicalLayout(dtsi);
if (keys.length !== 38) throw new Error(`layout keys ${keys.length}`);

const parsed = parseKeymap(keymap, 38);
if (parsed.layers.length !== 5) throw new Error(`layers ${parsed.layers.length}`);
if (parsed.layers[0].id !== "base_layer") throw new Error(`name ${parsed.layers[0].id}`);

const base = parsed.layers[0].bindings.map((b) => b.text);
if (base[0] !== "&kp Q") throw new Error(`P0 ${base[0]}`);
if (base[10] !== "&hml LGUI A") throw new Error(`P10 ${base[10]}`);
if (base[35] !== "&lt ADJ ENTER") throw new Error(`P35 ${base[35]}`);
if (base[18] !== "&hmr RALT I") throw new Error(`P18 ${base[18]}`);

const adj = parsed.layers[4].bindings.map((b) => b.text);
if (adj[20] !== "&sys_reset") throw new Error(`adj P20 ${adj[20]}`);
if (adj[27] !== "&bt BT_SEL 0") throw new Error(`adj P27 ${adj[27]}`);

const nav = parsed.layers[2].bindings[8];
if (!nav.text.includes("LC(LS(DOWN))") && parsed.layers[2].bindings[28].text !== "&kp LC(LS(DOWN))") {
  const found = parsed.layers[2].bindings.find((b) => b.text.includes("LC(LS(DOWN))"));
  if (!found) throw new Error("missing LC(LS(DOWN))");
}

parsed.layers[0].bindings[0].text = "&kp A";
const out = applyBindings(keymap, parsed.layers);
if (!out.includes("&kp A") || out === keymap) throw new Error("apply did not change Q");
if (!out.includes("tapping-term-ms 200 -> 280")) throw new Error("lost comment");
if (out.split("\n").length !== keymap.split("\n").length) throw new Error("rewrote line count");

const tiny = "bindings = <&kp Q  &hml LGUI A\n// c\n&trans>;";
const inner = tiny.indexOf("<") + 1;
const toks = parseBindingsBlock(tiny, inner, tiny.lastIndexOf(">"));
if (toks.map((t) => t.text).join("|") !== "&kp Q|&hml LGUI A|&trans") {
  throw new Error(`tiny ${toks.map((t) => t.text)}`);
}

console.log("ok", parsed.layers.map((l) => l.id).join(", "));
