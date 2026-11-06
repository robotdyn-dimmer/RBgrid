# Changelog — RBGrid webapp

All notable changes to the RBGrid web interface. Newest first.

## 1.1.5 — 2026-11-06

For hub firmware 1.1.5. Requires hub API version 1. App changes since 1.1.4. With the input metered per phase, history and money per period show L1 only in 1.1.5; live values and the hub's system totals are the full sum.

### Added
- **Three-phase input: one point, or one point per phase** (hub ≥ 1.1.5, root u3i3-per-phase-points). On the STD-U3I3 card that carries the input (Points): «Input: one point for all three phases (phases netted)» / «Input: one point per phase», with the buttons «One point, phases netted» and «One point per phase», a confirm saying what each means for the bill, and the restart prompt. Per phase: the input keeps its history on L1, L2/L3 become input points of their own. Back to one point: those points are deleted (their own history erased, their totals move into the input). With the input split, the app reads «the input» as the sum of its phase points, each one a phase of it, as the hub sums them.

### Fixed
- **Tariffs, export-only tariffs (e.g. SEG):** every zone shows its **Sell price**, whatever the net-metering toggle; a new zone always gets an explicit sell price (0 when no other zone has one); the toggle reads «Net metering (monthly kWh credit)» and its help says to leave it off for a paid-per-kWh export tariff (content sellprice-gated-on-netmetering).
- **Hub's day on a clock-change day:** «today» windows, hour axes and day totals start at the hub's real midnight on the days summer time begins or ends — after the change they started an hour early (Berlin 29.03: 22:00Z instead of 23:00Z). Where midnight doesn't exist (Santiago's 24:00 change) the day starts at 01:00, like on the hub.
- **Time:** southern-hemisphere zones in the timezone list and detected from the browser — Sydney/Melbourne, Adelaide, Brisbane, Perth, Auckland, Santiago (with their summer time), São Paulo, Johannesburg; the raw config editor no longer shows «Daylight saving» (`enable_dst`): the hub takes summer time from the zone's rule alone, so the flag changed nothing (content dst-toggle-dead).
- **Tariffs, texts:** the priority hint says a Weekend zone ranks 3rd only on Saturday and Sunday (last on weekdays) and that a zone's days come from «Days»; «Remove zone» says where its hours go — to the default zone, or unpriced when there is none or the zone removed is the default.
- **Point page:** «readings not corrected» sends you to Point settings → Current sensor, where the model is changed (it pointed at the module card).
- **Car park:** a three-phase charger bound as one point per conductor is one station when its points share a group (2–3 station points, each on its own phase L1/L2/L3, same rated power) — one card, one load-balance segment and one «Sessions today» session, counted on the sum of its points (was ×3). The group's name from Settings → Consumer groups names the card.
- **Economics → Balance:** «Unmetered, % of input» shows «—» when the input imported under 10 Wh for the period (was −3 157 043 % on a near-idle input); Overview's unmetered share likewise hides below 50 W of input load (root econ-balance-percent).
- **Tariffs:** a zone's sell price stays visible and editable with net metering off (the hub values exports at it
  either way), with a note to set it to 0 without a feed-in contract; the net-metering text calls the ratio the
  «selling ratio», as the field does.

## 1.1.4 — 2026-11-04

For hub firmware 1.1.4. Requires hub API version 1. App changes since 1.1.3:

### Fixed
- **Unbind / Delete point:** both dialogs say the point's stored history is erased for good (it is — the hub frees
  the slot); the sensor channel stays for a new point.
- The quick tariff doesn't ask before replacing the hub's factory example plan, only a plan you set up.
- The input's role texts no longer send you to the setup wizard (the hub keeps it fixed).
- **Home Assistant:** with no broker saved the panel says MQTT is off (the hub has no default broker).
- **Add a module:** when the hub's module at 0x50 isn't answering, the dialog points to «Find one at another address».
- **Add a module (hub 1.1.4):** an empty placeholder module at 0x50 (`placeholder: true`) no longer blocks the dialog — a new module from the box connected there takes that slot.
- **History:** points past the first six are greyed out (there is no «Other» group).
- **Connect screen:** the address hint names the router's device list and `rbgrid-xxxx.local`.

## 1.1.3 — 2026-11-04

For hub firmware 1.1.3, which answers browsers only from allowed origins and always allows the hosted app
at `https://webapp.rbgrid.io`.

