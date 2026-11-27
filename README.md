# PULP-ESP32

ESP32-S3 controller for PULP, a paper shredder and pulper that can scan pages first. You use it with a 4x4 keypad and a 16x2 I2C LCD. When you choose to scan, a Raspberry Pi captures each page over UART before the paper is shredded.

## How to use

### Keypad layout

```
┌───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ A │   A = Cancel (any screen except PIN entry)
├───┼───┼───┼───┤
│ 4 │ 5 │ 6 │ B │   B = Pi link check (standby)
├───┼───┼───┼───┤
│ 7 │ 8 │ 9 │ C │   C = Manual scan (only when a scan fails or a page is missed)
├───┼───┼───┼───┤
│ * │ 0 │ # │ D │   # = Open menu (from standby)
└───┴───┴───┴───┘   D = Finish (scan mode)
```

### 1. Standby

The device starts here and comes back here when a job ends or is cancelled.

```
[=] PULP Status:
    Standby
```

| Press | Result |
|---|---|
| `#` | Opens the mode menu |
| `B` | Shows the Pi link counters for 3 s (see below) |

`B` shows what has arrived from the Pi on the UART since power-up:

```
Pi RX B:1234
ok:12 bad:0
```

- `B:` is the bytes received, `ok` the valid frames, `bad` the broken ones.
- **All zero after the Pi has sent something:** nothing reaches the ESP32's RX (GPIO 12). Check the wire from the Pi's TX (GPIO14).
- **`bad` going up:** a signal arrives but is garbled. Check the common GND and the baud rate.

### 2. Mode menu

```
[1] Scan & Shred
[2] Shred only
```

| Press | Result |
|---|---|
| `1` | **Scan & Shred**: asks for a PIN, then scans each page before shredding (go to step 3) |
| `2` | **Shred only**: feed pages to shred without scanning (go to step 6) |
| `A` | Back to standby |

### 3. Set a PIN (Scan & Shred only)

The PIN protects the scanned file on the Pi. It is **exactly 10 keys**, and **every key counts**: `0`–`9`, `A`–`D`, `*` and `#`.

```
Input Your PIN:
PIN:******
```

- Each key press adds a `*`.
- PIN entry **cannot be cancelled**. `A` is a PIN character here, not cancel. You must enter all 10 keys.
- There is no backspace. If you make a mistake, finish the 10 keys and enter something different on the confirm screen. The mismatch sends you back to standby.
- After the 10th key the confirm screen opens:

```
Confirm PIN:
PIN:**********
```

Enter the same 10 keys again. This screen also can't be cancelled.

| Outcome | LCD (about 1.5 s) | Next |
|---|---|---|
| PINs match | ` PIN Confirmed!` | Scan session starts (step 4) |
| PINs differ | `PINs Don't Match` | Back to **standby**. Start over from `#` |

### 4. Scan session

When the session starts, the white scan flash turns on. It stays on until the session ends.

```
Insert paper
D:Finish
```

Scanning is automatic, so you don't press anything per page:

1. A page reaches the feeder entrance. The entrance IR sensor starts the belt.

   ```
   Feeding page 1
   D:Finish
   ```

