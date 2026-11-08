# rbgrid_cmd reply schemas

JSON Schema (draft 2020-12) of what the `rbgrid_cmd` action returns, for the HA side to validate against instead of
guessing. Schema version = the reply's `schema` field (now 1); a file's `$id` is `rbgrid_cmd/<name>/<version>`.

| File | Reply |
|---|---|
| `reply.schema.json` | The action response wrapper: plain (`data`) or one part of a large reply (`b64`) |
| `cmd_ack.schema.json` | Acknowledgement of `set …` / `unit …`: `data.result` ok\|error, `data.message`, `data.reply` (action result) |
| `i2c_action.schema.json` | `set i2c` (scan shown) |
| `schema`, `sensors`, `units`, `telemetry_data`, `tariff`, `system`, `economics`, `summary`, `i2c`, `espnow`, `price` | `get <section>` |
| `history.schema.json`, `series.schema.json` | `history`, `series` |

Rules:

- Inferred from one live device (ESPHome build 1.2.0-dev, S3 board, 2026-11-08) with 3 points (rbAmp main input, ESP-NOW node inputs). Every key listed under `required` was present there; arrays' items require only the keys all items had.
- **Extra keys may appear; ignore them.** A key is removed or changes meaning only with a new `schema` number.
- Numbers are typed `number`: the device prints whole floats without a fraction.
- Regenerate after a change: `tools/fetch_samples.py <dir> name='<cmd json>' …` then `tools/gen_schemas.py <dir> schemas`, and check the diff by hand.

docs: none (internal; the HA integration's contract)
