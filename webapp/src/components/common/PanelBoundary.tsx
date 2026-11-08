/**
 * PanelBoundary — one section that throws while rendering shows a note instead of taking the whole
 * page down (root espnow-panel-bench 07.11: a node with no `sync` blanked the Points page).
 */
import React from 'react';
import { Alert } from '@mui/material';
import { t } from '../../i18n';

interface Props { children: React.ReactNode; name: string }
interface State { failed: boolean }

export class PanelBoundary extends React.Component<Props, State> {
  state: State = { failed: false };

  static getDerivedStateFromError(): State {
    return { failed: true };
  }

  componentDidCatch(err: unknown): void {
    // eslint-disable-next-line no-console
    console.error(`[${this.props.name}] render failed`, err);
  }

  render(): React.ReactNode {
    if (this.state.failed) return <Alert severity="warning" sx={{ mb: 3 }}>{t('common.sectionFailed', { name: this.props.name })}</Alert>;
    return this.props.children;
  }
}
