/**
 * morphSvg — apply a freshly built SVG string to a host IN PLACE (spec §2.3 / §10.2: schemas are
 * never recreated for value changes). When the new markup has the same element tree (same tags,
 * same order) as what is mounted, only attributes and text nodes are copied over — the <svg> and
 * every child keep their identity (no flicker, SMIL timelines keep running). A different tree
 * (structure changed) is swapped in whole.
 *
 * The markup comes from our own builders (escaped text only), parsed in an inert <template>.
 */
function sameShape(a: Element, b: Element): boolean {
  if (a.tagName !== b.tagName || a.children.length !== b.children.length) return false;
  for (let i = 0; i < a.children.length; i++) if (!sameShape(a.children[i], b.children[i])) return false;
  return true;
}

function copy(dst: Element, src: Element): void {
  for (const at of Array.from(dst.attributes)) if (!src.hasAttribute(at.name)) dst.removeAttribute(at.name);
  for (const at of Array.from(src.attributes)) if (dst.getAttribute(at.name) !== at.value) dst.setAttribute(at.name, at.value);
  if (src.children.length === 0) {
    if (dst.textContent !== src.textContent) dst.textContent = src.textContent;
    return;
  }
  for (let i = 0; i < src.children.length; i++) copy(dst.children[i], src.children[i]);
}

/** Returns true when patched in place, false when the content was replaced. */
export function morphSvg(host: HTMLElement, markup: string): boolean {
  const cur = host.firstElementChild;
  const tpl = document.createElement('template');
  tpl.innerHTML = markup;
  const next = tpl.content.firstElementChild;
  if (cur && next && host.childElementCount === 1 && tpl.content.childElementCount === 1 && sameShape(cur, next)) {
    copy(cur, next);
    return true;
  }
  host.innerHTML = markup;
  return false;
}
