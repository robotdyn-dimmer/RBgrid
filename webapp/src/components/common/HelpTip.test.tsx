import React from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { HelpTip } from './HelpTip';

describe('HelpTip (W3-2)', () => {
  it('opens a plain-words explanation with the doc page when one is known', () => {
    render(<HelpTip topic="defaultZone" />);
    fireEvent.click(screen.getByRole('button'));
    expect(screen.getByText(/Действует в часы и дни|Applies in hours/)).toBeInTheDocument();
    expect(screen.getByRole('link')).toHaveAttribute('href', 'https://www.rbgrid.io/webapp-tariff-configuration');
  });
  it('links the phase topic to the units page', () => {
    render(<HelpTip topic="phase" />);
    fireEvent.click(screen.getByRole('button'));
    expect(screen.getByRole('link')).toHaveAttribute('href', 'https://www.rbgrid.io/webapp-units-configuration');
  });
});
