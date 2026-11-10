# Sensors and modules

## Overview

All measurement in RBgrid is done by **rbAmp modules** on the hub's I2C bus. A hub with firmware v1.1.2 or later reads up to **8** modules. You see them, and add new ones, on the **Points** page under **rbAmp modules**. What you measure with each channel is set up as a point (see [Points](web-points.md)).

Modules can also sit on a **remote node** that reports to the hub by radio; the node's channels appear as points, not as module cards here (see [Remote nodes](web-nodes.md)).

The app saves module and channel settings directly to the hub. They are kept in the hub's internal flash and survive restarts; no external memory module is needed.

## Measurement model

All voltage and current measurement is performed by the **rbAmp modules on the I2C bus**; each module handles synchronisation and digitisation itself. Channels are configured by their CT model (below).

Each module exposes a voltage input (`U`) and current channels (`I0`, `I1`, …); a module card summarises this as `1U + 7I`, `3U + 3I`, and so on (see [Module cards](#module-cards)).

- **Voltage** comes from the module's `U` input. There is no separate, user-selectable voltage sensor. In the points list the voltage input appears as a **Voltage bus** point (channel `V`). Like the main input, it cannot be removed: *"The main input and the voltage bus cannot be deleted."*
- **Current** comes from a current transformer on a channel. The point shows the channel's CT model and its range.
- **Three-phase module (STD-U3I3)** exposes three voltages and three currents. It can measure your input on all three phases (see [Use as the three-phase input](#use-as-the-three-phase-input)).
- **Frequency** is reported by the hub (the overview card's `Frequency`) and is not configurable. On hub firmware v1.1.3 and earlier, a module with unwired voltage inputs (for example an STD-U3I3 used for current only) could set a wrong frequency and voltage, around 45 Hz. Since v1.1.4 they come only from a module that sees mains.

## Module cards

Each connected rbAmp module appears as a **module card** under **Points → rbAmp modules**. Under the module's **model** (or "rbAmp module") and a status chip, **online** or **not found**, is a line like this:

```
module 0 · 0x50 · 1U + 7I · 6 of 7 in use
```

It gives the module index, its I2C address, its channel make-up (`1U` voltage + `7I` current) and, on modules with two or more current channels, how many are bound. The card also shows:

- the **calibration** status: "calibration confirmed", "calibration status not reported by the module" or "calibration: no data" (not shown for BASIC modules);
- "firmware *version*";
- notes where they apply:
  - BASIC module: it counts consumption only (it doesn't see export), and its firmware isn't updated through the hub;
  - current-only module: "Current-only module: for current monitoring, not energy metering — no kWh or cost shown for it."
  - uncalibrated module: the sensor model can't be chosen;
- one line per current channel (below);
- buttons, where they apply: **Update firmware** (see [Module firmware update](#module-firmware-update)) and **Use as the three-phase input** (see [below](#use-as-the-three-phase-input)).

### Per-channel lines

Each current channel shows its sensor model, its accurate range and what it is bound to:

```
I2: SCT-013-010 · accurate lo–hi A → Water heater
```

An unbound channel ends in `free` instead of a point name:

```
I2: SCT-013-010 · accurate lo–hi A · free
```

The app works out the accurate range from the sensor's type and rating. Where the hub reports the channel's full scale, that sets the upper end of the range. For the sensor's own specification, see the module's page on rbamp.com.

### SCT correction status

A current channel shows whether the module applied its SCT correction:

- **Corrected**: the channel shows `correction ×k` (the factor it applied).
- **Not corrected**: `⚠ readings not corrected:` followed by the reason:
  - `the channel is not calibrated`
  - `the channel gain does not match the CT model`
  - `the module does not know this CT model`

> **STD-U3I3:** the module is factory-calibrated together with its sensors; its readings come from that calibration. The app doesn't show the near-limit warnings (`clipping`, `sensor limit`) for an STD-U3I3.

### Channel that is not on the module

If a point is bound to a channel the module does not have, the point shows:

```
no such channel on the module
```

The point and its history are kept. Don't unbind it while you wait (see [Points](web-points.md#unbinding-a-point)).

### CT at its limit

On the tile and in **Needs attention**, a channel near its limit is flagged:

- `clipping`: the current has reached the sensor's clip point (its full-scale current, above its nominal rating); the reading is **under-counted**.
- `sensor limit`: the current is near the sensor's clip current: the hub flags it at about 90 % of that current. For SCT-013 clamps the clip current is about 116 % of the rating (58 A on an SCT-013-050), so the warning comes at about 104 % of the rating (52 A). Up to the clip current, readings are correct. STD modules' own sensors carry no such flags; they read correctly to about 130 % of their rating.

## Channel CT model

For **SCT-series** channels the CT model is chosen in the point's settings (**Current sensor**, see [Point settings](web-points.md#point-settings)): the SCT-013 code for the clamp actually fitted. The model is written to the module together with its type calibration. It applies at once and the module restarts; the hub doesn't.

Changing it **replaces the channel's factory calibration with the calibration for that model**. That factory calibration can't be restored, so set the model only to the clamp in use.

- **CT-series (factory-paired) modules**: the sensor is paired and calibrated at the factory, so the model is fixed and only shown.
- **Uncalibrated modules**: the model can't be chosen.

Supported SCT-013 models (voltage output, 1 V at rated current):

- `SCT-013-005` · `SCT-013-010` · `SCT-013-020` · `SCT-013-030` · `SCT-013-050` · `SCT-013-060` · `SCT-013-100`

The current-output `SCT-013-000` (50 mA) is not supported.

## Adding modules

A hub with firmware v1.1.2 or later reads up to **8** rbAmp modules on its I2C bus. Add each one from the app with **Points → Add a module**. The button appears on hubs with firmware 1.1.2 or later.

Every new rbAmp module arrives at address **0x50**, and two modules can't share an address. Connect new modules **one at a time, with the hub switched off**.

### The first module on a new hub (hub v1.1.4)

On a new hub, module 0 at 0x50 is an **empty slot** until a module is connected there: nothing is measured on it yet. The dialog says so: "Module 0 at 0x50 is an empty slot: nothing is measured on it. A new module from the box answers at 0x50 and takes this slot as module 0 — no address change, no scan."

1. Switch the hub off. To remove a module, unplug its whole cable, not only the power.
2. Connect ONE new module (SDA, SCL, GND, VCC) and switch the hub on. The hub's default bus pins depend on the board: **SDA 25 / SCL 26** on an ESP32-WROVER, and **SDA 8 / SCL 9** on an ESP32-S3, which the hub supports from firmware **v1.1.8** (the S3 has no GPIO25, and its GPIO26 belongs to the flash/PSRAM bus). Both are defaults you can change in the hub's settings.
3. Give it 10–20 seconds, open **Add a module** again and press **Check again**. A new module connected at 0x50 is picked up in this slot: it shows as present, and its channels appear under **Free sensors** on the Points page.

![Add a module on a new hub: module 0 at 0x50 is an empty slot](https://www.rbgrid.io/assets/img/webapp-docs/wa-sensors-add-module.webp)

If your only module sits at another address, module 0 isn't an empty slot (the grid input depends on it), and the dialog says it isn't answering. Move the module to 0x50, or use **Find one at another address**.

### More modules

When a configured module already sits at 0x50, it moves to a free address first, and the new one is then connected at 0x50. On hub v1.1.2 and v1.1.3 this is also how the first added module works.

The dialog works out where you are from the hub's state, so you can close it to switch the hub off and open it again later.

#### Step by step

1. **Free address 0x50 (if needed).** If one of your modules already sits at 0x50, the dialog offers to move it first: **Move it to 0x5…**. The module stores its new address itself; its points keep their names, history and settings. When it's done: "Module *n* now answers at 0x5…. Address 0x50 is free for the new module."
   - If that module isn't answering, check its connection and press **Check again**; its address can only change while it answers.
   - **Find one at another address** scans for a new module that is already at another address.
2. **Switch the hub off.** To remove a module, unplug its whole cable, not only the power: a module left on the bus without power stops the readings of every module.
3. **Connect ONE new module** to the hub's I2C connector next to the others (SDA, SCL, GND, VCC).
4. **Switch the hub on**, open **Add a module** again and press **Scan**.
5. The scan shows "New module *model* at 0x50 · firmware *version*". Press **Add**.
6. "Module *n* added at 0x50." Its channels now appear under **Free sensors** on the Points page: bind them as points (see [Binding a point](web-points.md#binding-a-point)). To add another, press **Add another** and repeat from step 1.

### Cables and pull-ups

- Keep each module's cable up to 15 cm.
- With up to three modules, leave the modules' pull-ups as they are: no cutting, no extra resistors.
- With four or more modules, cut the pull-ups on all but one module (or fit one external pair).

See the rbAmp [wiring guide](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) for longer cables.

### If something goes wrong

| Message | What to do |
|---|---|
| No new module answered. | Check that the module is connected and powered, and that it is the only new one. Scan again. |
| A bus line is held low — a module may be without power, set to another mode or wired wrong. | Check the module connections, then scan again. |
| Also on the bus, not rbAmp modules: … | Other I2C devices answered; they aren't added. |
| No new module answers at another address. | A new module from the box is at 0x50: move the existing 0x50 module first (step 1). |
| No free address is left in 0x51–0x5F. | All addresses are taken. |
| The hub already has *n* modules — its maximum. | The hub can't take more modules. |
| This hub's firmware can't add modules from the app. | Update the hub firmware (see [Settings](web-settings.md#hub-firmware)). |
| Couldn't read the hub's module list / The request didn't go through / The hub refused | Check the connection to the hub and try again; the message gives the reason. |

If the **Points** page itself shows **The module bus is stuck**, check the connections (a module wired without power blocks the bus), then restart the hub (see [Bus warnings](web-points.md#bus-warnings)).

## Use as the three-phase input

An **STD-U3I3** module measures three voltages and three currents. Its module card offers **Use as the three-phase input** when the module is present, you have an input, and the input isn't already three-phase. On hubs older than 1.1.2 the card says "Three-phase input from one *model*: update the hub to 1.1.2." instead.

![An STD-U3I3 card already used as the three-phase input: all three channels in use, so the button is no longer offered](https://www.rbgrid.io/assets/img/webapp-docs/wa-sensors-three-phase-input.webp)

1. Press **Use as the three-phase input**.
2. Confirm **Use as the input**. The input keeps its history.
3. The app says the hub will measure the input on all three phases after a restart. Press **Restart now**, or restart later.

After the restart the input is one point, **L1–L3**, with per-phase readings (see the **Per phase (U3I3)** table on the [point's page](web-points.md#live-values)). The Overview's **Phases** card then shows a phase-rotation check, for example "Phase rotation forward (L1 → L2 → L3)."

If the hub refuses, the app shows the reason.

### One point, or one point per phase

From app and hub **v1.1.5** the same module card also decides *how* the three phases are counted. The mode is in bold under the firmware line, with a one-line hint, and the button switches to the other mode. A hub on firmware older than v1.1.5 shows no mode line at all: its card offers only **Use as the three-phase input**, as above.

| The card says | Its hint | The button offers |
|---|---|---|
| "Input: one point for all three phases (phases netted)" | "Like a meter that nets the phases: export on one phase offsets import on another." | **One point per phase** |
| "Input: one point per phase" | "Like a meter that counts each phase: every phase has its own import and export, nothing is offset." | **One point, phases netted** |
| "Input: measured on L1 only" | "Only L1 counts toward the input; L2 and L3 of this module aren't in it." | **One point, phases netted**, and **One point per phase** while L2 and L3 are free |

Which mode is right for you is your meter's behaviour, not a preference: a meter that nets the phases and a meter that counts each phase bill the same house differently.

**To one point per phase.** The confirm asks "Meter the input per phase?": "Each phase becomes its own input point with its own import and export, as on a meter that counts per phase. Export on one phase is no longer offset by import on another. The input point keeps its history and takes L1; L2 and L3 start from zero. Applies after a restart." If a phase doesn't get its point, the card says which and why; pressing **One point per phase** again retries.

**Back to one point.** The confirm asks "Meter the input as one point?". When there are phase points to remove, the button is red and the text names them: "The points *…* are deleted and their own history is erased; their totals move into the input, so the site's totals stay." Their totals survive; their own records don't — so download anything you need from [History](web-history.md#downloads) **before** you regroup. With no phase points to remove, the input simply keeps its history.

**Either way the change needs a restart.** The card says "Saved. The hub meters the input this way after a restart." and offers **Restart now**, which asks "Reboot the device now?" ("The hub goes offline for about half a minute; metering resumes after start-up."). Until you restart, the card says "The change applies after a restart — until then the card shows the old layout." and hides the mode buttons. On app **v1.1.5** that message sat above the module list instead, where it could be off the top of the screen: if you pressed a mode button there and nothing seemed to happen, scroll up — the restart is still needed.

**What the app shows per period while the input is split.** From firmware **v1.1.6**, with the matching app package, those figures cover the **whole input**: the hub serves the three phase points as one summed series. On **v1.1.5** the per-period figures — the Economics balance, the Overview month and "vs yesterday", the energy chart — showed the **L1 point only**. Live values and the hub's system totals are the full sum on either version, each phase point keeps its own history on the hub, and the History page always shows the phases separately.

A split input is **three** measurement points, not one, so it takes three of the twelve the hub keeps history for (see [Points](web-points.md#messages)).

> ⚠️ **Before installing firmware older than v1.1.5 on a hub whose input is metered per phase, switch it back to one point.** Older firmware handles one supply point only.

## Module firmware update

**STD modules** can have their firmware updated through the hub, with an image file supplied by **RobotDyn**. The module card shows **Update firmware** when the module supports it and is present. BASIC modules aren't updated through the hub.

![Module firmware update (STD only): choose the .ota file supplied by RobotDyn](https://www.rbgrid.io/assets/img/webapp-docs/wa-sensors-module-firmware.webp)

The dialog shows the module's current version. The hub writes the new image to the module's spare memory half and switches the module to it; this takes about 2 minutes over Wi-Fi, and **metering keeps going**.

### Steps

1. Press **Update firmware** on the module card.
2. **Choose .ota file**: the image from RobotDyn.
3. If a **.manifest** file came with it, add it with **Add .manifest**. Without it, the hub checks that the image fits the board only after writing the whole image (about 2 minutes).
4. The app checks the file before sending it:
   - it must be an `.ota` file of exactly the size the module expects;
   - the manifest must match the `.ota`, the module's spare memory half and its board.

   If a check fails, the message says which, and the file isn't sent. If it's the version the module already runs, the app tells you; you don't need to update.
5. Start the update. The progress shows **Writing**, **Checking what was written**, **Switching the module to the new firmware**, then **Done**.

While switching, the module is off the bus for 10–15 seconds; that is expected. If the new firmware doesn't start, the module goes back to the old one by itself.

You can close the window with **Hide**; the update continues, and opening the dialog again picks it up.

Module firmware updates work from hub firmware v1.1.0, on STD modules only.

### Results

| Message | Meaning |
|---|---|
| The module runs the new firmware: *old* → *new*. | Done. |
| The module stayed on its previous firmware. Reason: … | The update didn't apply; the module runs its old firmware. **Hub log →** opens [Events](web-events.md#hub-log); the hub log is on its **Log** tab. |
| The hub is already updating a module. | Wait for it to finish, then try again. |
| The hub refused / Could not send the file | The message gives the reason. |

## Chart refresh

Charts refresh themselves: the hour view once a minute, the day view once every 5 minutes, and longer ranges once every 15 minutes.
