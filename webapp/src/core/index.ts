/**
 * @rbgrid webapp core — the device-management layer (AWS-IoT-modelled, single device).
 * Built additively alongside the current app; pages migrate onto it phase by phase.
 *
 * P0 (this layer): transport abstraction (Direct ⊕ MQTT), schema/capability discovery,
 * and the ConfigShadow (desired/reported/delta reconciliation = the settings core).
 */
export * from './json';
export * from './schema/capabilities';
export * from './shadow/ConfigShadow';
export * from './transport/types';
export { DirectTransport } from './transport/DirectTransport';
export { MqttTransport } from './transport/MqttTransport';
export { createTransport } from './transport/TransportRouter';
export * from './jobs/JobTracker';
export * from './economics/types';
export * from './espnow/types';
export * from './provisioning/ProvisioningFSM';
export * from './auth/TokenStore';
export { DeviceSession } from './session/DeviceSession';
export type { DeviceSessionOptions } from './session/DeviceSession';
export * from './react/DeviceProvider';
export * from './mocks';
