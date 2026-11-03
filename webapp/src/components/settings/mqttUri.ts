/**
 * The hub's MQTT broker is one string, `broker_uri` (mqtt-config): mqtt://host:port or
 * mqtts://user:pass@host:port. GET masks the credentials («mqtts://***@host»), and POSTing the mask
 * is refused — so the form takes host / port / TLS / login / password and rebuilds the URI.
 */
export interface BrokerForm {
  tls: boolean;
  host: string;
  port: string;
  user: string;
  pass: string;
  /** The hub holds credentials we can't see (masked) — they must be typed again to keep them. */
  maskedCreds: boolean;
}

export const EMPTY_BROKER: BrokerForm = { tls: false, host: '', port: '1883', user: '', pass: '', maskedCreds: false };

export function parseBrokerUri(uri: string | null | undefined): BrokerForm {
  const m = /^(mqtts?):\/\/(?:([^@/]*)@)?([^:/]+)(?::(\d+))?/.exec(String(uri ?? '').trim());
  if (!m) return { ...EMPTY_BROKER };
  const tls = m[1] === 'mqtts';
  const cred = m[2] ?? '';
  const masked = cred === '***';
  const [user, pass] = masked ? ['', ''] : [decodeURIComponent(cred.split(':')[0] ?? ''), decodeURIComponent(cred.split(':').slice(1).join(':'))];
  return { tls, host: m[3], port: m[4] ?? (tls ? '8883' : '1883'), user, pass, maskedCreds: masked };
}

/** null when the form can't make a URI the hub accepts (host required, port 1..65535). */
export function buildBrokerUri(f: BrokerForm): string | null {
  const host = f.host.trim();
  const port = Number(f.port);
  if (!host || /[\s/@:]/.test(host) || !Number.isInteger(port) || port < 1 || port > 65535) return null;
  if (f.pass && !f.user) return null; // a password needs a login
  const cred = f.user ? `${encodeURIComponent(f.user)}${f.pass ? `:${encodeURIComponent(f.pass)}` : ''}@` : '';
  const uri = `${f.tls ? 'mqtts' : 'mqtt'}://${cred}${host}:${port}`;
  return uri.length < 160 ? uri : null;
}
