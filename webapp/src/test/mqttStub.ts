/**
 * Jest stub for mqtt.js — mqtt v5 is ESM-only and CRA's jest doesn't transform it.
 * Tests never exercise the real client (they use fake brokers via the MqttLike
 * interface); the browser build uses the real mqtt. Mapped in via package.json
 * jest.moduleNameMapper.
 */
export function connect(): unknown {
  return {
    on() {},
    publish() {},
    subscribe() {},
    unsubscribe() {},
    end() {},
  };
}

const mqtt = { connect };
export default mqtt;
