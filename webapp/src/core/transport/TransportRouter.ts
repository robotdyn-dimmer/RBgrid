/**
 * TransportRouter — picks the backend for a single-device target. Exactly one
 * transport is active at a time (operator decision 2026-06-23). The domain layer
 * holds a `DeviceTransport` and never branches on which one it is.
 *
 * Per root's HTTPS answer: hosted webapp → broker (wss), local → direct.
 */
import { DeviceTransport, DeviceTarget } from './types';
import { DirectTransport } from './DirectTransport';
import { MqttTransport } from './MqttTransport';
import { CmdChannel } from './mqtt/CmdChannel';
import { BrowserMqttClient } from './mqtt/BrowserMqttClient';

export function createTransport(target: DeviceTarget): DeviceTransport {
  switch (target.kind) {
    case 'direct':
      return new DirectTransport();
    case 'mqtt': {
      if (!target.brokerUrl || !target.deviceMac) {
        throw new Error('MQTT transport needs brokerUrl + deviceMac');
      }
      const client = new BrowserMqttClient({
        url: target.brokerUrl,
        // broker credentials are passed via the target; never logged.
        password: target.token,
      });
      const channel = new CmdChannel(client, `rbgrid/${target.deviceMac.toLowerCase()}`);
      return new MqttTransport(channel);
    }
    default: {
      const exhaustive: never = target.kind;
      throw new Error(`Unknown transport kind: ${exhaustive}`);
    }
  }
}
