# Web app FAQ

## Opening the app

**Where do I open the app?**
Two ways: at [webapp.rbgrid.io](https://webapp.rbgrid.io/) (Chrome or Edge, connect to the hub's address), or at the hub's own address once you've installed the app on it. → [Opening the app](opening-the-app.md)

**The hub's address opens, but there's no app (only a minimal page).**
The web app isn't part of the firmware file. Install `rbgrid-ui_<version>.zip` from the release onto the hub once, or open the app at [webapp.rbgrid.io](https://webapp.rbgrid.io/) instead. → [Opening the app](opening-the-app.md)

**webapp.rbgrid.io says no hub answers.**
- Your computer must be on the same network as the hub.
- Check the address: it's the hub's address on your home network, not `192.168.4.1`.
- When Chrome or Edge asked whether the site may access devices on your local network, you had to allow it. If you didn't, allow it in the browser's site settings (**Local network access**) and connect again.
- Firefox and Safari haven't been tested yet: use Chrome or Edge.

**Do I need internet for the app?**
At webapp.rbgrid.io, only to load the page. The app installed on the hub works on your home network alone. The hub uses internet only to set its clock; without internet, set the clock from your browser (**Use this device's time**). → [Time](time.md)

**Does my data go through RobotDyn's servers?**
No. In both ways the app talks to the hub directly on your network. At webapp.rbgrid.io only the page itself loads from our site.

**What is demo.rbgrid.io?**
A demo with sample sites, so you can look around without a hub. It can't connect to your hub.

## Setup

**Can I leave the wizard half-way?**
Yes. Nothing is blocked: skip optional steps and come back later from **System → Setup wizard**. **Done** lists what's left. → [Setup wizard](setup-wizard.md)

**The wizard says "Almost done". What's missing?**
The **Done** step lists it: a site name (not "Default"), a synchronized clock, your own tariff and at least one point. Each row has a **Fix** button.

**The time says "not synchronized".**
The hub sets its clock over the internet (NTP). Check that the hub is on your home Wi-Fi with internet and choose **Sync with NTP**. Without internet, choose **Use this device's time**. → [Time](time.md)

**Why do "today" and the tariff zones look shifted?**
The hub's time zone differs from where you are. Set it in **Settings → Time**. The Overview shows a note when the hub's zone and your browser's differ by 30 minutes or more.

**I changed a setting, but nothing happened.**
Settings has a bar at the top: *Unsaved changes: n*. Choose **Apply**. Some settings apply only after a restart: then the button reads **Save and restart**. A few panels (Security, Browser access, Hub access point, MQTT broker) save with their own button. → [Settings](settings.md)

**I changed my home Wi-Fi. How do I move the hub?**
In **Settings**, the **Connection** card: scan, choose the network, enter its password, **Save and reboot**. If the hub gets a new address, open the app at the new one.

## Tariff and money

**Why does money show "factory example"?**
No tariff has been set yet. Set yours in the wizard (new hub) or in **Tariffs**. → [Tariffs](tariffs.md)

**I used the wizard's tariff step and my taxes and zones are gone.**
The wizard's quick tariff always rebuilds the whole plan. When the hub has a fuller plan of your own, it asks **Replace the hub's tariff?** first. Use it on a new hub; to change an existing plan, use **Tariffs**.

**Are standing (monthly) charges counted?**
No. Costs and forecasts are energy-based: per-kWh prices, per-kWh taxes and fees, and VAT.

**What does "≈" mean next to an amount?**
It's the app's own estimate at your current rates, for example the cost per hour of the power right now, or a month forecast. → [The Overview](dashboard.md)

**If I change the tariff, do past costs change?**
No. The hub stores the cost of each period at the rates in force then. A new tariff applies from the moment you save it.

## Points and modules

**What's a "point"?**
One measured line with a name and a role: the grid input, a circuit, an appliance, solar. Points come from the current channels of your rbAmp modules. → [Points](points.md)

**How many modules and points can the hub take?**
Up to 8 rbAmp modules (hub firmware v1.1.2 and later). The hub keeps history for up to 12 points; more points show live values only.

**Can I rename a point?**
Yes, on hub firmware v1.1.4 and later. On v1.1.3 and earlier, on ESP32 chip revisions before v3, a renamed point could disappear after the next restart, together with its history: update the hub first. → [Points](points.md#point-settings)

**I added a point, but it has no history.**
Restart the hub: a newly bound point starts keeping history after a restart. If all 12 history slots are used, the point shows live values only.

**How do I add a second module?**
**Points → Add a module**, then follow the dialog. New modules all arrive at the same factory address, so the app first moves a module off it for you. Connect one new module at a time, with the hub off. → [Sensors and modules](sensors.md)

**What happens when I unbind or delete a point?**
Both do the same: the point stops being metered and leaves the overview, and **its stored history on the hub is erased for good**. Download it first in **History** (CSV) if you may need it. The sensor channel stays free and can be bound as a new point.

**The scan doesn't find my new module.**
- Connect new modules **one at a time**, with the hub switched off: every new module arrives at the same factory address, 0x50.
- If a module already sits at 0x50, the dialog first moves it to a free address. Do that before you connect the new one.
- *A bus line is held low* or *the module bus is stuck*: a module may be without power, set to another mode, or wired wrong. When you remove a module, unplug its whole cable, not only the power.
→ [Sensors and modules](sensors.md)

**What does "Unmetered" mean?**
The difference between what the grid input measures and the sum of your metered lines. It isn't one device: it's everything no point measures.

**The input shows export, but I have no solar.**
A current sensor on the input most likely faces the wrong way. Switch off the supply and turn the sensor round on the conductor (arrow towards the home).

**A point says "sensor reversed — not metered".**
A Basic module counts consumption in one direction only. Its sensor faces the wrong way, so the load isn't counted. Turn the sensor round (arrow towards the load).

**What do "clipping" and "sensor limit" mean?**
The current is at, or near, the top of the sensor's range. Above it, readings are cut off and the point counts less than it uses. Fit a sensor with a higher rating.

**Can I measure three phases?**
Yes, with an rbAmp STD-U3I3 on the input: set **Settings → Mains** to **Three phase**, then **Points → Use as the three-phase input**, and restart. → [Scenario: one main meter](scenario-main-meter.md)

## Home Assistant and MQTT

**How do I get the data into Home Assistant?**
Connect the hub to an MQTT broker in **Settings → Home Assistant**. Home Assistant finds the hub's sensors by itself (MQTT discovery). → [MQTT broker](mqtt.md)

**Which sensors go into the Energy dashboard?**
The points' energy sensors ("Energy In", "Energy Out"). **Settings → Home Assistant** shows a table of what to pick.

**I deleted a point, but Home Assistant still shows it.**
The hub removes a deleted point's entities from Home Assistant, as long as it's connected to the MQTT broker when you delete it. If it wasn't, delete the leftover device in Home Assistant (**Settings → Devices & services → MQTT**).

**Is the MQTT connection encrypted?**
Not yet: use a broker on your own network, with its own users and access rules. → [MQTT broker](mqtt.md#connect-the-hub-to-a-broker)

## Security

**Do I need an API token?**
It's optional, but recommended. Without it, anyone on your network who opens the app can change the hub's settings and tariff. → [Settings](settings.md)

**I set a token, and another browser can't save anything.**
The token is kept per browser. Enter it once in the other browser (**Settings → Security**).

**The app warns about the hub's access point or the API token.**
The hub's own Wi-Fi still uses the setup password, or no API token is set, or both. The warning's buttons lead to **Access-point password** and **API token**. It goes away only when both are done: without a token, the hub doesn't tell the app whether its password was changed.

**I forgot the token.**
There's no way to reset it from the app: every change, factory reset included, needs the token. The only way back is a clean flash over USB: erase the flash and write the four release files again (the "start clean" steps in the release notes). That erases Wi-Fi, settings, points, tariff and tokens; expect the history to be lost too. Keep your API token somewhere safe.

## Updates

**How do I update the app?**
At webapp.rbgrid.io it's always the latest; nothing to do. For the app installed on the hub, install the new `rbgrid-ui_<version>.zip` from the release the same way as the first time. Your settings and history are on the hub and stay. → [Opening the app](opening-the-app.md)

**How do I update the hub's firmware?**
In **Settings → Hub firmware**, upload `main_release.bin` from the new release (the other three files are for USB flashing only). Wi-Fi, settings, points, tariff, tokens and history are kept, and the modules keep counting during the update. If the new firmware doesn't start, the hub goes back to the previous one by itself. Then install the release's matching `rbgrid-ui_<version>.zip`. Don't restart the hub in the first minute after an update: the new firmware isn't confirmed yet, and a restart inside that minute rolls it back to the previous one. → [Settings](settings.md#hub-firmware)