2. The belt stops 175 ms after the page clears the entrance IR, with the page under the camera (see [Conveyor](#conveyor)).
3. The ESP32 asks the Pi to scan the page:

   ```
   Scanning page 1
   Please wait...
   ```

| Result | LCD | What to do |
|---|---|---|
| Page captured | `Page 1 scanned!` / `D:Finish` | Insert the next page, or press `D` to finish |
| Page failed | `Failed. Pages:0` / `C:Retry D:Finish` | Press `C` to scan the same page again, or `D` to finish |
| Page didn't clear the entrance IR (10 s) | `Page not found` / `C:Scan D:Finish` | Press `C` to scan whatever is under the camera, or `D` to finish |

`Pages:` shows how many pages were captured successfully in this session.

When a page is captured, the conveyor carries it on to the shredder for 5 s and the shredder starts for 10 s (see [Shredder](#shredder)). If you insert the next page while the belt is still holding or carrying the previous one, the next page is fed in once that 5 s run ends.

### 5. Finishing a scan session

| Press | Result |
|---|---|
| `D` | Tells the Pi to close the scan file, then runs the shredder one more time to clear it (`Shredding`, 10 s). The LCD then shows `Done` for 2 s and returns to standby |
| `A` | Cancels the session and returns to standby right away. The extra 10 s shredder run happens in the background. **The Pi is not told to close the file** |

Both keys turn off the flash. `A` stops the conveyor immediately. `D` lets a page that is already moving to the shredder finish its 5 s run.

```
[=] PULP Status:   ──►   [=] PULP Status:   ──►   Standby
    Shredding                  Done
  (10 s, longer if              (2 s)
   resting first)
```

### 6. Shred only

```
Insert paper
D:Finish
```

Feed pages into the entrance one at a time. Each page that reaches the entrance IR sensor runs the conveyor for 10 s and starts the shredder. You don't need to press anything per page.

| Press | Result |
|---|---|
| `D` | Final 10 s shredder run (`Shredding`), then `Done` for 2 s, then standby. A page already on the belt finishes its run |
| `A` | Cancels and returns to standby right away. The belt stops; the extra 10 s shredder run happens in the background |

The pulper doesn't run after a session. Only the Pi's `PULP` command starts it (see [Pulper](#pulper)).

Keys you press during the `D` wind-down (in either mode) are saved and run after it ends. For example, pressing `#` during `Done` opens the menu as soon as the device is back in standby.

### Quick reference

| Goal | Keys |
|---|---|
| Shred only | `#` `2`, feed pages, then `D` |
| Scan & shred | `#` `1`, 10-key PIN, the same PIN again, then insert pages one by one, then `D` |
| Cancel | `A` (not during PIN entry) |
| Turn on after the dashboard turned it off | `#` |

For every dashboard terminal command, the adjustable settings and the raw UART protocol, see [docs/COMMANDS.md](docs/COMMANDS.md).

## Background features

These run while you use the keypad and LCD.

### Raspberry Pi link (UART)

- The ESP32 uses Serial1, TX GPIO 11 → Pi RX (GPIO15) and RX GPIO 12 ← Pi TX (GPIO14), at 115200 baud.
- Messages are NMEA-0183-style frames: `$CMD,<id>,<args...>*<XOR checksum>\r\n`. `<id>` is a random message ID.
- Frames are also copied to the native USB port and the UART-bridge USB port for monitoring. Every line received from the Pi is printed on the native USB port as `[rx ok] ...`, or `[rx bad] ...` when the frame is broken or its checksum is wrong. The ESP32 also accepts Pi replies on those ports, so you can test from a PC without a Pi.

| Direction | Command | When |
|---|---|---|
| ESP32 → Pi | `SCANMODE,<id>,<PIN as hex>` | PIN confirmed; scan session opens |
| ESP32 → Pi | `SCAN,<id>` | Page stopped under the camera (or `C` pressed); capture it |
| ESP32 → Pi | `SCAN_DONE,<id>` | `D` pressed; close the file |
| ESP32 → Pi | `CONV_START,<id>,ir` | Paper reached the entrance and the belt started. `ir`: the ESP32 stops the belt itself, so the Pi doesn't watch |
| ESP32 → Pi | `PAGE_IN,<id>` | A sheet reached the entrance sensor (scan or shred only); the Pi counts sheets shredded |
| ESP32 → Pi | `CONV_TIMEOUT,<id>` | Belt fed for 10 s without the page clearing the IR (or a `CONV_PAUSE`); stopped for safety |
| ESP32 → Pi | `PULP_DONE,<id>,ok\|water_low\|aborted` | Pulping cycle finished; `water_low` when the container was empty (pump skipped or stopped early), `aborted` when `POWER_OFF` stopped it |
| ESP32 → Pi | `POWER_ON,<id>` | `#` pressed while the machine was off: it is on again (see [Power](#power)) |
| Pi → ESP32 | `SCAN_ACK,...` | Page captured |
| Pi → ESP32 | `NACK,...` | Page failed |
| ESP32 → Pi | `STATUS_REPORT,<id>,...` | Telemetry: every second, right away on a change, and in reply to `STATUS` (see [`STATUS_REPORT`](#status_report)) |
| Pi → ESP32 | `CONV_PAUSE,<id>` | Stop a feed now and scan (optional: the ESP32 stops it on its own) |
| Pi → ESP32 | `PULP,<id>` | Run the pulper |
| Pi → ESP32 | `FLASH`, `CONV_RUN`, `CONV_STOP`, `SHRED`, `SHRED_STOP`, `DOOR`, `LCD_RESET`, `STATUS` | Direct control of the hardware (see [Actions](#actions)) |
| Pi → ESP32 | `POWER_OFF`, `POWER_ON`, `RESTART` | Turn the machine off or on, restart the ESP32 (see [Power](#power)) |
| Pi → ESP32 | `SET`, `UNSET`, `GET` | Change, reset or read the adjustable settings (see [Settings](#settings)) |
| ESP32 → Pi | `SETTING,<id>,<name>,<value>,<default>,<min>,<max>` / `SETTING_ERR,<id>,<name>,<reason>` | The answer to `SET`, `UNSET` and `GET` |

The PIN is sent as hex-encoded ASCII (20 characters, e.g. `1A#` → `314123`), because `*` is the NMEA checksum delimiter and would break the frame. On the Pi, decode it with `bytes.fromhex(pin_hex).decode()`.

Frames from the ESP32's tasks are sent under a mutex, so two tasks can't mix their bytes on the line.

If the Pi never replies to `SCAN`, the LCD stays on `Scanning page N` until you press `A` or `D`.

### Settings

The times, speeds and angles below are kept in the ESP32's flash, so a change survives reboots and reflashes, and the machine keeps using it while the Pi is down. Change them from the dashboard terminal:

```
/GET                         all settings, with their range and default
/GET pulp_water_ms           one
/SET pulp_water_ms 15000     change one; the board saves it at once
/UNSET pulp_water_ms         back to the default
```

A new value applies from the next time it is used (the belt speed at once, and the door moves when it is resting open). A value outside the range is refused with `SETTING_ERR`.

| Name | Default | Range | What |
|---|---|---|---|
| `pulp_water_ms` | 12000 | 0–60000 | Pump run per pulping cycle |
| `pulp_close_ms` | 1000 | 0–10000 | Wait for the door to close before the blender starts |
| `pulp_blend_ms` | 5000 | 500–60000 | Each blender run |
| `pulp_rest_ms` | 3000 | 0–60000 | Rest between blender runs |
| `pulp_blends` | 5 | 1–50 | Blender runs per cycle |
| `pulp_settle_ms` | 5000 | 0–60000 | Door stays closed after the last run |
| `shred_ms` | 10000 | 1000–30000 | Shredder run (both modes, the `D` wind-down, `SHRED`'s default) |
| `shred_rest_ms` | 3000 | 0–30000 | Shredder rest after a run |
| `belt_speed` | 70 | 1–255 | Belt PWM duty for feeds and ejects (also `CONV_SPEED`) |
| `belt_stop_ms` | 175 | 0–5000 | Scan feed: belt run after the page clears the entrance IR (also `CONV_STOP_MS`) |
| `belt_eject_ms` | 5000 | 500–30000 | Scan mode: carry the page to the shredder |
| `belt_shred_ms` | 10000 | 500–30000 | Shred only: carry each page to the shredder |
| `feed_timeout_ms` | 10000 | 2000–30000 | A feed gives up (`Page not found`) |
| `door_open_deg` | 90 | 0–180 | Pulper door servo, open (its resting position) |
| `door_closed_deg` | 0 | 0–180 | Pulper door servo, closed (while blending) |
| `water_high` | 0 | 0–1 | `1`: the water sensor's HIGH means water (also `WATER_ACTIVE`) |

The defaults are the `SETTINGS` table in `src/main.cpp`. Values saved by older firmware (`CONV_SPEED`, `CONV_STOP_MS`, `WATER_ACTIVE`) are carried over.

### PIN handling

The PIN is sent to the Pi once, in the `SCANMODE` frame. It is then wiped from the ESP32's RAM. Note: this frame is also copied to the USB monitoring ports, so anyone watching the serial monitor can see the PIN.

### Scan flash

GPIO 13 drives the white flash LED through a transistor. It turns on when a scan session starts and off within about 20 ms after `D` or `A`. In between, or outside a session, the Pi's `FLASH` command can switch it, and it stays as set until the next session start or end.

### Conveyor

- A TCRT5000 IR module at the feeder entrance (GPIO 47, LOW = paper present) detects incoming paper.
- The motor driver's ENA pin is on GPIO 41. Feeds and ejects run at PWM duty 70 by default (`belt_speed`, see [Settings](#settings)). Direction pins (IN1/IN2) are not controlled by the ESP32, so wire them for forward.
- The belt reacts when paper *arrives* at the entrance (3 readings in a row, about 60 ms). It runs only during a session, in either mode.
- **Scan & Shred:** the ESP32 runs the belt. The Pi's only job is to say when to pause.
  1. **Feed:** paper arrives, the belt starts, and the ESP32 sends `CONV_START`.
  2. **Stop:** once the page's back edge leaves the entrance IR, the belt runs 175 ms more and stops with the page under the camera. The ESP32 holds the page and sends `SCAN`. `belt_stop_ms` changes the 175 ms (see [Settings](#settings)): raise it if pages stop short, lower it if they overshoot. If the IR doesn't clear within 10 s (`feed_timeout_ms`), the belt stops, the ESP32 sends `CONV_TIMEOUT` and the LCD shows `Page not found`.
  3. **Eject:** after `SCAN_ACK`, the ESP32 runs the belt for 5 s to carry the page to the shredder. After a `NACK` the page stays in place so you can retry with `C`.
  - A page that arrives while one is held or being ejected is fed in automatically once the eject ends.
- **Shred only:** the Pi isn't involved. Each arriving page runs the belt for 10 s (restarting the 10 s if it's already running) and starts the shredder. Pi conveyor commands are ignored.
- `A` stops the belt immediately. `D` lets an eject that's already running finish.
- In any state, the Pi can run the belt for a set time (`CONV_RUN`) or stop it (`CONV_STOP`). See [Actions](#actions).
- Timings are adjustable (`belt_*`, `feed_timeout_ms`, see [Settings](#settings)); the `CONV_RUN` cap is `CONVEYOR_MANUAL_MAX_MS` in `src/main.cpp`.

### Shredder

- Relay on GPIO 38, HIGH = on.
- **When it starts:**
  - Scan & Shred: each page the Pi confirms (`SCAN_ACK`).
  - Shred only: each page the entrance sensor detects.
  - Either mode: once more when the session ends with `D` or `A`, to clear the chamber.
  - Any state: the Pi's `SHRED` command (see [Actions](#actions)).
- **Cycle:** on for 10 s, then a 3 s rest.
  - A page during the 10 s restarts the 10 s; the times don't add up. In general, a new request during a run keeps it on until whichever end is later.
  - A page during the rest waits, and the shredder runs again as soon as the rest ends.
- Timings are adjustable (`shred_ms`, `shred_rest_ms`, see [Settings](#settings)); the `SHRED` cap is `SHREDDER_MAX_MS` in `src/main.cpp`.

### Pulper

- The pulper never runs on its own. It runs only when the Pi sends `PULP`, and the ESP32 replies `PULP_DONE` when the cycle ends.
- **The door stays open (90°)** between cycles. It is closed (0°) only while the blender runs.
- **Cycle (about 55 s; 43 s when the pump is skipped):**
  1. **Water:** the pump runs for 12 s. It is **skipped when the water container is low** (see [Water container](#water-container)), so the pump never runs dry.
  2. **Door:** the servo closes it, then waits 1 s for it to shut.
  3. **Blend:** the blender runs 5 s, then rests 3 s, 5 times. There is no rest after the 5th run.
  4. **Done:** after 5 s more with the door closed, the door opens again, and the ESP32 sends `PULP_DONE,<id>,ok`, or `PULP_DONE,<id>,water_low` when the pump was skipped.
- A `PULP` that arrives while a cycle is running is ignored.
- The cycle runs in the background. The keypad and LCD keep working, and the LCD doesn't show it. `STATUS` reports which step it is on.
- At power-up the door goes to open (90°), which is also where the servo library starts it, so it doesn't twitch.
- The servo needs its own 5 V supply, with GND shared with the ESP32. The ESP32's 3.3 V pin can't power it.
- Timings, the number of blender runs and the door angles are adjustable (`pulp_*`, `door_*_deg`, see [Settings](#settings)). The times above are the defaults.

### Water container

- An on/off water sensor on GPIO 8 sits at the very bottom of the container the pump draws from. It can't tell how full the container is, only whether any water is left.
- It is a 3-wire module on 5 V with an NPN open-collector output: it pulls the pin LOW while it detects water and lets go when the container is empty. The pin has the ESP32's pull-up (to 3.3 V, so the 5 V module can't reach the pin), and a loose wire reads "no water", so the pump stays off.
- For a module that works the other way round (HIGH on water), `/SET water_high 1` (or `WATER_ACTIVE,<id>,1`); `0` goes back to LOW. The ESP32 saves it in flash. Don't use a module that drives its output to 5 V: the ESP32's pins take 3.3 V at most.
- It is read every 200 ms. A change only counts after 5 readings in a row (1 s), so ripples from the pump don't flip it.
- **No water:** the pulper skips the pump, and a pump that is running stops at once. Either way the cycle ends with `PULP_DONE,<id>,water_low`.

### Power

The admin dashboard on the Pi can turn the machine off and on (`POWER_OFF` / `POWER_ON`).

- **Off:** the belt, shredder, pump, blender and flash stop at once, and a pulping cycle stops early (`PULP_DONE,<id>,aborted`) with the door opened again. A scan session in progress ends with `SCAN_DONE`, so the pages already scanned are kept. The LCD goes dark.
- **While off:** Pi commands other than `POWER_ON`, `POWER_OFF`, `RESTART` and `STATUS` are ignored. `STATUS_REPORT` keeps coming, with state `off`. A key other than `#` lights the LCD for 5 s to show `Press # to start`.
- **`#` turns it back on**, sends `POWER_ON` to the Pi, and pulls `PI_WAKE` (GPIO 40) LOW for 300 ms. Wired to the Pi's GPIO3 (header pin 5), that pulse starts a Pi that was shut down. Without the wire, a halted Pi needs its power unplugged and plugged back in.
- **The off state is not saved:** the ESP32 always boots on, in standby, without waiting for the Pi. When internals starts it re-applies the dashboard's power setting, so a machine switched off there goes off again once the Pi is up.
- **`RESTART`** switches every output off and restarts the ESP32. The dashboard sends it when it restarts the Pi.

### Telemetry

- `TelemetryTask` sends a `STATUS_REPORT` to the Pi every second, and right away (at most every 100 ms) when the machine's state changes: mode, belt, shredder, pulper, flash, entrance sensor, page count, or water sensor.
- Fields are listed under [`STATUS_REPORT`](#status_report).

### IR break-beam

- An IR LED on GPIO 10 sends a looping 38 kHz NEC frame to a VS1838 receiver on GPIO 9.
- `irBeamPresent()` returns false when paper blocks the beam.
- The beam runs all the time, but nothing uses it yet.

### FreeRTOS tasks

| Task | Role |
|---|---|
| `MasterTask` | State machine: handles keys and Pi replies |
| `KeypadTask` | Polls the keypad every 50 ms |
| `LcdTask` | Draws screens requested through a queue |
| `FeederTask` | Scan flash at session start/end, and page scan requests |
| `ConveyorTask` | Owns the belt: entrance sensor, feed/eject/Pi runs, timeouts |
| `ShredderTask` | Owns the shredder relay: run / rest cycle |
| `PulperTask` | Pulping cycle on the Pi's `PULP` command: water (skipped or stopped when the container is empty), door closes, blender, door opens |
| `TelemetryTask` | Reads the water sensor and sends `STATUS_REPORT` every second and on changes |

## Actions

Every thing the machine can do is one function in the **Actions** section of `src/main.cpp`. The keypad flow (FreeRTOS tasks) and the Pi's commands call the same functions, so they share the same timers and safety limits. The task that owns a part (conveyor, shredder) still makes the final call. For example, a shredder request during the 3 s rest waits for the rest to end, whoever sent it.

Below the actions, `src/hardware.h` holds the pin numbers and the raw pin I/O (`hwFlash()`, `hwConveyorDuty()`, `hwShredderRelay()`, `hwPaperAtEntrance()`). Only the actions and their owning tasks use them.

### Function reference

| Function | What it does | Pi command |
|---|---|---|
| `flashOn()` / `flashOff()` | Scan flash on / off. Stays as set until a scan session starts or ends | `FLASH,<id>,1` / `FLASH,<id>,0` |
| `flashIsOn()` | Whether the flash is on | in `STATUS` |
| `conveyorRun(ms, duty)` | Runs the belt for `ms` at PWM `duty` (0–255), in any state. Replaces whatever the belt was doing | `CONV_RUN,<id>[,<ms>[,<duty>]]` |
| `settingSet(id, value)` | Changes an adjustable setting and saves it in flash (the belt speed also applies to a run in progress) | `SET,<id>,<name>,<value>`, `CONV_SPEED,<id>,<duty>` |
| `conveyorStop()` | Stops the belt at once, in any state, and drops a page waiting to be fed | `CONV_STOP,<id>` |
| `conveyorPauseForScan()` | Scan mode: stops a feed at once and asks the Pi to scan | `CONV_PAUSE,<id>` |
| `conveyorEject()` | Scan mode: carries the scanned page to the shredder (5 s) | — (sent after `SCAN_ACK`) |
| `paperAtEntrance()` | Whether the entrance IR sensor sees paper | in `STATUS` |
| `shredderRun(ms)` | Runs the shredder for `ms`, then the 3 s rest | `SHRED,<id>[,<ms>]` |
| `shredderStop()` | Ends a run now (the rest still follows) and drops a pending one | `SHRED_STOP,<id>` |
| `shredderBusy()` | Whether a run is pending or on | in `STATUS` |
| `pulperRun()` | One pulping cycle, then `PULP_DONE`. Ignored while one runs | `PULP,<id>` |
| `pulperBusy()` | Whether a pulping cycle is running | in `STATUS` |
| `doorOpen()` / `doorClose()` | Pulper door servo to 90° / 0°. Ignored while a pulping cycle runs | `DOOR,<id>,1` / `DOOR,<id>,0` |
| `pumpOn()` / `pumpOff()` | Water pump on / off | — |
| `waterIsLow()` | Whether the pump's water container is empty | in `STATUS` |
| `blenderOn()` / `blenderOff()` | Blender relay on / off | — |
| `scanTrigger()` | Scan mode: asks the Pi to capture whatever is under the camera (`C` key) | — |
| `sendStatus()` | Sends a `STATUS_REPORT` frame | `STATUS,<id>` |
| `machinePowerOff()` | Turns the machine off (see [Power](#power)) | `POWER_OFF,<id>` |
| `machinePowerOn(fromKeypad)` | Turns it back on; from the keypad it also sends `POWER_ON` and pulses `PI_WAKE` | `POWER_ON,<id>` |
| `machineRestart()` | All outputs off, then restarts the ESP32 | `RESTART,<id>` |

### Command arguments and limits

- **Optional arguments:** a missing or non-numeric argument uses the default.
- **`CONV_RUN`:** `ms` defaults to `belt_eject_ms` (5000) and is capped at 30000 (`CONVEYOR_MANUAL_MAX_MS`). `duty` defaults to the saved belt speed. A duty above 255 also uses it.
- **`SHRED`:** `ms` defaults to `shred_ms` (10000) and is capped at 30000 (`SHREDDER_MAX_MS`).
- **`FLASH`:** anything other than `0` or `1` is ignored.
- **`CONV_SPEED`, `CONV_STOP_MS`, `WATER_ACTIVE`:** the same as `SET` of `belt_speed`, `belt_stop_ms` and `water_high`. Out of range is ignored.
- **`SET`, `UNSET`, `GET`:** see [Settings](#settings).
- **`LCD_RESET`:** frees the LCD's I2C bus and re-initialises the display. The LCD task also does this by itself whenever the LCD stops answering (relay switching can leave the bus stuck, which freezes the last screen); it then retries every 250 ms.
- **`DOOR`:** `1` opens, `0` closes. Anything else, or a `DOOR` during a pulping cycle, is ignored.
- **Unknown commands** are ignored. The ESP32 doesn't acknowledge commands; send `STATUS` to check the result.

Example: run the belt 2 s at full speed, then read the state:

```
$CONV_RUN,1234,2000,255*<cs>
$STATUS,1235*<cs>
```

### How commands mix with a session

- **Flash:** the session turns it on at the start and off at the end. Anything the Pi sets in between holds until then.
- **Belt:** `CONV_RUN` works in any state, even standby. During a scan session it takes over the belt:
  - A page arriving while it runs is fed in after it ends.
  - A feed it replaces is no longer watched for `CONV_PAUSE`.
- **Stopping the belt:** `A` stops a Pi-started run too. Leaving a session (`A`, or `D` once its eject ends) doesn't stop a Pi run; it runs out its time.
- **Shredder:** `SHRED` adds a run. The `D` wind-down waits for it, so `Shredding` lasts until the shredder stops.
- **During the `D` wind-down:** Pi commands keep being handled.

### `STATUS_REPORT`

```
STATUS_REPORT,<id>,<state>,<mode>,<flash>,<belt>,<duty>,<shredder>,<paper>,<pages>,<pulper>,<speed>,<water>
```

| Field | Values |
|---|---|
| `state` | `standby`, `menu`, `pin`, `confirm_pin`, `processing`, `finishing`, `off` |
| `mode` | `scan`, `shred` (during `processing`), otherwise `none` |
| `flash` | `1` on, `0` off |
| `belt` | `off`, `feed`, `eject`, `manual` |
| `duty` | Current conveyor PWM duty, 0–255 |
| `shredder` | `idle`, `on`, `rest` |
| `paper` | `1` if the entrance sensor sees paper |
| `pages` | Pages scanned in the current session |
| `pulper` | `idle`, `door`, `water`, `blend`, `blend_rest` |
| `speed` | Saved belt speed, 1–255 |
| `water` | `1` while the sensor at the bottom of the container detects water, `0` when it is empty (the pulper skips the pump) |

### Adding an action

1. Add the pin and its raw I/O to `hardware.h`.
2. Add the action function to the **Actions** section of `main.cpp`. If a task owns the part, send the task a request instead of writing the pin.
3. Add a case for the command in `handlePiPort()`.
4. Document it here.

## Hardware / pinout

| Part | Pins |
|---|---|
| 16x2 I2C LCD (addr `0x27`) | SDA 1, SCL 2 |
| 4x4 keypad | Rows 15, 16, 17, 18; Columns 4, 5, 6, 7 |
| Pi UART (Serial1) | TX 11, RX 12 |
| Scan flash | 13 |
| Entrance IR sensor (TCRT5000) | 47 |
| Conveyor motor driver ENA | 41 |
| Shredder relay | 38 |
| IR break-beam | RX 9, TX 10 |
| Blender relay (pulper) | 14 |
| Pulper door servo | 21 |
| Water pump | 39 |
| Water sensor, on/off, at the bottom of the pump's container | 8 |
| Pi wake (to the Pi's GPIO3, header pin 5; optional) | 40 |

## Not implemented yet

- The IR break-beam is not used yet.

## Build

PlatformIO, board `esp32-s3-devkitc-1`:

```sh
pio run -t upload
pio device monitor
```

A Wokwi simulation config is included (`wokwi.toml`, `diagram.json`).
