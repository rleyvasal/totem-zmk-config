export const LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("").map((ch) => ({
  label: ch,
  binding: `&kp ${ch}`,
  category: "Alphabetical",
  description: `Letter ${ch}`,
}));
