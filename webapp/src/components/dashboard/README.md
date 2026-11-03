# Dashboard Components

This directory contains components specific to the RBGrid dashboard interface.

## Purpose
- Components for displaying real-time telemetry data
- Power monitoring widgets and cards
- System status indicators
- Quick action components

## Structure
- `PowerMonitor/` - Real-time power monitoring components
- `StatusCards/` - Device and system status display cards
- `QuickActions/` - Action buttons and controls
- `Summary/` - Summary statistics and overview components

## Guidelines
- Components should integrate with RBPower hooks
- Support real-time data updates via WebSocket
- Handle loading and error states gracefully
- Optimize for performance with React.memo where appropriate