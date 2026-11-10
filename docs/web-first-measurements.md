# First Measurements

This walks through taking your first reading on a running hub: connect a module, bind a channel to a point, confirm the numbers are right, and fix the usual first-time problems. Everything here is done in the web app (or the API) — no code. If you have not flashed and set up the hub yet, do *Build from Source* (or flash a release image) and the first-boot Wi-Fi steps first.

## 1. Connect a module

1. Wire an **rbAmp module** to the hub's I²C bus and power the hub.
2. Clip a current transformer (CT) on a conductor — **one conductor only** (live *or* neutral, not both), with the CT's arrow pointing toward the load.
3. For a module whose voltage input is used, connect its voltage input as described with the module.

For the physical install, wiring and the CT lineup, follow the [rbAmp module docs](https://github.com/rb-amp/rbamp-docs). Right after boot the hub probes its modules: **Points** may show "Device is initializing — retrying…" and tries again by itself. A hub on v1.1.4 also looks again for a missing module every 10 seconds.

## 2. Bind a channel to a point

1. Open **Management → Points**. Under **rbAmp modules**, the module appears as a card, its model on the title line and below it a line such as `module 0 · 0x50 · 1U + 7I · 1 of 7 in use` (on a fresh hub, channel I0 is already the input). Each current channel is listed with its sensor: a bound one shows the point it feeds, a free one shows `free`.
2. Go to **Free sensors**, find the channel and press **Bind**. In **Bind sensor as a point**, give it a **Name** (e.g. *Water heater*) and a **Role**. **Point ID**, **Phase**, **Line rating, kW** and **Location** are optional. There's no history setting: the hub keeps hourly history for every point, up to twelve points. The channel then shows the point name. → [Binding a point](web-points.md#binding-a-point)
3. Check the clamp model. On a module that takes a clamp-on CT of your choice (SCT series), the model is the point's **Current sensor**, in **Point settings** on the point's page. The factory set it to the sensor the channel was calibrated with. **Change it only if a different clamp is really fitted:** a new model is written to the module with its type calibration, and the channel's factory calibration is lost for good. On a factory-paired channel (CT series) the model is fixed. On an uncalibrated module it can't be chosen. → [Current sensor](web-points.md#point-settings)

On a fresh hub, the voltage input of the module at `0x50` appears on its own as a voltage point, **Main Voltage Bus**. It cannot be deleted, and neither can the main input. Modules added later get no voltage point of their own. If such a module sees mains, its voltage still feeds the hub's mains voltage and frequency.

## 3. Verify the reading

Open the point and check against something you know:

- **The load is metered the right way round.** A clamp fitted backwards shows differently by module:
  - On a **BASIC** module the point reads **sensor reversed — not metered**. A BASIC module meters forward flow only, so the load is **not metered at all** until you fix it.
  - On an **STD** module a reversed clamp reads as export (negative power), with no flag.

  Either way, turn the clamp round on the wire, with the arrow on its body towards the load. There's no software setting for the direction.

  > ⚠️ **One exception to "negative means backwards": a point with the Generation role.** There the arrow also points towards the load — from the inverter towards the house — and negative power is exactly what you want: the role books that flow as production, into **Energy out**. Turning that clamp round is what breaks it. → [Points](web-points.md#roles)
- **The value is plausible.** Switch a known load on and off (a kettle, a heater) and watch the point follow it. The reading is the current on **that one conductor** — RBgrid never multiplies it by the number of phases.
- **No limit flags.** If the point shows **`clipping`** or **`sensor limit`**, the current is at or near the CT's range: readings above a CT's rating are under-counted and the module cannot tell, so fit a CT with a higher rating (or move it to a smaller branch). The flags appear on the point's tile on the Overview, in **Load lines**, in the points table and in **Needs attention**, and the point's page shows an alert. → [The Overview](web-dashboard.md)
- **Correction applied.** If the module card shows **"readings not corrected"** with a reason, the module did not apply its CT correction, and the values may differ from the real ones. Check that the **Current sensor** set for the point matches the clamp, and that the module recognises it.

## 4. Troubleshoot

**No data on the point**
- Right after boot the hub is still probing its modules; the page retries by itself.
- Confirm the channel is bound (not `free`) and the CT is clipped around a conductor.
- Check the module's I²C wiring and that it appears on the expected address.

**Reading is zero or tiny**
- On a BASIC module, very small currents read as zero, and energy under that threshold isn't counted. Live current, power and power factor read 0 below about 1.5 % of the sensor's nominal current. A 15-minute period whose average power stays below about 0.2 % of the sensor's nominal power counts no energy, so tiny standby loads aren't counted. Put a real load on the circuit to confirm.
- A large clamp on a small circuit makes this threshold higher: choose a clamp rating close to the circuit.
- Make sure the CT is around **one** conductor — a CT around both live and neutral sees (almost) no net current.

**"Sensor reversed — not metered", or a negative reading on a consuming load**
- The CT is fitted backwards. Turn it round on the wire (arrow towards the load). On a BASIC module the load isn't metered until you do.

**Reading looks too high / too low by a round factor**
- Check that the **Current sensor** set for the point matches the clamp fitted; a mismatch applies the wrong correction. Change it only if a different clamp is really fitted.
- Do not divide a three-phase total by three in your own tooling — each point already reports one conductor only.

Once the point reads correctly, add the rest of your channels the same way and set your tariff plan ([Tariffs](web-tariffs.md)) to turn measurements into costs.
