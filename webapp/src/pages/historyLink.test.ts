import { historyHref, parseHistoryLink, winForPeriod } from './historyLink';

describe('historyLink', () => {
  it('round-trips the parameters', () => {
    const href = historyHref({ point: '0006', win: 'today', metric: 'money', view: 'table' });
    expect(href).toBe('/history?point=0006&win=today&metric=money&view=table');
    expect(parseHistoryLink(new URLSearchParams(href.split('?')[1]))).toEqual({ point: '0006', win: 'today', metric: 'money', view: 'table' });
  });
  it('ignores unknown values and an empty query', () => {
    expect(parseHistoryLink(new URLSearchParams('win=decade&metric=x&view=pie'))).toBeNull();
    expect(parseHistoryLink(new URLSearchParams(''))).toBeNull();
    expect(historyHref({})).toBe('/history');
  });
  it('maps the point page periods', () => {
    expect([winForPeriod('hour'), winForPeriod('day'), winForPeriod('week')]).toEqual(['today', 'thisMonth', 'thisMonth']);
  });
});
