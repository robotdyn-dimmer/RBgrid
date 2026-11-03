/**
 * useConnection — subscribe to the runtime device-connection state so the UI re-renders
 * when the user connects / switches device / enters demo mode.
 */
import { useEffect, useReducer } from 'react';
import {
  Connection, getConnection, isConnected, isDemo, onConnectionChange,
} from '../services/connection';

export function useConnection(): { connection: Connection | null; connected: boolean; demo: boolean } {
  const [, force] = useReducer((x: number) => x + 1, 0);
  useEffect(() => onConnectionChange(() => force()), []);
  return { connection: getConnection(), connected: isConnected(), demo: isDemo() };
}
