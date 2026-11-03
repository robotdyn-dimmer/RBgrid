import { setFormat, getFormat, intlLocale, dateLocale, setLang, t } from './i18n';
import { formatSample } from '../components/settings/InterfacePanel';

describe('number/date format (Настройки → Интерфейс)', () => {
  afterEach(() => setFormat('auto'));
  it('auto follows the language; an explicit format wins and is kept in this browser', async () => {
    await setLang('ru');
    expect(intlLocale()).toBe('ru-RU');
    setFormat('en-US');
    expect(intlLocale()).toBe('en-US');
    expect(localStorage.getItem('rbgrid.format')).toBe('en-US');
    setFormat('auto');
    expect(getFormat()).toBe('auto');
    expect(localStorage.getItem('rbgrid.format')).toBeNull();
    setFormat('xx-YY');
    expect(getFormat()).toBe('auto');
  });
  it('plural forms follow the language, not the format', async () => {
    await setLang('ru');
    setFormat('en-US');
    // en-US has no «few»: the Russian form must still be picked by the Russian rules
    expect(t('cards.pointsOnline', { count: 3 })).toBe('3 точки на связи');
  });
  it('dates name months in the interface language, numbers keep the format (L3P-20)', async () => {
    await setLang('en');
    setFormat('de-DE');
    expect(intlLocale()).toBe('de-DE');
    const d = new Date(Date.UTC(2026, 9, 16, 17, 5));
    const s = d.toLocaleString(dateLocale(), { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit', timeZone: 'UTC' });
    expect(s).toContain('Oct');
    expect(s).toContain('17:05');
    setFormat('en-US');
    expect(d.toLocaleString(dateLocale(), { hour: '2-digit', minute: '2-digit', timeZone: 'UTC' })).toMatch(/PM/);
    setFormat('auto');
    expect(dateLocale()).toBe(intlLocale());
    await setLang('ru');
  });
  it('the sample shows the decimal mark and the date order', () => {
    expect(formatSample('de-DE')).toMatch(/^1\.234,5 · 16\.10\.2026/);
    expect(formatSample('en-US')).toMatch(/^1,234\.5 · 10\/16\/2026/);
  });
});
