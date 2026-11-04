# Settings

**Settings** holds the hub's own configuration: the site, the interface and time, metering, the network and Home Assistant, access, and maintenance. Tariffs and points have their own pages; Settings links to them.

Settings are kept in the hub's own memory and survive a restart. Once an API token is set (see [Security](#security)), every change needs it; without it the app shows "Admin token required."

## The panels

The panels come in six groups, one after another, separated by a line. The groups have no headings of their own.

| Group | Panels |
|---|---|
| 1. The site | Site scenario · Site · Mains · Consumer groups · Charging stations |
| 2. Language and time | Interface · Time |
| 3. Metering | Tariff · Points · Telemetry |
| 4. Connectivity | Status and Connection · Hub access point · Home Assistant |
| 5. Access | Security · Browser access (CORS) |
| 6. Maintenance | Restore point · Hub firmware · Administration |

Some panels appear only when your hub supports the feature; each section below says when.

![Settings, the whole page (no group headings; panels in order)](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-groups.webp)

## Saving changes

Most panels have their own **Save** button. Three panels share a saving bar at the top of the page instead: **Site**, the nominal values in **Mains**, and **Charging stations**. The bar shows **No unsaved changes** or **Unsaved changes: n**, with:

- **Apply**: saves the changes that take effect at once;
- **Reset**: discards your unsaved changes;
- **Save and restart**: shown only when you've changed a field that applies after a restart. It saves the changes, then restarts the hub (the app asks first).

The bar stays at the top while you scroll. If a saved change needs a restart, the bar's message offers **Restart now**. The **Time**, **Hub access point** and **Home Assistant** panels have their own restart button.

### What needs a restart

Most settings apply at once. These need a hub restart:

| Change | How it applies |
|---|---|
| Mains nominal voltage and frequency | **Save and restart** |
| NTP sync, the time source and the NTP server | after a restart; the Time panel says so |
| The MQTT broker | after a restart (**Restart now**) |
| The Wi-Fi network (Connection) | **Save and reboot**: the hub restarts at once |
| Restoring a restore point | the hub restarts at once |
| A firmware update | the hub restarts by itself |

The access-point password normally applies at once; if the hub needs a restart for it, the panel says so.

![The bar at the top of Settings with one unsaved change: Reset · Apply](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-savebar.webp)

## Site scenario

![Settings: the Site scenario panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-site-scenario.webp)

Always shown. Three cards: **Home**, **Business** and **Charging stations**. The current one is marked "· current".

The scenario sets what the **Overview** shows. When you pick another one, the app asks "Change scenario?":

- **Change** saves it on the hub. The Overview changes for everyone who opens this hub. It applies at once, no restart.
- **Just preview** shows the scenario in this browser only. Nothing is saved on the hub.
- **Cancel** keeps the current scenario.

## Site

![Settings: the Site panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-site.webp)

Always shown.

| Field | What it does |
|---|---|
| **Site name** | The name of your home or building, for your reference. |
| **Location** | Where it is, for your reference. |

Save with **Apply** in the saving bar; both apply at once. They're the same fields as **Name** and **Where it is** in the [setup wizard](setup-wizard.md).

Fields the hub reads only when it starts are listed under **Applies after a hub restart** and saved with **Save and restart**.

## Mains

![Settings: the Mains panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-mains.webp)

Shown when your hub supports a mains system setting.

| Field | What it does | When it applies |
|---|---|---|
| **Mains system** | **Single phase** or **Three phase**. "Not set — counted as single-phase" until you choose. An option the hub hasn't validated on hardware is marked "— not validated on hardware". | Saved as soon as you choose ("Mains system saved on the device."); applies at once. |
| **Nominal voltage, V** | Your mains nominal voltage, 90–480 V. 0 means not set: the hub uses its default. | **Save and restart** |
| **Nominal frequency, Hz** | Your mains nominal frequency, 45–65 Hz. 0 means not set. | **Save and restart** |

The line "Nominal: … V · … Hz" shows the values in use.

The panel also helps keep the points consistent:

