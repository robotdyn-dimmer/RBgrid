# Time

## Overview

The hub needs the correct time for two things: to file each measurement under the right hour, day and month, and to pick the right tariff zone for its cost. It gets the time over the network (SNTP) from time servers, or once from your browser if the hub has no internet.

Open it in the app: **Settings → Time**. The setup wizard shows the time zone, the sync status and the two clock buttons; the NTP settings are only here.

## Fields

| Field | What it does |
|---|---|
| **Network time sync (SNTP)** | Turns time sync from internet time servers on or off. |
| **NTP server** | The main time server. Default: `pool.ntp.org`. |
| **Backup NTP server** | Used when the main server doesn't answer. Default: `time.nist.gov`. |
| **Resync interval, s** | How often the hub syncs again once it has the time. Default 3600 s (one hour); range 60–86400 s. |
| **Time zone** | Your local zone, with its daylight-saving rules. |

### Time zone

The time zone is a named zone with its daylight-saving rules, not a fixed UTC offset, so the clock changes on the right dates by itself.

- Pick one of the presets, for example "Central Europe — Berlin, Paris… (CET/CEST)".
- If your zone isn't in the list, choose **Other (POSIX TZ)** and enter the zone as a POSIX TZ string.
- The app suggests the zone of the browser you're using ("This browser's zone: … Use it").

### When changes apply

- **Time zone:** at once.
- **SNTP on/off and the NTP servers:** after a hub restart. The panel tells you when a restart is needed.

## Status

The panel shows the sync state and where the current time came from:

- **synchronized** or **synchronizing…**
- **source:** `SNTP` (a time server), `MANUAL` (set from a browser) or `NONE`.

After a failed sync the panel shows **not synchronized**.

Until the first successful sync, the hub tries again every 30 seconds. After that it syncs at the resync interval.

## No internet? Set the time from your browser

On a network without internet access, the hub can't reach a time server. Use **Use this device's time**: the hub takes the time from the browser you're using. The source then shows `MANUAL`.

**Sync with NTP** asks the hub to sync with the time server now.

The hub has no battery-backed clock. After a power cut without internet, its clock is unset again. Set it from the browser once more, or let it sync when the network is back.

## What happens while the clock isn't set

- **Live values** keep coming and are marked "time not synced".
- **History** records are written, but without a real date, and are flagged. History views and the CSV export skip them. Hours, days and months start counting once the clock is right.
- **Cost:** the hub still picks a tariff zone, using its unset clock, so cost in that period **may be priced in the wrong zone**. Set the time before relying on cost.

## The rbAmp modules' clocks

v1.1.0 doesn't set the clocks of the rbAmp modules. The hub's own clock is what files your readings and prices them.

## Network requirements

SNTP uses **UDP port 123**, outbound, to the servers you set. If your network blocks it, use a time server inside your network, or set the time from the browser.

## Troubleshooting

**The status never shows "synchronized"**
- Check that the hub is on a network with internet access (the **Status** card in Settings).
- Check that outbound UDP port 123 is allowed.
- Try another server, or a server's IP address instead of its name (a DNS problem).
- On a network without internet, use **Use this device's time**.

**Times in the app are off by an hour or more**
- Check the **Time zone**. A plain UTC offset doesn't change for daylight saving; use the named zone.

**Costs look wrong for a period**
- Check whether the clock was set during that period. Before the first sync after a power cut, readings can be priced in the wrong zone.
