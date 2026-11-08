import { espnowAvailable } from './available';
import { ONBOARDING_STEPS } from '../../onboarding/useOnboarding';

const mockHidden = { espnow: true };
jest.mock('../../constants/release', () => ({ get RELEASE_HIDDEN() { return mockHidden; } }));

describe('espnowAvailable (fw 1.1.7 `keys`, root espnow-nodes-ui)', () => {
  it('release build: only keys:true', () => {
    mockHidden.espnow = true;
    expect(espnowAvailable(true)).toBe(true);
    expect(espnowAvailable(false)).toBe(false);
    expect(espnowAvailable(undefined)).toBe(false);
  });
  it('preview build: a hub before 1.1.7 (no keys) too, but never keys:false', () => {
    mockHidden.espnow = false;
    expect(espnowAvailable(undefined)).toBe(true);
    expect(espnowAvailable(false)).toBe(false);
  });
  it('the wizard\'s Nodes step follows hasEspnow', () => {
    const step = ONBOARDING_STEPS.find((s) => s.key === 'nodes')!;
    expect(step.visible({ hasEspnow: true } as never)).toBe(true);
    expect(step.visible({ hasEspnow: false } as never)).toBe(false);
  });
});
