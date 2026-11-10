# The highest 15 minutes of the month

Some suppliers charge not only for the energy you used but for the hardest you pulled while using it: the
highest 15-minute or hourly average of a billing cycle. It is the one figure on such a bill you cannot
reconstruct afterwards — by the time the statement arrives, the quarter-hour that set it is long gone, and
nothing in the house remembers which one it was.

The hub remembers. For every billing cycle it keeps the highest 15-minute import and the highest hourly
import, when each happened, and the three heaviest days.

> ⚠️ **This is measurement, not billing.** The peak figure changes no cost in the app and is added to nothing.
> The hub has no capacity-charge calculation — it reports what you drew, and what your supplier does with
> such a figure is between you and your contract.

---

## What exactly is measured

At the **grid input**: the import of every point you set as a main supply, **summed**, for each closed
15-minute period. Three phases on one supply make one peak, not three. Export counts as zero rather than as a
negative — a peak is about what you took from the grid.

Reported as **power in watts**, because that is what a capacity figure is: a 15-minute period in which you
imported 1 kWh is a 4 kW average, whatever happened inside it.

| What the hub keeps | |
|---|---|
| **Highest 15-minute import** | the power, and the end of the period it happened in |
| **Highest hourly import** | the power, and which hour |
| **The three heaviest days** | each with its own peak hour and power |
| **Mean of those three days** | one figure, for contracts that average the worst days |
| **This hour so far** | what has accumulated in the hour currently being summed |
| **The previous cycle** | all of the above, kept so you can still read it after a cycle rolls over |

The two highest figures answer different questions. The 15-minute figure is the sharp one — a kettle, an
oven and a car charger overlapping for ten minutes will set it. The hourly figure is about sustained draw, and
a single short overlap barely moves it. Which one matters to you depends on your contract, so the hub keeps
both and decides nothing.

This needs the hub's clock to be set: a cycle is a span of local days. See [Time](web-time.md).

---

## The billing cycle

A cycle starts on a **day of the month between 1 and 28** — day 1 by default — at local midnight, and is set on
the Prices page in the app, or over the API.

The limit of 28 is not an oversight: a cycle starting on the 30th would skip February, and a figure that
silently has no cycle in some months is worse than one you set to the 28th on purpose.

### Changing the cycle day while a cycle is running
The figures for the cycle so far move into **previous**, and a new cycle opens immediately, starting at the
most recent local midnight of the new start day. The app asks you to confirm, because the current cycle's
numbers stop accumulating at that moment.

One courtesy worth knowing about: an **empty** cycle does not overwrite what is in *previous*. Change the
day twice in a row and you don't lose the last real cycle to a cycle that measured nothing.

**Reset** clears both the current and the previous cycle. It also asks for confirmation, and there is no undo —
the peaks are not in the history records, so a reset is the only copy gone.

---

## Why this will not match your supplier's figure exactly

It is measured in a different place. The hub sums the main-supply points **you** metered, with **your** clamps;
your supplier measures at its own meter, with its own clock and its own idea of when a period starts and ends.
Two honest measurements of the same house will differ.

What the hub's figure is good for is everything that happens **before** the bill: seeing which quarter-hour set
your peak, finding out it was the oven and the car together, and knowing whether last month's was better or
worse. What it is not is a prediction of a line on a statement, and the app never presents it as one.

If your supply has more than one main-supply point and one of them is not metered, the figure is low by
whatever went through it. [Points](web-points.md) is where you check which points are set as main supply.

---

## Where to find it

- The app's **Prices** page shows the current cycle, the previous one, and the start-day setting — see
  [Dynamic prices](web-prices.md).
- Over the API, the figures and the two settings (start day, reset).
- In the ESPHome build, the same figures and settings through the hub's command interface.

---

## What it does not do

- **No money.** No capacity charge, no cost, no figure in Economics.
- **Grid input only.** Not per point and not per circuit: you cannot ask for the peak of the heat pump here.
  Per-circuit power over time is [History](web-history.md).
- **Import only.** The highest export is not a peak.
- **Up to 31 days of a cycle** are tracked for the day ranking.
- **Nothing acts on it.** The hub doesn't warn you as you approach a previous peak, and it switches nothing.
  Cheap windows are about price, not power — see
  [Knowing when energy is cheapest](hub-price-notification.md).

---

## When the figure looks wrong: what to check

**The peak is zero, or absurdly low.**
No point is set as a main supply, so there is no grid input to sum. Check the roles on
[Points](web-points.md).

**The peak is lower than I know I drew.**
Either a second main supply isn't metered — the sum only includes points you set up — or the draw was shorter
than a period. A 7 kW load for five minutes inside a 15-minute period is a 15-minute average of about 2.3 kW,
and that average is what a capacity figure is.

**The 15-minute figure is much higher than the hourly one.**
Normal, and the two are measuring different things: one sharp overlap sets the 15-minute figure and barely
touches the hour it sits in.

**My figures disappeared.**
Either the cycle rolled over — look at the previous cycle — or the start day was changed, which moves the
current cycle into previous straight away. A reset clears both, and nothing restores them.

**I changed the start day and the cycle began on a date I didn't expect.**
A new cycle opens at the most recent local midnight of the new start day, which may be in the past. Set the day
you want the cycle to begin on, not today's date.

**The previous cycle holds almost nothing.**
A cycle that was cut short by a start-day change is as long as it got. The next full cycle will be a cycle.

**The peak hour is an hour nothing was running.**
Check the hub's time zone first — a figure in the right place at the wrong hour is nearly always a clock
question, not a measurement one. [Time](web-time.md).

**My supplier's capacity figure differs from the hub's.**
Expected, and explained above: different meter, different place, different clock. Use the hub's figure to find
out *what* set your peak, not to predict the charge.

> ⚠️ For any work inside an electrical panel, call a qualified electrician.
