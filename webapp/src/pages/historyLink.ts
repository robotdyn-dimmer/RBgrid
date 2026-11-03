/**
 * A link into «History» opened on a given point, window, metric and view — the «As a table» way
 * out of the Overview and point charts (charts audit §6: every chart has a table view, but only
 * History had one). History reads the parameters once and drops them from the address.
 */
import type { HistWindow } from './historyWindow';

export type HistoryLinkMetric = 'energy' | 'power' | 'voltage' | 'reactive' | 'money';
export interface HistoryLink { point?: string; win?: HistWindow; metric?: HistoryLinkMetric; view?: 'chart' | 'table' }

const WINS: HistWindow[] = ['today', 'yesterday', 'week', 'thisMonth', 'lastMonth', 'year'];
const METRICS: HistoryLinkMetric[] = ['energy', 'power', 'voltage', 'reactive', 'money'];

export function historyHref(l: HistoryLink): string {
  const q = new URLSearchParams();
  if (l.point) q.set('point', l.point);
  if (l.win) q.set('win', l.win);
  if (l.metric) q.set('metric', l.metric);
  if (l.view) q.set('view', l.view);
  const s = q.toString();
  return s ? `/history?${s}` : '/history';
}

/** The parameters History understands; anything unknown is ignored (null = no link at all). */
export function parseHistoryLink(q: URLSearchParams): HistoryLink | null {
  const out: HistoryLink = {};
  const point = q.get('point'), win = q.get('win'), metric = q.get('metric'), view = q.get('view');
  if (point) out.point = point;
  if (win && (WINS as string[]).includes(win)) out.win = win as HistWindow;
  if (metric && (METRICS as string[]).includes(metric)) out.metric = metric as HistoryLinkMetric;
  if (view === 'chart' || view === 'table') out.view = view;
  return Object.keys(out).length ? out : null;
}

/** The point page's trend period → the nearest History window (History has no week step: days). */
export function winForPeriod(p: 'hour' | 'day' | 'week' | string): HistWindow {
  return p === 'hour' ? 'today' : 'thisMonth';
}