- If no system is set but your points' phases suggest one, it says "The points' phases suggest: … Choose the system if that is right."
- If a point has a phase the chosen system doesn't allow, it lists those points. Change the phase on the point's page.

## Consumer groups

Shown when your hub supports consumer groups.

Groups gather points, for example "Ground floor" or "Workshop".

| Field | What it does |
|---|---|
| **Group n name** | The group's name. Required, up to 31 bytes (fewer characters for non-Latin letters). The helper shows how many points are in the group. |
| Delete icon | Removes the group. The app warns if the group still has points. |
| **Add group** | Adds a group, up to 16. |

Press **Save** (or **Cancel**). Changes apply at once. You add a point to a group on that point's page, in its **Group** field.

## Charging stations

![Settings: the Charging stations panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-charging.webp)

Shown when your hub supports charging-station metering and either the site scenario is **Charging stations** or a point has the **Charging station** role.

| Field | What it does | Default |
|---|---|---|
| **"Charging" threshold, W** | Above this power a station counts as charging. 0–100000 W. | 0 = 500 W |
| **Idle wait after session, min** | How long a station must stay below the threshold before the session counts as finished. 0–1440 min. | 0 = 30 min |
| **Charging price for the customer, per kWh** | The price per kWh used to count charging fees. 0–1000. | 0 = charging fees aren't counted |

Save with **Apply** in the saving bar; changes apply at once.

## Interface

![Settings: the Interface panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-interface.webp)

Always shown.

| Field | What it does | When it applies |
|---|---|---|
| **Interface language** | **English** or **Русский**. "Not set — browser language" follows each browser's language. The language is stored in the hub: it's the same for everyone who opens it. If the hub doesn't store a language, the field is off and the browser's language is used. | At once, no restart. |
| **Number and date format** | How numbers and dates look: as the language, or a fixed format (ru-RU, en-GB, en-US, de-DE). Each option shows a sample. | At once, **this browser only**. |

The light/dark theme isn't in Settings: its switch is at the bottom of the left panel (on a phone, under **More**).

## Time

![Settings: the Time panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-time.webp)

Always shown. The time zone, the time servers (SNTP) and setting the hub's clock from your browser. The panel says: "NTP sync, the time source and the NTP server apply after a hub restart; the time zone applies at once." See [Time](time.md).

## Tariff

![Settings: the Tariff panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-tariff.webp)

Always shown. A link card: the tariff plan, its zones and prices are set on the **Tariffs** page. **Open "Tariffs"** takes you there. See [Tariffs](tariffs.md).

## Points

![Settings: the Points panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-points.webp)

Always shown. A link card: each point's name, role, phase, nominals and the input limit are set on that point's page under **Points**. **Open "Points"** takes you there. See [Points](points.md).

## Telemetry

![Settings: the Telemetry panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-telemetry.webp)

Always shown. The live data stream the hub sends to the app.

| Field | What it does |
|---|---|
| **Stream profile** | **Lean**: realtime values only; energy and money are read on demand — less load on the hub. **Full**: every frame carries realtime values plus energy and money. Switches at once, no restart. The buttons are off if your hub doesn't support changing the profile. |
| **Apps connected now** | How many apps are receiving the live stream right now. Read-only. |
| **Technical details** → **Frame format** | The version of the live data frame. Read-only. |

## Hub-access warning

On a hub with firmware v1.1.2 or later, a warning appears at the top of the connectivity panels (and on the Overview) while the hub is open to anyone nearby:

- the hub's own Wi-Fi still uses its setup password **and** no API token is set: anyone nearby can join it and change the hub;
- no API token is set: anyone who reaches the hub can change it;
- the hub's own Wi-Fi still uses its setup password: anyone nearby can join it.

The buttons **Access-point password** and **API token** take you to the panel that fixes it. **Hide** hides the warning for this browser session.

Without an API token, the hub doesn't tell the app whether its access point still has the setup password. So the warning stays until **both** are set: an API token and your own access-point password.

## Status and Connection

![Network: Status + Connection](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-network.webp)

Always shown, as two cards.

**Status** (read-only):

