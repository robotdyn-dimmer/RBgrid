import {
  provisioningView, PROVISIONING_STEPS, EMPTY_PROVISIONING_STATUS, ProvisioningStatus,
} from './ProvisioningFSM';

const status = (over: Partial<ProvisioningStatus> = {}): ProvisioningStatus => ({
  ...EMPTY_PROVISIONING_STATUS,
  ...over,
});

describe('provisioningView', () => {
  it('first boot → current step is wifi, 0 progress', () => {
    const v = provisioningView(EMPTY_PROVISIONING_STATUS);
    expect(v.currentStep).toBe('wifi');
    expect(v.complete).toBe(false);
    expect(v.progress).toBe(0);
  });

  it('advances to the first unsatisfied REQUIRED step, skipping satisfied ones', () => {
    const v = provisioningView(status({
      wifi_provisioned: true, time_synced: true, hardware_configured: true,
    }));
    expect(v.currentStep).toBe('units'); // hardware done → units is next required
  });

  it('skips optional steps when picking the current step', () => {
    // everything required done except tariff; nodes (optional) not paired
    const v = provisioningView(status({
      wifi_provisioned: true, time_synced: true, hardware_configured: true,
      units_bound: 2, nodes_paired: 0, tariff_set: false,
    }));
    expect(v.currentStep).toBe('tariff'); // not 'nodes' (optional)
  });

  it('complete when all required steps satisfied (even if optional ones are not)', () => {
    const v = provisioningView(status({
      wifi_provisioned: true, time_synced: true, hardware_configured: true,
      units_bound: 1, tariff_set: true, // nodes + security optional, not done
    }));
    expect(v.currentStep).toBeNull();
    expect(v.complete).toBe(true);
    expect(v.progress).toBe(1);
  });

  it('trusts the device device_ready gate', () => {
    const v = provisioningView(status({ device_ready: true }));
    expect(v.complete).toBe(true);
  });

  it('reports per-step satisfied flags + marks the current one', () => {
    const v = provisioningView(status({ wifi_provisioned: true }));
    const wifi = v.steps.find((s) => s.id === 'wifi')!;
    const time = v.steps.find((s) => s.id === 'time')!;
    expect(wifi.satisfied).toBe(true);
    expect(wifi.current).toBe(false);
    expect(time.current).toBe(true); // first unsatisfied required
  });

  it('marks optional steps', () => {
    const v = provisioningView(EMPTY_PROVISIONING_STATUS);
    expect(v.steps.find((s) => s.id === 'nodes')!.optional).toBe(true);
    expect(v.steps.find((s) => s.id === 'security')!.optional).toBe(true);
    expect(v.steps.find((s) => s.id === 'wifi')!.optional).toBe(false);
  });

  it('progress counts only required steps', () => {
    const v = provisioningView(status({ wifi_provisioned: true, time_synced: true }));
    // 5 required steps (wifi/time/hardware/units/tariff), 2 done
    expect(v.progress).toBeCloseTo(2 / 5);
    expect(PROVISIONING_STEPS.filter((s) => !s.optional)).toHaveLength(5);
  });
});
