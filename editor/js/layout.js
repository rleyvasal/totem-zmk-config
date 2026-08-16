/**
 * Parse ZMK zmk,physical-layout keys from a .dtsi.
 * Each key is <&key_physical_attrs w h x y r rx ry> in centikey units (100 = 1u).
 */

export function parsePhysicalLayout(dtsi) {
  const keys = [];
  const re =
    /<&key_physical_attrs\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s*>/g;
  let match;
  while ((match = re.exec(dtsi))) {
    keys.push({
      w: Number(match[1]),
      h: Number(match[2]),
      x: Number(match[3]),
      y: Number(match[4]),
      r: Number(match[5]),
      rx: Number(match[6]),
      ry: Number(match[7]),
    });
  }
  return keys;
}

export function layoutBounds(keys, pad = 40) {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const k of keys) {
    minX = Math.min(minX, k.x);
    minY = Math.min(minY, k.y);
    maxX = Math.max(maxX, k.x + k.w);
    maxY = Math.max(maxY, k.y + k.h);
  }
  return {
    minX: minX - pad,
    minY: minY - pad,
    width: maxX - minX + pad * 2,
    height: maxY - minY + pad * 2,
  };
}
