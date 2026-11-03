/** UTF-8 byte length (no TextEncoder dependency — jsdom lacks it). Device name limits are bytes. */
export const utf8Len = (v: string): number => {
  let n = 0;
  for (const ch of v) {
    const c = ch.codePointAt(0) ?? 0;
    n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return n;
};
