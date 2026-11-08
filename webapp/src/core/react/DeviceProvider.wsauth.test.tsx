import React, { useEffect } from 'react';
import { render } from '@testing-library/react';
import { DeviceProvider } from './DeviceProvider';
import { hasWsAuthSource } from '../../services/websocket';
import { connectDevice } from '../../services/connection';
import { createTransport } from '../transport/TransportRouter';

/** root ws-ticket-app-check step 1: child effects run before the provider's — the ticket source
 *  must already be there when the first child connects the socket. */
it('the ws ticket source is installed before any child effect runs', () => {
  connectDevice('192.168.1.139');
  const seen: boolean[] = [];
  const Child = () => { useEffect(() => { seen.push(hasWsAuthSource()); }, []); return null; };
  const transport = createTransport({ kind: 'direct', baseUrl: 'http://192.168.1.139' });
  transport.getSchema = async () => null;
  transport.getProvisioning = async () => null;
  render(<DeviceProvider transport={transport}><Child /></DeviceProvider>);
  expect(seen).toEqual([true]);
});
