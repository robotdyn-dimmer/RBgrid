# Sensors and modules

## Overview

All measurement in RBgrid is done by **rbAmp modules** on the hub's I2C bus. A hub with firmware v1.1.2 or later reads up to **8** modules. You see them, and add new ones, on the **Points** page under **rbAmp modules**. What you measure with each channel is set up as a point (see [Points](points.md)).

The app saves module and channel settings directly to the hub. They are kept in the hub's internal flash and survive restarts; no external memory module is needed.

## Measurement model

All voltage and current measurement is performed by the **rbAmp modules on the I2C bus**; each module handles synchronisation and digitisation itself. Channels are configured by their CT model (below).

Each module exposes a voltage input (`U`) and current channels (`I0`, `I1`, …); a module card summarises this as `1U + 7I`, `3U + 3I`, and so on (see [Module cards](#module-cards)).

- **Voltage** comes from the module's `U` input. There is no separate, user-selectable voltage sensor. In the points list the voltage input appears as a **Voltage bus** point (channel `V`). Like the main input, it cannot be removed: *"The main input and the voltage bus cannot be deleted."*
- **Current** comes from a current transformer on a channel. The point shows the channel's CT model and its range.
- **Three-phase module (STD-U3I3)** exposes three voltages and three currents. It can measure your input on all three phases (see [Use as the three-phase input](#use-as-the-three-phase-input)).
- **Frequency** is reported by the hub (the overview card's `Frequency`) and is not configurable. Known issue, hub firmware v1.1.3 and earlier: if a module's voltage inputs aren't wired (for example an STD-U3I3 used for current only), the hub may take the frequency and voltage from that module and show a wrong value, around 45 Hz. A fix that uses only a module that sees mains is in development for an upcoming release.

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

The point and its history are kept. Don't unbind it while you wait (see [Points](points.md#unbinding-a-point)).

### CT at its limit

On the tile and in **Needs attention**, a channel near its limit is flagged:

- `clipping`: the current is being clipped; the reading is **under-counted**.
- `sensor limit`: the current is at or above **85 %** of the sensor's range.

## Channel CT model

For **SCT-series** channels the CT model is chosen in the point's settings (**Current sensor**, see [Point settings](points.md#point-settings)): the SCT-013 code for the clamp actually fitted. The model is written to the module together with its type calibration. It applies at once and the module restarts; the hub doesn't.

Changing it **replaces the channel's factory calibration with the calibration for that model**. That factory calibration can't be restored, so set the model only to the clamp in use.

- **CT-series (factory-paired) modules**: the sensor is paired and calibrated at the factory, so the model is fixed and only shown.
- **Uncalibrated modules**: the model can't be chosen.

Supported SCT-013 models (voltage output, 1 V at rated current):

- `SCT-013-005` · `SCT-013-010` · `SCT-013-020` · `SCT-013-030` · `SCT-013-050` · `SCT-013-060` · `SCT-013-100`

The current-output `SCT-013-000` (50 mA) is not supported.

## Adding modules

A hub with firmware v1.1.2 or later reads up to **8** rbAmp modules on its I2C bus. Add each one from the app with **Points → Add a module**. The button appears on hubs with firmware 1.1.2 or later.

Every new rbAmp module arrives at address **0x50**, and two modules can't share an address. So the module that sits at 0x50 now moves to a free address first, and the new one is then connected at 0x50. Connect new modules **one at a time, with the hub switched off**.

The dialog works out where you are from the hub's state, so you can close it to switch the hub off and open it again later.

### Step by step

1. **Free address 0x50 (if needed).** If one of your modules already sits at 0x50, the dialog offers to move it first: **Move it to 0x5…**. The module stores its new address itself; its points keep their names, history and settings. When it's done: "Module *n* now answers at 0x5…. Address 0x50 is free for the new module."
   - If that module isn't answering, check its connection and press **Check again**; its address can only change while it answers.
   - **Find one at another address** scans for a new module that is already at another address.
2. **Switch the hub off.** To remove a module, unplug its whole cable, not only the power: a module left on the bus without power stops the readings of every module.
3. **Connect ONE new module** to the hub's I2C connector next to the others (SDA, SCL, GND, VCC).
4. **Switch the hub on**, open **Add a module** again and press **Scan**.
5. The scan shows "New module *model* at 0x50 · firmware *version*". Press **Add**.
6. "Module *n* added at 0x50." Its channels now appear under **Free sensors** on the Points page: bind them as points (see [Binding a point](points.md#binding-a-point)). To add another, press **Add another** and repeat from step 1.

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
| This hub's firmware can't add modules from the app. | Update the hub firmware (see [Settings](settings.md#hub-firmware)). |
| Couldn't read the hub's module list / The request didn't go through / The hub refused | Check the connection to the hub and try again; the message gives the reason. |

If the **Points** page itself shows **The module bus is stuck**, check the connections (a module wired without power blocks the bus), then restart the hub (see [Bus warnings](points.md#bus-warnings)).

## Use as the three-phase input

An **STD-U3I3** module measures three voltages and three currents. Its module card offers **Use as the three-phase input** when the module is present, you have an input, and the input isn't already three-phase. On hubs older than 1.1.2 the card says "Three-phase input from one *model*: update the hub to 1.1.2." instead.

1. Press **Use as the three-phase input**.
2. Confirm **Use as the input**. The input keeps its history.
3. The app says the hub will measure the input on all three phases after a restart. Press **Restart now**, or restart later.

After the restart the input is one point, **L1–L3**, with per-phase readings (see the **Per phase (U3I3)** table on the [point's page](points.md#live-values)). The Overview's **Phases** card then shows a phase-rotation check, for example "Phase rotation forward (L1 → L2 → L3)."

If the hub refuses, the app shows the reason.

## Module firmware update

**STD modules** can have their firmware updated through the hub, with an image file supplied by **RobotDyn**. The module card shows **Update firmware** when the module supports it and is present. BASIC modules aren't updated through the hub.

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
| The module stayed on its previous firmware. Reason: … | The update didn't apply; the module runs its old firmware. **Hub log →** opens [Events](events.md#hub-log); the hub log is on its **Log** tab. |
| The hub is already updating a module. | Wait for it to finish, then try again. |
| The hub refused / Could not send the file | The message gives the reason. |

## Chart refresh

Charts refresh themselves: the hour view once a minute, the day view once every 5 minutes, and longer ranges once every 15 minutes.
