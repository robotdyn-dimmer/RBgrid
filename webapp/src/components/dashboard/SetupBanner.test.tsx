import React from 'react';
import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { SetupBanner } from './SetupBanner';

const mockGetSystemSettings = jest.fn();
jest.mock('../../services/api', () => ({ getApiClient: () => ({ getSystemSettings: mockGetSystemSettings }) }));

describe('SetupBanner', () => {
  beforeEach(() => { window.localStorage.clear(); mockGetSystemSettings.mockReset(); });

  it('names the factory signs and links the wizard', async () => {
    mockGetSystemSettings.mockResolvedValue({ home_name: 'Default' });
    render(<MemoryRouter><SetupBanner placeholderTariff /></MemoryRouter>);
    await waitFor(() => expect(screen.getByRole('link').getAttribute('href')).toBe('/onboarding'));
    expect(screen.getByRole('alert').textContent).toMatch(/setupBanner\.name|name|названия/);
  });

  it('stays away once the site is named and the tariff is real', async () => {
    mockGetSystemSettings.mockResolvedValue({ home_name: 'Мой дом' });
    const { container } = render(<MemoryRouter><SetupBanner placeholderTariff={false} /></MemoryRouter>);
    await waitFor(() => expect(mockGetSystemSettings).toHaveBeenCalled());
    expect(container.textContent).toBe('');
  });
});
