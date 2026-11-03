import { buildScene, sceneStructuralKey, sceneLoads, SCENE_MAX_LOADS } from './flowScene';
import { FlowFrame } from './flowFrame';

const frame = (over: Partial<FlowFrame> = {}): FlowFrame => ({
  grid: 4.2, solar: 0.9, house: 5.1, gridHasDirection: true,
  loads: [
    { id: 'a', name: 'Бойлер', icon: 'drop', v: 2.0 },
    { id: 'b', name: 'Кухня', icon: 'plug', v: 0.6 },
    { id: 'c', name: 'ЗС-1', icon: 'car', v: 3.3 },
  ],
  ...over,
});

describe('isometric home scene', () => {
  it('without generation: no panels, no solar cable, no solar label (spec §10.3)', () => {
    const s = buildScene(frame({ solar: null }));
    expect(s.svg).not.toContain('var(--solar)');
    expect(s.anchors.map((a) => a.id)).not.toContain('solar');
  });
  it('with generation draws the panels and anchors a solar label', () => {
    const s = buildScene(frame());
    expect(s.svg).toContain('var(--solar)');
    expect(s.anchors.map((a) => a.id)).toEqual(['grid', 'solar', 'house', 'c', 'a', 'b']);
  });
  it('reduced motion: no SMIL at all (spec §10.5)', () => {
    expect(buildScene(frame(), { motion: false }).svg).not.toContain('animateMotion');
    expect(buildScene(frame()).svg).toContain('animateMotion');
  });
  it('structural key ignores jitter inside a power bucket (in-place updates, §10.2)', () => {
    expect(sceneStructuralKey(frame())).toBe(sceneStructuralKey(frame({ grid: 4.3, house: 5.2, solar: 0.95 })));
    expect(sceneStructuralKey(frame())).not.toBe(sceneStructuralKey(frame({ solar: null })));
  });
  it('caps the objects, biggest first; the unmetered rest is never dropped (GEN-16)', () => {
    const many = Array.from({ length: 8 }, (_, i) => ({ id: `l${i}`, name: `L${i}`, icon: 'plug' as const, v: i }));
    const loads = sceneLoads(frame({ loads: [{ id: 'o', name: 'Без датчика', icon: 'plug', v: 9, synthetic: true }, ...many] }));
    expect(loads).toHaveLength(SCENE_MAX_LOADS);
    expect(loads[0].id).toBe('l7');
    const last = loads[loads.length - 1];
    expect(last.synthetic).toBe(true);
    // the rest = the unmetered 9 + the folded small points (0..3)
    expect(last.v).toBe(9 + 0 + 1 + 2 + 3);
  });
  it('aria text is escaped', () => {
    expect(buildScene(frame(), { aria: 'a "b" <c>' }).svg).toContain('aria-label="a &quot;b&quot; &lt;c&gt;"');
  });
});
