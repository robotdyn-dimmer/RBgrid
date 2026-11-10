# Time

## Overview

The hub needs the correct time for two things: to file each measurement under the right hour, day and month, and to pick the right tariff zone for its cost. It gets the time over the network (SNTP) from time servers, or once from your browser if the hub has no internet.

Open it in the app: **Settings → Time**. The setup wizard shows a compact panel: the time zone with the browser's suggestion, the sync status, the two clock buttons and **Save**. The NTP settings are only here.

## Fields

| Field | What it does |
|---|---|
| **Network time sync (SNTP)** | Turns time sync from internet time servers on or off. |
| **NTP server** | The main time server. Default: `pool.ntp.org`. A host name or address, up to 63 characters, with no spaces and no "/": the field says "A server name, without spaces or “/”, up to 63 characters" when it isn't one, and "Required" when it is empty. |
| **Backup NTP server** | Used when the main server doesn't answer. Default: `time.nist.gov`. Empty, or a host under the same rule. |
| **Resync interval, s** | How often the hub syncs again once it has the time. Default 3600 s (one hour), and it has to be between **60 and 86400 s**: from app v1.1.6 the panel won't save a value outside that range, and takes whole seconds only: it shows "At least 60", "At most 86400" or "Required" under the field and leaves **Save** disabled until you fix it. On app **v1.1.5 and earlier** both the app and the hub took any number, and a value outside the range turned NTP sync off after the next restart. A hub still carrying such a value is brought back into range when it first boots on firmware v1.1.6. |
| **Time zone** | Your local zone, with its daylight-saving rules. |

### Time zone

The time zone is a named zone with its daylight-saving rules, not a fixed UTC offset, so the clock changes on the right dates by itself.

- Pick one of the presets, for example "Central Europe — Berlin, Paris… (CET/CEST)".
- If your zone isn't in the list, choose **Other (POSIX TZ)** and enter the zone as a POSIX TZ string. The string must carry its own daylight-saving rule.
- The list includes southern-hemisphere zones, where daylight saving runs the other way round the year: Sydney/Melbourne/Canberra/Hobart, Adelaide, Brisbane, Perth, Auckland/Wellington, Santiago, São Paulo/Buenos Aires and Johannesburg.
- The app suggests the zone of the browser you're using ("This browser's zone: … **Use it**"). The suggestion appears only when the app knows the browser's zone and it differs from the hub's. On app v1.1.4 and earlier, southern-hemisphere zones with daylight saving are not offered as presets and get no suggestion: pick a POSIX TZ string, or update the hub.

### Saving and when changes apply

The Time panel has its own **Save** button; it isn't part of the Settings bar at the top. Until you press it, the panel says "The zone isn't saved yet — press "Save"." When the hub has an admin token, saving needs it.

Once saved:

- **Time zone:** at once. It's the only time setting that applies live.
- **SNTP on/off, the time source, the NTP server, the backup NTP server and the resync interval:** after a hub restart. The panel then says "Time settings saved. Some settings take effect after a restart." with **Restart now**.

> ⚠️ **Updating from v1.1.5 or earlier? Open this page once afterwards.** On firmware **v1.1.5 and earlier** the hub stored this panel as one whole file, while the app sends only the fields you changed — so saving the **time zone** on its own wiped the NTP servers, and after the next restart the hub had no time sync at all: the clock stayed unset, history waited for a real date, and energy was priced from whatever time the hub thought it was. **Fixed in v1.1.6**, which merges what the app sends into what is already stored. A hub recovers at its first boot on v1.1.6 — the saved time zone is kept, and the servers go back to `pool.ntp.org` and `time.nist.gov`. If the hub restarted more than once in that state the time zone is gone too: set it again here. Treat costs recorded while the clock was unset as suspect, as with any reading taken before the first sync.
>
> **If your hub has to stay on older firmware**, update the app package anyway: the v1.1.6 package sends this panel in full to a hub on v1.1.5 or earlier, so the servers survive a save there too. What carried the risk was app packages **v1.1.2 to v1.1.5** talking to firmware v1.1.5 or earlier.

## Status

The panel shows the sync state and where the current time came from:

- **synchronized**: the hub has the time.
- **synchronizing…**: the hub is getting the time now.
- **not synchronized**: the clock has not been set yet, or the last sync failed.
- **source:** the word the hub reports:
  - `SNTP`: the time came from a time server.
  - `MANUAL`: a manual time — the clock was set from a browser with **Use this device's time**. From firmware v1.1.6 the hub counts that as a synchronization, so the status beside it reads **synchronized**.
  - `NONE`: the hub has no confirmed time source yet.

If the hub reports no status, the panel shows no state.

Until the first successful sync, the hub tries again every 30 seconds. After that it syncs at the resync interval.

## No internet? Set the time from your browser

On a network without internet access, the hub can't reach a time server. Use **Use this device's time**: the hub takes the time from the browser you're using. On firmware **v1.1.6** that counts as a synchronization — the status shows **synchronized** with source `MANUAL`, the setup wizard's time step is satisfied, and the hub files its records under real dates from then on. Nothing corrects the clock afterwards, so it still drifts.

> **On firmware v1.1.5 and earlier a browser-set clock did not count as one.** On a hub on Wi-Fi that couldn't reach a time server the panel went on reporting **not synchronized** with source `NONE`; on a hub with no network at all the status stayed **synchronized** but the source still read `NONE`. Either way the hub treated its own clock as unconfirmed, so the wizard's time step stayed unticked and stored records kept waiting for a real date instead of being filed. If that is your hub, updating is what fixes it.

**Sync with NTP** asks the hub to sync with the time server now.

The hub has no battery-backed clock. Its clock survives a restart, but not a power cut. After a power cut without internet, its clock is unset again. Set it from the browser once more, or let it sync when the network is back.

## What happens while the clock isn't set

- **Live values** keep coming. The app tells you the clock isn't set: on the Overview, **Imported today** reads "hub clock not set", and the **Today** card says that "today" counts from the hub's own last midnight or restart, not your calendar day. The wizard's last step shows "not synced".
- **The clock stays "not set".** The hub doesn't make up a date. It uses a manual time saved earlier only if that time is recent.
- **History** records made before the first sync wait in the hub's memory. At the first sync they get their real time stamps. If the wait is very long, the oldest of them are dropped. Records that still have no real date are flagged "clock not synced": the History chart and the CSV export leave them out, and the History table shows them marked "before sync".
- **Cost:** until the hub's clock is set, cost **may be priced in the wrong tariff zone** — the hub prices energy with whatever clock it has rather than waiting, and those costs are stored like any other. Set the time before relying on cost.

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
- On firmware **v1.1.5 and earlier** there are two causes, both fixed in **v1.1.6**: saving the time zone on its own wiped the NTP servers (see the note above), and a **Resync interval** outside 60–86400 s turned sync off at the next restart. Update the hub, then open this panel once and check the zone and the servers.
- Otherwise, check that the hub has internet access, that outbound UDP port 123 is allowed, and that the **Resync interval** is between 60 and 86400 s.

**The Time panel sometimes won't open**
- **Fixed in v1.1.6.** On **v1.1.5 and earlier**, opening **Settings → Time** on a hub with no reachable time server failed about one time in three while the hub was retrying NTP. Open it again, or update the hub.
