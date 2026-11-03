/**
 * Escape a value for safe interpolation into an SVG/XML string that is later injected via
 * `innerHTML` (flow schemas). Escapes the five XML special characters so data-derived text
 * (unit names, short names, labels, ids) can never open a tag or break out of an attribute.
 */
const XML_ESC: Record<string, string> = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
};

export function escapeXml(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => XML_ESC[c]);
}
