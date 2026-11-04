# Time

## Overview

The hub needs the correct time for two things: to file each measurement under the right hour, day and month, and to pick the right tariff zone for its cost. It gets the time over the network (SNTP) from time servers, or once from your browser if the hub has no internet.

Open it in the app: **Settings → Time**. The setup wizard shows a compact panel: the time zone with the browser's suggestion, the sync status, the two clock buttons and **Save**. The NTP settings are only here.

## Fields

| Field | What it does |
|---|---|
| **Network time sync (SNTP)** | Turns time sync from internet time servers on or off. |
| **NTP server** | The main time server. Default: `pool.ntp.org`. |
| **Backup NTP server** | Used when the main server doesn't answer. Default: `time.nist.gov`. |
| **Resync interval, s** | How often the hub syncs again once it has the time. Default 3600 s (one hour). Use a value between 60 and 86400 s. The app and the hub accept other values, but a value outside this range turns NTP sync off after the next restart (a known issue, to be fixed in an upcoming release). |
| **Time zone** | Your local zone, with its daylight-saving rules. |

### Time zone

The time zone is a named zone with its daylight-saving rules, not a fixed UTC offset, so the clock changes on the right dates by itself.

- Pick one of the presets, for example "Central Europe — Berlin, Paris… (CET/CEST)".
- If your zone isn't in the list, choose **Other (POSIX TZ)** and enter the zone as a POSIX TZ string. The string must carry its own daylight-saving rule.
- The app suggests the zone of the browser you're using ("This browser's zone: … **Use it**"). The suggestion appears only when the app knows the browser's zone and it differs from the hub's. Southern-hemisphere zones with daylight saving get no suggestion: pick them yourself.

### Saving and when changes apply

The Time panel has its own **Save** button; it isn't part of the Settings bar at the top. Until you press it, the panel says "The zone isn't saved yet — press "Save"." When the hub has an admin token, saving needs it.

Once saved:

- **Time zone:** at once. It's the only time setting that applies live.
- **SNTP on/off, the time source, the NTP server, the backup NTP server and the resync interval:** after a hub restart. The panel then says "Time settings saved. Some settings take effect after a restart." with **Restart now**.

## Status

The panel shows the sync state and where the current time came from:

- **synchronized**: the hub has the time.
- **synchronizing…**: the hub is getting the time now.
- **not synchronized**: the clock has not been set yet, or the last sync failed.
- **source:** the word the hub reports:
  - `SNTP`: the time came from a time server.
  - `MANUAL`: a manual time. With **synchronized**, it was set from a browser. With **not synchronized**, the hub is running on its own fallback and the clock isn't confirmed. The status tells the two apart.
  - `NONE`: the hub has no time source yet.

If the hub reports no status, the panel shows no state.

Until the first successful sync, the hub tries again every 30 seconds. After that it syncs at the resync interval.

## No internet? Set the time from your browser

On a network without internet access, the hub can't reach a time server. Use **Use this device's time**: the hub takes the time from the browser you're using. The status then shows **synchronized**, with source `MANUAL`.

**Sync with NTP** asks the hub to sync with the time server now.

The hub has no battery-backed clock. Its clock survives a restart, but not a power cut. After a power cut without internet, its clock is unset again. Set it from the browser once more, or let it sync when the network is back.

## What happens while the clock isn't set

- **Live values** keep coming. The app tells you the clock isn't set: on the Overview, **Imported today** reads "hub clock not set", and the **Today** card says that "today" counts from the hub's own last midnight or restart, not your calendar day. The wizard's last step shows "not synced".
- **The clock stays "not set".** The hub doesn't make up a date. It uses a manual time saved earlier only if that time is recent.
- **History** records made before the first sync wait in the hub's memory. At the first sync they get their real time stamps. If the wait is very long, the oldest of them are dropped. Records that still have no real date are flagged "clock not synced": the History chart and the CSV export leave them out, and the History table shows them marked "before sync".
- **Cost:** until the hub's clock is set, cost **may be priced in the wrong tariff zone** (a known issue, to be fixed in an upcoming release). Set the time before relying on cost.

## The rbAmp modules' clocks

The hub doesn't set the clocks of the rbAmp modules. The hub's own clock is what files your readings and prices them.

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
- When the hub's offset and your browser's differ by 30 minutes or more, the Overview says so and offers **Change the zone →**.

**Costs look wrong for a period**
- Check whether the clock was set during that period. Until the clock is set (for example after a power cut without internet), readings can be priced in the wrong zone.

**Time sync stopped working after a restart**
- Check the **Resync interval**. A value outside 60–86400 s turns NTP sync off after a restart. Set a value in that range, save and restart.
