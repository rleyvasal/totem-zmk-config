/**
 * Source-preserving ZMK keymap parser.
 * Finds each layer's bindings = < ... >; and records exact spans so a save
 * rewrites only the binding tokens, never comments or formatting.
 */

const BINDING_START = "bindings = <";

function skipLineComment(src, i, end) {
  if (src[i] === "/" && src[i + 1] === "/") {
    while (i < end && src[i] !== "\n") i++;
  }
  return i;
}

function readIdent(src, i, end) {
  if (i >= end || !/[A-Za-z_]/.test(src[i])) return i;
  i++;
  while (i < end && /[A-Za-z0-9_]/.test(src[i])) i++;
  return i;
}

function readNumber(src, i, end) {
  if (i >= end || !/\d/.test(src[i])) return i;
  while (i < end && /\d/.test(src[i])) i++;
  return i;
}

function readParenGroup(src, i, end) {
  if (src[i] !== "(") return i;
  let depth = 1;
  i++;
  while (i < end && depth > 0) {
    if (src[i] === "(") depth++;
    else if (src[i] === ")") depth--;
    i++;
  }
  return i;
}

function readArg(src, i, end) {
  if (/[A-Za-z_]/.test(src[i])) {
    i = readIdent(src, i, end);
    if (src[i] === "(") i = readParenGroup(src, i, end);
    return i;
  }
  if (/\d/.test(src[i])) return readNumber(src, i, end);
  return i;
}

export function parseBindingsBlock(src, start, end) {
  const bindings = [];
  let i = start;
  while (i < end) {
    i = skipLineComment(src, i, end);
    if (i >= end) break;
    if (src[i] === "&") {
      const tokenStart = i;
      i++;
      i = readIdent(src, i, end);
      while (i < end) {
        let j = i;
        while (j < end && /[ \t]/.test(src[j])) j++;
        if (j < end && src[j] === "/" && src[j + 1] === "/") break;
        if (j >= end || src[j] === "&" || src[j] === ">" || src[j] === "\n") break;
        const next = readArg(src, j, end);
        if (next === j) break;
        i = next;
      }
      bindings.push({
        start: tokenStart,
        end: i,
        text: src.slice(tokenStart, i),
      });
      continue;
    }
    i++;
  }
  return bindings;
}

function layerNameBefore(src, bindIdx) {
  const before = src.slice(Math.max(0, bindIdx - 240), bindIdx);
  const match = before.match(/([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*$/s);
  return match ? match[1] : `layer_${bindIdx}`;
}

export function parseKeymap(src, expectedCount = 38) {
  const layers = [];
  let searchFrom = 0;
  while (true) {
    const bindIdx = src.indexOf(BINDING_START, searchFrom);
    if (bindIdx < 0) break;
    const innerStart = bindIdx + BINDING_START.length;
    const close = src.indexOf(">;", innerStart);
    if (close < 0) break;
    const bindings = parseBindingsBlock(src, innerStart, close);
    if (bindings.length === expectedCount) {
      layers.push({
        id: layerNameBefore(src, bindIdx),
        bindings,
      });
    }
    searchFrom = close + 2;
  }
  return { source: src, layers };
}

export function applyBindings(original, layers) {
  const replacements = [];
  for (const layer of layers) {
    for (const b of layer.bindings) {
      if (b.text !== original.slice(b.start, b.end)) {
        replacements.push({ start: b.start, end: b.end, text: b.text });
      }
    }
  }
  replacements.sort((a, b) => b.start - a.start);
  let out = original;
  for (const r of replacements) {
    out = out.slice(0, r.start) + r.text + out.slice(r.end);
  }
  return out;
}

export function bindingLabel(text) {
  const parts = text.trim().split(/\s+/);
  if (parts.length === 0) return "?";
  if (parts[0] === "&trans") return "▽";
  if (parts[0] === "&none") return "∅";
  if (parts[0] === "&sys_reset") return "RST";
  if (parts[0] === "&host_log_dump") return "LOG";
  if (parts[0] === "&mac_lock") return "Mac";
  if (parts[0] === "&win_lock") return "Win";
  if (parts[0] === "&studio_unlock") return "STU";
  if (parts[0] === "&bt") {
    if (parts[1] === "BT_SEL") return `BT${parts[2] ?? ""}`;
    if (parts[1] === "BT_CLR") return "BTx";
    return "BT";
  }
  const last = parts[parts.length - 1];
  const pretty = {
    BSPC: "⌫",
    SPACE: "␣",
    ENTER: "⏎",
    ESC: "Esc",
    LSHFT: "⇧",
    RSHFT: "⇧",
    LCTRL: "Ctrl",
    RCTRL: "Ctrl",
    LALT: "Alt",
    RALT: "Alt",
    LGUI: "GUI",
    RGUI: "GUI",
    LBKT: "[",
    RBKT: "]",
    SEMI: ";",
    COMMA: ",",
    DOT: ".",
    FSLH: "/",
    BSLH: "\\",
    UNDER: "_",
    MINUS: "-",
    PLUS: "+",
    STAR: "*",
    HASH: "#",
    AT: "@",
    GRAVE: "`",
    SQT: "'",
    DQT: '"',
    PIPE: "|",
    AMPS: "&",
    CARET: "^",
    EXCL: "!",
    TILDE: "~",
    DLLR: "$",
    PRCNT: "%",
    EQUAL: "=",
    LPAR: "(",
    RPAR: ")",
    TAB: "Tab",
    DEL: "Del",
    INS: "Ins",
    HOME: "Home",
    END: "End",
    UP: "↑",
    DOWN: "↓",
    LEFT: "←",
    RIGHT: "→",
    PG_UP: "PgUp",
    PG_DN: "PgDn",
    C_VOL_UP: "Vol+",
    C_VOL_DN: "Vol-",
    C_MUTE: "Mute",
    C_BRI_UP: "Bri+",
    C_BRI_DN: "Bri-",
    C_PP: "⏯",
    C_PREV: "⏮",
    C_NEXT: "⏭",
    CAPS: "Caps",
    N0: "0",
    N1: "1",
    N2: "2",
    N3: "3",
    N4: "4",
    N5: "5",
    N6: "6",
    N7: "7",
    N8: "8",
    N9: "9",
  };
  if (pretty[last]) return pretty[last];
  if (/^N\d+$/.test(last)) return last.slice(1);
  if (last.length === 1) return last;
  return last.replace(/_/g, " ");
}

export function bindingHoldHint(text) {
  const parts = text.trim().split(/\s+/);
  if (parts[0] === "&hml" || parts[0] === "&hmr") return parts[1] || "";
  if (parts[0] === "&lt") return parts[1] || "";
  if (parts[0] === "&sk") return "sticky";
  return "";
}

/** Positions used by left-half recovery / combos. Warn before overwriting. */
export const PROTECTED = {
  0: "Q+W ESC combo",
  1: "Q+W ESC combo",
  8: "U+Y ñ combo",
  9: "U+Y ñ combo",
  20: "[+Z reset / [+X dump",
  21: "[+Z reset",
  22: "[+X dump",
  26: "N+M dictation combo",
  27: "N+M dictation combo",
};