| Field | What it shows |
|---|---|
| **Mode** | **connected to Wi-Fi**, **own access point**, or **Wi-Fi + own access point**. |
| **Network** | The Wi-Fi network the hub is on. |
| **Address** | The hub's IP address. |
| **Signal** | Signal strength in dBm, with **good** (−60 dBm or better), **fair** (down to −72 dBm) or **weak**. |

**Connection** sets the Wi-Fi network the hub joins:

| Field | What it does |
|---|---|
| **Scan networks** | Lists the networks the hub can see, with their security, channel and signal. Click one to fill in its name. |
| **SSID** | The network's name. Required. |
| **Password** | The network's password. |

Press **Save and reboot**. The app asks you to confirm, then the hub restarts at once to join the new network. If it gets a new IP address there, connect to it again from the app's start screen.

The hub's own access point stays on after it joins your network, so you can always reach the hub at `192.168.4.1` through it.

## Hub access point

![Hub access point](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-hub-ap.webp)

Shown on hubs with firmware v1.1.2 or later. The hub runs its own Wi-Fi access point, named `rbgrid-<id>`. This panel sets its password.

| Field | What it does |
|---|---|
| **Network** … | The access point's name, with **your password** or **first-login password** (the setup password is still in use). The hub shows which only with the API token. |
| **New access-point password** | Your own password: 8–63 characters, Latin letters, digits and symbols only. |

- **Change password** sets it, after you confirm. It applies at once; devices on the access point have to reconnect with the new password. If the hub needs a restart for it, the panel says "It takes effect after a restart." and shows a restart button.
- **Back to the first-login password** returns to the setup password. Shown only once you've set your own.

The app never shows or makes up a password. Keep yours somewhere safe.

## Home Assistant