### Changed
- **Browser access (CORS):** the hub's built-in origins are shown as fixed rows («always allowed»); your own
  origins are the editable list (up to the hub's limit); «Allow any website (not recommended)» is the wildcard.
  Saving always sends the choice explicitly. A page served by the hub itself is never asked to add its origin.
- **Connect screen:** in the hub kit it offers only the hub's IP address (the broker way was never in the kit);
  on an https page it reminds about the browser's local-network permission when no hub answers.
- **Demo:** the demo-site picker sits under the logo.

### Fixed
- **Where the money goes:** when some part has no cost, the bar shows energy for all parts (it drew Wh as money).
- **Standby insight:** the share reads «% of the month's consumption» — it is computed from energy, not cost.
- **Surplus insight:** says the buy/sell price difference instead of «better than selling it».
- **Economics:** the net-metering credit block is hidden while the plan has net metering off; the progressive
  block shows only when the hub runs one.
- **Setup wizard:** the quick tariff asks before replacing a fuller plan (taxes and levies, net metering, fixed
  payments, more zones); «Open MQTT / Home Assistant settings» lands on the Home Assistant panel.
- Wording: two-pole phases on a three-phase site no longer say «240 V»; current-only modules don't mention load
  control; hints name the real panels (Settings → Connection / Security).

## 1.1.2 — 2026-11-03

The web UI for hub firmware 1.1.2: `rbgrid-ui_v1.1.2.zip` = `kit/` + `ui-manifest.json`, installed the same
way as 1.1.0. Requires hub API version 1. The new screens (add a module, the input on a U3I3, the hub's
access point, factory reset) show on hubs with firmware 1.1.2 or later; on older hubs the app works as 1.1.0.

### Changed (2026-11-03)
- **Release:** the v1.1.2 screens are on (bench gate 03.11, hub image 4456a483). Adding a module also finds one
  already set to another address (scan shows its model and firmware) — only a module from the box needs 0x50 freed.
- **Hub access warning** (Overview + Settings, hub 1.1.2+, rides with the access-point block): while the hub's
  own Wi-Fi keeps the setup password and no admin token is set, a warning says anyone nearby can join and
  change the hub, with links to both; a lighter note while only one is done. Hideable for the session.
- **Add a module** is on for hubs with firmware 1.1.2+ (bench run 03.11: scan 0.17 s, readdress ≈ 2 s, the
  module keeps its points). The «bus stuck» text names the real causes (a module without power, in another
  mode, or miswired) instead of pull-ups. Points warns when a bus line is held low (`bus_line_low_s`) — it
  clears by itself, no restart.
- **Live-only points** (hub 1.1.2 `history:false`): the point page and History say the point has no stored
  history and why (the hub keeps history for 12 points, or a restart starts it); the bind dialog warns when all
  12 history slots are taken. Older hubs don't send the flag — nothing shown.
- **Factory reset** (Administration): on hubs with firmware 1.1.2+ it is a real reset (`/api/system/factory-reset`):
  settings, points, tariff, Wi-Fi, tokens, the access-point password and — unless the box is cleared — the
  history; the hub comes back with its own access point, and the dialog says the app loses it. Older hubs
  get no reset button: their call only undid the last site-settings save. Hidden in release builds until the
  bench run (`RELEASE_HIDDEN.factoryReset`).
- **Points:** «The module bus is stuck — no readings for N s…» with Restart when the hub reports
  `bus_stalled_s` (v1.1.2). The add-module steps say to unplug a module's whole cable, not only its power.
- **Hub access point** (Settings): shown on hubs with firmware 1.1.2+ that list `/api/system/ap`, hidden
  in release builds until the bench run (`RELEASE_HIDDEN.apPassword`); when the hub answers `applied: false`
  the card says the password takes effect after a restart and offers «Restart now».
- **Time:** the status chip reads «not synchronized» for every hub state that isn't synced or syncing
  (UNINITIALIZED, TIMEOUT, ERROR, FALLBACK) — those showed no chip at all.
- **Points:** «Use as the three-phase input» on an STD-U3I3 card (hub firmware 1.1.2+): the hub regroups
  the module under the input's point after a restart, history kept (`POST /api/v1/units/<input>/rebind`).
  Older hubs get a caption to update. Hidden in release builds until the bench run (`RELEASE_HIDDEN.inputGroup`).
  The old «Bind as one 3-phase point» offer is gone — every hub version refused it.
- **Demo:** the demo hub reports no firmware version (demo.rbgrid.io names only a published firmware,
  site team 03.11); its HA note drops the sentence about firmware 1.1.2.
- **Russian:** the month forecast reads «Прогноз стоимости за месяц», not «Прогноз счёта» (a bill isn't promised).
- **MQTT broker:** the «Secure connection (TLS, mqtts)» switch is hidden in release builds — the hub
  can't verify a broker certificate yet. A hub that already has an `mqtts://` address keeps it; the
  switch shows it read-only.
- **Home Assistant:** a note under the Energy-dashboard map for hubs before firmware 1.1.2 — HA may not
  offer «Cost (Total)» / «Revenue (Total)»; 1.1.2 fixes the type and names the second Export value.
  On a 1.1.2+ hub the map names «Export value (Total)» and the note goes (version from `/api/system/version`).

### Changed (2026-11-02)
- Binding a point no longer asks for **History storage**: the hub keeps hourly records and rolls
  days, weeks, months and years up from them for every point whatever was picked there.
- «Savings» is now **Insights**; money for exported energy reads **Export value** (was
  «Revenue»), as on rbgrid.io.
- «Monthly bill forecast» reads **Month cost forecast** (the hub's estimate at the plan's rates, not the
  supplier's bill).
- New insight: **EV charging yesterday in a dearer zone** (titled by the user's own group / point name) — what the same energy would have cost
  in the cheapest zone, from the plan's rates (a past fact, not a forecast).
- «Where the money goes» splits the remainder honestly: measured points past the top are «Other points»,
  what no point measures is «Unmetered» (one «Other» read as full coverage).
- History by days (7 days, this month) draws today too — a partial bar from the live counters (the
  period total already counted it).
- «Where the money goes» counts an appliance on several phases (a group whose points share one
  role — a three-phase charger, a heat pump) as one share.
- The standby estimate leaves out heat pumps and chargers (they run on their own schedule).
- The tariff editor no longer shows **fixed payments** or the **per-zone credit ratio**: a v1.1.x hub
  counts neither in any cost (fixed charges) or settlement (the plan-wide ratio is used); the plan
  keeps them and a save sends them back unchanged.
- The tariff editor no longer offers the **Holiday** zone type: v1.1.x hubs have no holiday dates
  to fill, so the zone never activates (a plan that already has one still shows it).


## 1.1.0 — 2026-10-20

First release of the web UI as a hub-installable kit: `rbgrid-ui_v1.1.0.zip` = `kit/` +
`ui-manifest.json` (`npm run build:embedded`; installed with `ui-upload` / `ui-commit`, see the
hub documentation *Installing the web UI*). Requires hub API version 1. Interface language:
English by default, Russian included.

### Release scope
- Some screens in the code are not part of this release and are hidden in a release build
  (`src/constants/release.ts`).
- Telemetry frame v2 is the hub's default; the UI reads v2 and older v1 hubs alike.
- Split-phase mains is deferred: not offered in a release build (still shown on a hub already set
  to it). The input is channel I0 of the first module; choosing it comes later.

### Public demo
- `npm run build:demo` → `rbgrid-demo_v1.1.0.zip` for demo.rbgrid.io: built-in demo sites, each
  with its own hardware and situation. The default is a three-phase house with a heat pump and an
  EV charger on a two-zone DE tariff (8 points — the v1.1.x history limit); the others are a home with solar, a flat on one 7-channel
  module, a three-phase workshop and a car park with chargers. Links: `?scenario=heatpump-ev|solar|flat|workshop|carpark`
  (or `?demo=<id>`), optionally `&screen=overview|history|economics|events|tariffs|points|settings|onboarding` and
  `&range=today|yesterday|7d|month|year` for History.

### Added
- **Overview by site scenario** (home / business / charging stations): live power and money,
  energy-flow schemas (home, distribution, balance, single-line), «Needs attention», savings
  advice, month forecast, standby («vampire») estimate and a guided hunt.
- **Charts** (one module for all screens): today by hour with a live running hour — with solar
  on site the energy view shows the house's hour in two sections, from the grid and from its own
  solar, with export below the line, point trends,
  History with time windows, previous-period ghost, hover card, «as a table» view, CSV export
  and a per-point summary CSV. Charts refresh by themselves (hours every minute).
- **Points**: bind a free sensor channel (point ID, reuse of a removed point's ID), rename, role,
  phase, rating, groups; module cards with model, channels in use («3 of 7 in use»), the CT of
  each channel and its accurate range; module firmware update for STD modules.
- **BASIC modules** (UI1/UI3/UI7, I1–I3): consumption-only metering said up front; CT model per
  channel (with a warning that the factory calibration is replaced); a backwards CT is reported,
  not shown as export; CT clipping / near-limit («clipping» / «sensor limit») on the tile, in «Needs
  attention» and the tables; SCT correction status per channel (uncorrected readings are marked).
- **Multi-phase**: mains system (1P / split / 3P), per-phase cards, voltage band and unbalance.
- **Hub log** (Events → Log), hub firmware update from Settings, settings with the hub's own
  ranges and on-restart fields, interface language and number/date formats.
- **Hub access point** (Settings → Network): the hub's own Wi-Fi `rbgrid-<mac4>`; the user sets
  its password, with a reminder while the first-login password is in use.
- **Setup wizard** for a fresh hub: Wi-Fi, time zone, tariff as printed on the bill, points,
  admin token.

### Changed
- A point bound to a channel the fitted module doesn't have (a 3-channel module in place of a
  7-channel one) reads «no such channel on the module», not «offline», and is left out of alarms
  and the flow; such channels are never offered for binding.
- A free channel is «free», not an error. A CT model change restarts the module; its points keep
  their last reading for a few seconds instead of flashing a fault.
- Unbinding a point warns that the hub deletes its history (download CSV first).
- The UI re-reads the hub's hardware list as soon as the frame's `hw_rev` changes.

### Fixed
- Language packs are served from `static/lang/` (the only kit folder a hub serves).
- Erased-flash history records (year 2106) are dropped; day totals are checked against the
  hub's hour records.
- Energy-flow cards keep their text inside the card in every language.

### Security / build
- Flow schemas XML-escape every data-derived name before the SVG is injected.
- Clean-clone build: the design system builds itself on install; `package-lock.json` tracked.
