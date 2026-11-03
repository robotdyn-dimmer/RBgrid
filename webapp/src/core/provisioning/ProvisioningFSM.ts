/**
 * ProvisioningFSM — the initialization / onboarding state machine (operator-emphasized).
 *
 * Drives the first-boot wizard AND the "re-run a step" path in settings. Each step's
 * "satisfied?" is derived from the device's reported provisioning status
 * (`GET /api/system/provisioning`, root-confirmed shape 2026-06-23), so the flow is
 * RESUMABLE — re-enter mid-onboarding and the FSM skips done steps. No client-side
 * progress guessing, no separate flag to keep in sync.
 *
 * The model = AWS-IoT JITP provisioning: a resumable claim/onboard sequence with a
 * "device ready" gate. Pure (status in → derived view out) so it's testable.
 */

/** Reported provisioning status — GET /api/system/provisioning (root-confirmed). */
export interface ProvisioningStatus {
  wifi_provisioned: boolean;
  time_synced: boolean;
  hardware_configured: boolean;
  units_bound: number;
  nodes_paired: number;
  tariff_set: boolean;
  token_set: boolean;
  https_enabled: boolean;
  device_ready: boolean;
}

export type ProvisioningStepId =
  | 'wifi' | 'time' | 'hardware' | 'units' | 'nodes' | 'tariff' | 'security';

export interface ProvisioningStep {
  id: ProvisioningStepId;
  label: string;
  /** Optional steps don't block the "ready" sequence (e.g. a hub with only local units). */
  optional: boolean;
  satisfied: (s: ProvisioningStatus) => boolean;
  /** One-line guidance for the wizard. */
  hint: string;
}

export const PROVISIONING_STEPS: ProvisioningStep[] = [
  { id: 'wifi', label: 'Wi-Fi', optional: false, satisfied: (s) => s.wifi_provisioned,
    hint: 'Connect the device to your network.' },
  { id: 'time', label: 'Time', optional: false, satisfied: (s) => s.time_synced,
    hint: 'Sync the clock (NTP) so history and tariffs line up.' },
  { id: 'hardware', label: 'Hardware', optional: false, satisfied: (s) => s.hardware_configured,
    hint: 'Set the grid nominals (voltage / frequency).' },
  { id: 'units', label: 'Units', optional: false, satisfied: (s) => s.units_bound > 0,
    hint: 'Bind at least one sensor to a metering unit.' },
  { id: 'nodes', label: 'Nodes', optional: true, satisfied: (s) => s.nodes_paired > 0,
    hint: 'Pair ESP-NOW remote metering nodes (optional).' },
  { id: 'tariff', label: 'Tariff', optional: false, satisfied: (s) => s.tariff_set,
    hint: 'Set the tariff plan for cost & billing.' },
  { id: 'security', label: 'Security', optional: true, satisfied: (s) => s.token_set,
    hint: 'Set an API token (and HTTPS) to protect the device (optional on a trusted LAN).' },
];

export interface ProvisioningStepView {
  id: ProvisioningStepId;
  label: string;
  optional: boolean;
  satisfied: boolean;
  hint: string;
  /** This is the step the wizard should focus. */
  current: boolean;
}

export interface ProvisioningView {
  steps: ProvisioningStepView[];
  /** First unsatisfied required step, or null when all required steps are done. */
  currentStep: ProvisioningStepId | null;
  /** Required steps done (mirrors the device's own `device_ready` when present). */
  complete: boolean;
  /** Fraction of required steps satisfied (0..1) for a progress bar. */
  progress: number;
}

/** Build the wizard view from a reported status. */
export function provisioningView(
  status: ProvisioningStatus,
  steps: ProvisioningStep[] = PROVISIONING_STEPS,
): ProvisioningView {
  const required = steps.filter((s) => !s.optional);
  const firstUnsatisfied = required.find((s) => !s.satisfied(status));
  const currentStep = firstUnsatisfied?.id ?? null;
  const requiredDone = required.filter((s) => s.satisfied(status)).length;

  return {
    steps: steps.map((s) => ({
      id: s.id,
      label: s.label,
      optional: s.optional,
      satisfied: s.satisfied(status),
      hint: s.hint,
      current: s.id === currentStep,
    })),
    currentStep,
    // Trust the device's own gate when it says ready; else fall back to required-steps done.
    complete: status.device_ready || (currentStep === null),
    progress: required.length ? requiredDone / required.length : 1,
  };
}

/** An all-false status — first boot / before any GET. */
export const EMPTY_PROVISIONING_STATUS: ProvisioningStatus = {
  wifi_provisioned: false,
  time_synced: false,
  hardware_configured: false,
  units_bound: 0,
  nodes_paired: 0,
  tariff_set: false,
  token_set: false,
  https_enabled: false,
  device_ready: false,
};