![Home Assistant + MQTT broker + what to pick in HA's Energy dashboard](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-home-assistant.webp)

Shown when your hub supports MQTT. The hub sends its measurements to an MQTT broker; Home Assistant finds the hub's sensors there by itself.

At the top, the panel shows whether the hub is connected to a broker ("The hub is connected to broker …"), or that it isn't ("The device isn't connected to an MQTT broker — HA won't see it.").

**MQTT broker**:

| Field | What it does |
|---|---|
| **Broker address** | The broker's IP address or host name, for example 192.168.1.100. No spaces, `/`, `@` or `:`. |
| **Port** | 1–65535. Default 1883. |
| **Login** | The broker user for the hub. |
| **Password** | That user's password. A password without a login isn't accepted. |

If the hub already has a login and password stored, it doesn't show them: type them again when you save, or the broker is saved without them.

Press **Save**. The hub connects to the new broker after a restart: press **Restart now**. With no broker saved, MQTT is off. There is no separate switch to turn MQTT off.

The panel also lists what to pick in Home Assistant's **Energy** dashboard:

| Energy dashboard entry | Sensor |
|---|---|
| Grid consumption | RBGrid Main · Energy In |
| Return to grid | RBGrid Main · Energy Out |
| Solar panels | RBGrid *your generation point* · Energy Out |
| Individual devices | RBGrid *point* · Energy In, for each point |

Topics, discovery and the command channel: see [MQTT broker](mqtt.md).

## Security

![Settings: the Security panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-security.webp)

Shown when your hub supports an API token. (The whole Access group, this panel and **Browser access**, is shown then.)

The **API token** is a password for changing the hub. The chip shows **set** or **not set**.

| Action | What it does |
|---|---|
| **Set token** + **Set** | Sets a token: any text you choose. From then on every change to the hub needs it ("Token set — writes now require it."). |
| **New token (change)** + **Change** | Replaces the token. The app asks first: "Change the API token? Current sessions will need to use the new token." |
| **Delete** | Removes the token, after you confirm. Changes become open to anyone who reaches the hub. |

The app keeps the token in this browser. In another browser or device, you'll be asked for it. Keep it like a password.

## Browser access (CORS)

![Settings: Browser access (CORS), with the built-in origin row](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-cors.webp)

Shown with **Security**. Sets which other web pages may call the hub's API from a browser, for example your own dashboard on another address. If the hub doesn't let you read the policy, the panel says "Policy unavailable — an admin token is required or the device does not support this setting."

| Field | What it does |
|---|---|
| Status chip | **open to any site** or **allowed origins only**. |
| Built-in rows | On a v1.1.3 hub, the hub's own allowed sites are shown as fixed rows marked "always allowed — the RBgrid app at this address". You can't remove them, and they don't count toward your list. |
| **Allowed origins (n/max)** | Your own list of allowed addresses ("No origins of your own yet." when empty). The maximum comes from the hub. Remove one with its cross. |
| **App origin (https://…)** + **Add** | Adds an address: `http://` or `https://`, a host, and an optional port, for example `https://host:8443`. |
| **Add this origin** | Adds the address of the page you're using now. Shown when it isn't on the list, and not on the app served by the hub itself. |
| **Close access (list only)** / **Allow any website (not recommended)** | Allows only the built-in and listed addresses, or any site. For any site, the app asks "Allow any website?" → **Allow all**. |

Press **Save**. It applies at once, no restart ("Access policy saved (no restart needed)."). The app won't save a list that would lock out the page you're using.

From v1.1.3, a hub allows **only its own pages and the listed addresses** by default. The RBgrid app at `https://webapp.rbgrid.io` and `https://demo.rbgrid.io` is always allowed: these are the built-in rows. A hub that was set to allow any site before the update keeps that setting.

## Restore point

![Settings: the Restore point panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-restore-point.webp)

Always shown. A copy of the hub's settings, kept on the hub: settings, points, tariffs, time and modules. Tokens aren't included. There's one restore point; a new one replaces the previous.

| Item | What it does |
|---|---|
| Status | "No restore point." or "Saved … · files: n". |
| **Save restore point** | Saves a copy now. If one exists, the app asks before replacing it. |
| **Restore and restart** | Brings the saved copy back and restarts the hub, after you confirm. The hub is back in 20–30 seconds. |

Nothing is saved automatically, so save one before a big change. The restore point stays on the hub; it isn't a file you download.

## Hub firmware

![Settings: the Hub firmware panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-firmware.webp)

Shown when your hub supports firmware updates over the network.

The panel shows the hub's current version and build. To update:

1. **Choose a .bin file**: `main_release.bin` from the release (the other three release files are for USB flashing only). The app checks the file first. It refuses a file that isn't a firmware image, one without a description, and firmware for another device or another chip ("This is another device's firmware, not the hub's"). If you pick the build that is already installed, it says "This build is already installed. No need to update."; **Install** is still offered.
2. **Install**, after you confirm. A progress bar shows the upload.
3. The hub restarts by itself. The app waits for it (up to about three minutes) and then confirms the new firmware started.

If the new firmware doesn't start, the hub goes back to the previous one, and the app tells you so.

Wi-Fi, settings, points, tariff, tokens, history and the installed web app are kept. The modules keep counting during the update. Afterwards, install the release's matching `rbgrid-ui_<version>.zip`.

## Administration

![Settings: the Administration panel](https://www.rbgrid.io/assets/img/webapp-docs/wa-settings-admin.webp)

Always shown.

**Reboot device** restarts the hub, after you confirm.

**Factory reset** is shown on hubs with firmware v1.1.2 or later. It returns the hub to its first start: the settings, points, tariff, Wi-Fi, tokens and the access-point password are erased. **Also erase the measurement history** is ticked by default; untick it to keep the history. The hub then restarts with its own access point, as on first boot, and the app loses its connection to it.

## Troubleshooting

**A change didn't take effect**
- Check whether it needs a restart (see *What needs a restart*), and use **Save and restart** or **Restart now**.

**"Admin token required."**
- An API token is set on the hub. Enter it in **Security**.

**The app can't reach the hub after a change**
- If you changed the Wi-Fi network, the hub may now have another address. Join its own access point, `rbgrid-<id>`, and check **Status**.

**Settings went wrong after an experiment**
- If you saved a restore point earlier, use **Restore and restart**. Save one before the next experiment.
- To start from scratch, use **Administration → Factory reset**.
