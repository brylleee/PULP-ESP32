# PULP command reference

Everything you can send to the machine, and everything it sends back. For the
keypad and LCD, see [README.md](../README.md#how-to-use).

- [Where to type commands](#where-to-type-commands)
- [Operational settings](#operational-settings): the defaults and how to change them
- [Terminal commands](#terminal-commands): every `/COMMAND` in the dashboard
- [Raw protocol](#raw-protocol): the UART frames between the Pi and the ESP32

---

## Where to type commands

**Dashboard → admin → Terminal.** You need to be logged in as an admin.

- Start a command with `/`. Case doesn't matter: `/pulp`, `/PULP` and `PULP` are the same.
- Separate arguments with spaces (or commas).
- A pasted raw frame (`$FLASH,123,1*xx`) is also accepted. The `$` is dropped and the frame is rebuilt with a fresh ID and checksum.
- `/help` lists every command with a one-line description.

**What the terminal answers:**

- `Sent $CMD,<id>,...*<cs>`: the exact frame that went to the board.
- After a control command: the board's next status report, so you can see what changed.
- After `/GET`, `/SET` and `/UNSET`: the setting values the board sent back.

**Logging:** every command is written to the admin event log as `terminal: <admin> sent <frame> to the board`.

**Board not reporting:** the terminal still sends the command, adds `Warning: the board is not reporting, it may not be listening`, and the command may be lost.

**Board off (`/POWER_OFF`):** it ignores everything except `/POWER_ON`, `/POWER_OFF`, `/RESTART` and `/STATUS`.

---

## Operational settings

These are stored in the **ESP32's flash**:

- They survive reboots and firmware reflashes.
- The machine keeps using them while the Pi is off.
- A new value applies the next time it is used. Exceptions: the belt speed changes a running belt immediately, and a door that is resting open moves to a new open angle straight away. A pulping cycle that is already running keeps the times it started with.

### How to change them

```
/GET                        list every setting: value, range, and the default if you changed it
/GET pulp_water_ms          one setting
/SET pulp_water_ms 15000    change it (saved at once)
/UNSET pulp_water_ms        back to the default
```

- Times are in **milliseconds** (1000 = 1 s). A trailing `ms` is accepted: `/SET belt_stop_ms 200ms`.
- Values are whole numbers. A value outside the range is refused, and nothing changes: `Refused: pulp_water_ms must be 0-60000`.
- An unknown name is also refused: `Refused: <name> unknown setting`.
- `/GET` with no answer means the board's firmware predates settings, or the board is offline.

### Defaults

| Setting | Default | Range | Unit | What it controls |
|---|---|---|---|---|
| **Pulper** | | | | |
| `pulp_water_ms` | 12000 (12 s) | 0–60000 | ms | Pump run per cycle. Skipped or cut short when the water container is empty |
| `pulp_close_ms` | 1000 (1 s) | 0–10000 | ms | Wait for the door to close before the blender starts |
| `pulp_blends` | 5 | 1–50 | runs | Blender runs per cycle |
| `pulp_blend_ms` | 5000 (5 s) | 500–60000 | ms | Length of each blender run |
| `pulp_rest_ms` | 3000 (3 s) | 0–60000 | ms | Rest between blender runs (none after the last one) |
| `pulp_settle_ms` | 5000 (5 s) | 0–60000 | ms | The door stays closed this long after the last run, then opens |
| **Shredder** | | | | |
| `shred_ms` | 10000 (10 s) | 1000–30000 | ms | Shredder run per page, for the `D` wind-down, and `/SHRED`'s default |
| `shred_rest_ms` | 3000 (3 s) | 0–30000 | ms | Rest after each run. A new run waits for it |
| **Conveyor belt** | | | | |
| `belt_speed` | 70 | 1–255 | PWM duty | Belt speed for feeds and ejects |
| `belt_stop_ms` | 175 | 0–5000 | ms | Scan mode: the belt keeps running this long after the page's back edge clears the entrance IR, then stops under the camera. Lower it if pages overshoot, raise it if they stop short |
| `belt_eject_ms` | 5000 (5 s) | 500–30000 | ms | Scan mode: carries the scanned page to the shredder. Also `/CONV_RUN`'s default |
| `belt_shred_ms` | 10000 (10 s) | 500–30000 | ms | Shred-only mode: carries each page to the shredder |
| `feed_timeout_ms` | 10000 (10 s) | 2000–30000 | ms | Scan mode: a feed gives up if the page never clears the IR. The LCD shows `Page not found` |
| **Pulper door servo** | | | | |
| `door_open_deg` | 90 | 0–180 | degrees | Open, its resting position (between cycles and at power-up) |
| `door_closed_deg` | 0 | 0–180 | degrees | Closed, only while the blender runs |
| **Water sensor** | | | | |
| `water_high` | 0 | 0–1 | | `0`: the sensor pulls its output LOW when it detects water (NPN module, this machine). `1`: HIGH means water |

**A full pulping cycle with the defaults** (about 55 s; 43 s when the pump is skipped):

```
pump (pulp_water_ms) → door closes (pulp_close_ms)
  → [blend (pulp_blend_ms) → rest (pulp_rest_ms)] × pulp_blends, no rest after the last
  → wait (pulp_settle_ms) → door opens → PULP_DONE
```

Cycle length = `pulp_water_ms + pulp_close_ms + pulp_blends × pulp_blend_ms + (pulp_blends − 1) × pulp_rest_ms + pulp_settle_ms`.

### Examples

| Goal | Command |
|---|---|
| Pump water for 15 s | `/SET pulp_water_ms 15000` |
| Shorter pulping: 6 runs of 4 s | `/SET pulp_blends 6` then `/SET pulp_blend_ms 4000` |
| Pages overshoot the camera | `/SET belt_stop_ms 150` (go down 25 at a time) |
| Slower belt | `/SET belt_speed 60` (retune `belt_stop_ms` afterwards) |
| Door doesn't close fully | `/SET door_closed_deg 5` (or a few degrees the other way) |
| Shred longer after each page | `/SET shred_ms 15000` |
| Everything back to factory | `/UNSET <name>` for each one you changed (`/GET` marks those with "default …") |

### Fixed limits (not adjustable)

| Limit | Value |
|---|---|
| `/CONV_RUN` longest run | 30000 ms |
| `/SHRED` longest run | 30000 ms |
| Water sensor debounce | 5 readings, 200 ms apart (1 s) |
| Entrance IR debounce | 3 readings, 20 ms apart |
| Status report interval | every 1 s, and at most 100 ms after any change |

---

## Terminal commands

### Station (answered by the Pi itself)

| Command | What it does |
|---|---|
| `/help` (or `/?`) | Lists every command |
| `/health` | Database, harpocrates (scanner), camera, API uptime and the board's last status |
| `/clear` | Clears the terminal window (in the browser only) |

### Status

| Command | What it does |
|---|---|
| `/STATUS` | Asks the board for a status report now, and shows it: state, session mode, belt (mode, duty, speed), flash, shredder, pulper step, entrance IR, water sensor |

### Settings

| Command | Arguments | What it does |
|---|---|---|
| `/GET` | `[name]` | Lists all settings, or one |
| `/SET` | `<name> <value>` | Changes one and saves it in the board's flash |
| `/UNSET` | `<name>` | Puts one back to its default |

See [Operational settings](#operational-settings) for the names, defaults and ranges.

### Conveyor belt

| Command | Arguments | What it does |
|---|---|---|
| `/CONV_RUN` | `[ms] [duty]` | Runs the belt for `ms` (1–30000; default `belt_eject_ms`) at `duty` (1–255; default `belt_speed`). Works in any state, and replaces whatever the belt was doing |
| `/CONV_STOP` | | Stops the belt at once and drops a page waiting to be fed |
| `/CONV_SPEED` | `<1-255>` | Same as `/SET belt_speed` |
| `/CONV_STOP_MS` | `<0-5000>` | Same as `/SET belt_stop_ms` |

### Shredder

| Command | Arguments | What it does |
|---|---|---|
| `/SHRED` | `[ms]` | Runs the shredder for `ms` (1–30000; default `shred_ms`), then the rest (`shred_rest_ms`). A run already going is extended if the new one ends later. During a rest, the run waits for the rest to end |
| `/SHRED_STOP` | | Ends the run now (the rest still follows) and drops a pending run |

### Pulper

| Command | Arguments | What it does |
|---|---|---|
| `/PULP` | | Runs one pulping cycle (see [the cycle](#defaults)). Ignored while one is running. The Pi logs `PULP_DONE` when it ends |
| `/DOOR` | `open` \| `close` | Moves the door servo to `door_open_deg` / `door_closed_deg`. Ignored during a pulping cycle. The door stays put until the next cycle, which ends with it open |
| `/WATER_ACTIVE` | `low` \| `high` | Same as `/SET water_high 0` / `1` |

### Flash and display

| Command | Arguments | What it does |
|---|---|---|
| `/FLASH` | `[on\|off]` | Turns the scan flash on (no argument, `on`, `1`, `true`) or off (`off`, `0`, `false`). Stays as set until a scan session starts or ends, which turn it on and off by themselves |
| `/LCD_RESET` | | Unfreezes the LCD: frees its I2C bus and re-initialises it. The board also does this by itself when the LCD stops answering |

### Power

| Command | What it does |
|---|---|
| `/POWER_OFF` | Turns the machine off: belt, shredder, pump, blender and flash stop at once. A pulping cycle aborts (the door reopens). A scan session in progress ends, and its pages are kept. The LCD goes dark. The Pi stays on. Not kept across an ESP32 restart: it always boots on |
| `/POWER_ON` | Turns the machine back on, to standby |
| `/RESTART` | Restarts the ESP32 (all outputs off first). Settings are kept; it comes back on |

The dashboard's **Power** card does the same, and can also shut down or reboot the Pi. That needs the sudoers rule.

---

## Raw protocol

The Pi and the ESP32 talk over UART: ESP32 TX GPIO 11 → Pi RX (GPIO15), ESP32 RX GPIO 12 ← Pi TX (GPIO14), 115200 baud, 8N1.

### Frame format

```
$CMD,<id>,<arg1>,<arg2>,...*<checksum>\r\n
```

- `<id>`: a random message ID (a 32-bit unsigned number), new for every frame.
- `<checksum>`: two hex digits, the XOR of every byte between `$` and `*`.
- The Pi answers every frame from the ESP32 with `ACK` (the board ignores it), except `STATUS_REPORT`. It answers a frame it couldn't parse, or a command that failed, with `NACK`.
- The ESP32 ignores unknown commands and arguments that are out of range. To check the result, send `STATUS` (or `GET` for settings).

### ESP32 → Pi

| Frame | When |
|---|---|
| `SCANMODE,<id>,<PIN as hex>` | PIN confirmed on the keypad; a scan session opens. The PIN is the 10 keys as hex-encoded ASCII (`1A#` → `314123`), because `*` would end the frame |
| `PAGE_IN,<id>` | A sheet reached the entrance sensor (either mode); the Pi counts sheets shredded |
| `CONV_START,<id>,ir` | The belt started feeding a page. `ir`: the ESP32 stops it by itself after the IR clears |
| `SCAN,<id>` | The page is stopped under the camera (or `C` was pressed): capture it. The board waits for `SCAN_ACK` or `NACK` |
| `CONV_TIMEOUT,<id>` | The page didn't clear the IR within `feed_timeout_ms`; belt stopped, LCD shows `Page not found` |
| `SCAN_DONE,<id>` | `D` pressed (or the machine was turned off during a session): close the document |
| `PULP_DONE,<id>,<result>` | Pulping cycle ended. `ok`; `water_low` (the pump was skipped or stopped because the container was empty); `aborted` (`POWER_OFF` stopped it) |
| `POWER_ON,<id>` | `#` pressed while the machine was off; it's on again. The ESP32 also pulses GPIO 40 (Pi wake) |
| `STATUS_REPORT,<id>,...` | Telemetry, see below |
| `SETTING,<id>,<name>,<value>,<default>,<min>,<max>` | The answer to `GET`, `SET` and `UNSET` |
| `SETTING_ERR,<id>,<name>,<reason>` | `SET`, `UNSET` or `GET` refused: `unknown setting`, or `must be <min>-<max>` |

### Pi → ESP32

| Frame | Arguments | What it does |
|---|---|---|
| `SCAN_ACK,<id>` | | The page was captured: eject it to the shredder and start the shredder |
| `NACK,<id>` | | The page failed: it stays, and the LCD says to press `C` to retry |
| `CONV_PAUSE,<id>` | | Stop a feed now and scan (optional; the ESP32 stops it by itself) |
| `CONV_RUN,<id>[,<ms>[,<duty>]]` | ms ≤ 30000, duty ≤ 255 (0 or missing = default) | Run the belt |
| `CONV_STOP,<id>` | | Stop the belt |
| `CONV_SPEED,<id>,<duty>` | 1–255 | = `SET belt_speed` |
| `CONV_STOP_MS,<id>,<ms>` | 0–5000 | = `SET belt_stop_ms` |
| `SHRED,<id>[,<ms>]` | ≤ 30000 (0 or missing = `shred_ms`) | Run the shredder |
| `SHRED_STOP,<id>` | | Stop the shredder |
| `PULP,<id>` | | One pulping cycle |
| `DOOR,<id>,<0\|1>` | 1 open, 0 close | Door servo (ignored mid-cycle) |
| `WATER_ACTIVE,<id>,<0\|1>` | | = `SET water_high` |
| `FLASH,<id>,<0\|1>` | | Flash off / on |
| `LCD_RESET,<id>` | | Free the I2C bus and re-init the LCD |
| `STATUS,<id>` | | Send a `STATUS_REPORT` now |
| `GET,<id>[,<name>]` | | Answer `SETTING` for one setting, or all |
| `SET,<id>,<name>,<value>` | see the settings table | Change and save a setting; answers `SETTING` or `SETTING_ERR` |
| `UNSET,<id>,<name>` | | Back to the default; answers `SETTING` |
| `POWER_OFF,<id>` | | Machine off |
| `POWER_ON,<id>` | | Machine on |
| `RESTART,<id>` | | Restart the ESP32 |

### `STATUS_REPORT` fields

```
STATUS_REPORT,<id>,<state>,<mode>,<flash>,<belt>,<duty>,<shredder>,<paper>,<pages>,<pulper>,<speed>,<water>
```

| Field | Values |
|---|---|
| `state` | `standby`, `menu`, `pin`, `confirm_pin`, `processing`, `finishing`, `off` |
| `mode` | `scan`, `shred` (while processing), `none` |
| `flash` | `1` on, `0` off |
| `belt` | `off`, `feed`, `eject`, `manual` |
| `duty` | Current belt PWM duty, 0–255 |
| `shredder` | `idle`, `on`, `rest` |
| `paper` | `1` when the entrance IR sees paper |
| `pages` | Pages scanned in the current session |
| `pulper` | `idle`, `door` (closing, or the wait before opening), `water`, `blend`, `blend_rest` |
| `speed` | `belt_speed` |
| `water` | `1` water detected, `0` container empty |

Sent every second, at most 100 ms after any of these fields changes, and in reply to `STATUS`.
