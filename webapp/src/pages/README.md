# Pages

This directory contains page-level components that represent full views in the RBGrid application.

## Purpose
- Complete page layouts combining multiple components
- Route-level components
- Page-specific logic and state management

## Structure
- `Dashboard/` - Main dashboard page
- `Configuration/` - Device configuration pages
- `Telemetry/` - Telemetry data viewing pages
- `Reports/` - Energy reports and analytics pages
- `Settings/` - Application settings pages

## Guidelines
- Pages should compose smaller components
- Handle route parameters and navigation
- Manage page-level state
- Implement proper loading and error boundaries
- Use lazy loading for better performance