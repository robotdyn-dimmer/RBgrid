import { morphSvg } from './morphSvg';

it('keeps the same <svg> when only values change, replaces it when the structure changes', () => {
  const host = document.createElement('div');
  morphSvg(host, '<svg viewBox="0 0 10 10"><rect width="4"/><text>1 кВт</text></svg>');
  const svg = host.firstElementChild;
  expect(morphSvg(host, '<svg viewBox="0 0 10 10"><rect width="6"/><text>2 кВт</text></svg>')).toBe(true);
  expect(host.firstElementChild).toBe(svg);
  expect(host.querySelector('rect')!.getAttribute('width')).toBe('6');
  expect(host.querySelector('text')!.textContent).toBe('2 кВт');
  expect(morphSvg(host, '<svg viewBox="0 0 10 10"><rect width="6"/><rect/><text>2</text></svg>')).toBe(false);
  expect(host.firstElementChild).not.toBe(svg);
});
