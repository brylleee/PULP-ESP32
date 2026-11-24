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

2. The Pi watches through its camera and stops the belt the moment the page is centred (see [Conveyor](#conveyor)).
3. The ESP32 asks the Pi to scan the page:

   ```
   Scanning page 1
   Please wait...
   ```

| Result | LCD | What to do |
|---|---|---|
| Page captured | `Page 1 scanned!` / `D:Finish` | Insert the next page, or press `D` to finish |
| Page failed | `Failed. Pages:0` / `C:Retry D:Finish` | Press `C` to scan the same page again, or `D` to finish |
| Pi never saw the page centred (10 s) | `Page not found` / `C:Scan D:Finish` | Press `C` to scan whatever is under the camera, or `D` to finish |

`Pages:` shows how many pages were captured successfully in this session.

When a page is captured, the conveyor carries it on to the shredder for 5 s and the shredder starts (see [Shredder](#shredder)). If you insert the next page while the belt is still holding or carrying the previous one, the next page is fed in once that 5 s run ends.

### 5. Finishing a scan session

| Press | Result |
|---|---|
| `D` | Tells the Pi to close the scan file, then runs the shredder one more time to clear it (`Shredding`, 5 s). The LCD then shows `Done` for 2 s and returns to standby |
| `A` | Cancels the session and returns to standby right away. The extra 5 s shredder run happens in the background. **The Pi is not told to close the file** |

Both keys turn off the flash. `A` stops the conveyor immediately. `D` lets a page that is already moving to the shredder finish its 5 s run.

```
[=] PULP Status:   ──►   [=] PULP Status:   ──►   Standby
    Shredding                  Done
  (5 s, longer if              (2 s)
   resting first)
```

### 6. Shred only

```
Insert paper
D:Finish
```

Feed pages into the entrance one at a time. Each page that reaches the entrance IR sensor runs the conveyor for 5 s and starts the shredder. You don't need to press anything per page.

| Press | Result |
|---|---|
| `D` | Final 5 s shredder run (`Shredding`), then `Done` for 2 s, then standby. A page already on the belt finishes its run |
| `A` | Cancels and returns to standby right away. The belt stops; the extra 5 s shredder run happens in the background |

The pulper doesn't run after a session. Only the Pi's `PULP` command starts it (see [Pulper](#pulper)).

Keys you press during the `D` wind-down (in either mode) are saved and run after it ends. For example, pressing `#` during `Done` opens the menu as soon as the device is back in standby.

### Quick reference

| Goal | Keys |
|---|---|
| Shred only | `#` `2`, feed pages, then `D` |
| Scan & shred | `#` `1`, 10-key PIN, the same PIN again, then insert pages one by one, then `D` |
| Cancel | `A` (not during PIN entry) |

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
| ESP32 → Pi | `CONV_START,<id>` | Paper reached the entrance; belt started, watch for framing |
| ESP32 → Pi | `PAGE_IN,<id>` | A sheet reached the entrance sensor (scan or shred only); the Pi counts sheets shredded |
| ESP32 → Pi | `CONV_TIMEOUT,<id>` | Belt fed for 10 s without `CONV_PAUSE`; stopped for safety |
| ESP32 → Pi | `PULP_DONE,<id>,ok\|water_low` | Pulping cycle finished; `water_low` when the pump was skipped |
| Pi → ESP32 | `SCAN_ACK,...` | Page captured |
| Pi → ESP32 | `NACK,...` | Page failed |
| ESP32 → Pi | `STATUS_REPORT,<id>,...` | Telemetry: every second, right away on a change, and in reply to `STATUS` (see [`STATUS_REPORT`](#status_report)) |
| Pi → ESP32 | `CONV_PAUSE,<id>` | Page is centred under the camera; stop the belt and scan |
| Pi → ESP32 | `PULP,<id>` | Run the pulper |
| Pi → ESP32 | `FLASH`, `CONV_RUN`, `CONV_STOP`, `SHRED`, `SHRED_STOP`, `STATUS` | Direct control of the hardware (see [Actions](#actions)) |

The PIN is sent as hex-encoded ASCII (20 characters, e.g. `1A#` → `314123`), because `*` is the NMEA checksum delimiter and would break the frame. On the Pi, decode it with `bytes.fromhex(pin_hex).decode()`.

Frames from the ESP32's tasks are sent under a mutex, so two tasks can't mix their bytes on the line.

If the Pi never replies to `SCAN`, the LCD stays on `Scanning page N` until you press `A` or `D`.

### PIN handling

The PIN is sent to the Pi once, in the `SCANMODE` frame. It is then wiped from the ESP32's RAM. Note: this frame is also copied to the USB monitoring ports, so anyone watching the serial monitor can see the PIN.

### Scan flash

GPIO 13 drives the white flash LED through a transistor. It turns on when a scan session starts and off within about 20 ms after `D` or `A`. In between, or outside a session, the Pi's `FLASH` command can switch it, and it stays as set until the next session start or end.

### Conveyor

- A TCRT5000 IR module at the feeder entrance (GPIO 47, LOW = paper present) detects incoming paper.
- The motor driver's ENA pin is on GPIO 41. Feeds and ejects run at PWM duty 70 by default. The Pi's `CONV_SPEED` changes it, and the ESP32 saves the new value in flash, so it survives a reboot or a reflash. Direction pins (IN1/IN2) are not controlled by the ESP32, so wire them for forward.
- The belt reacts when paper *arrives* at the entrance (3 readings in a row, about 60 ms). It runs only during a session, in either mode.
- **Scan & Shred:** the ESP32 runs the belt. The Pi's only job is to say when to pause.
  1. **Feed:** paper arrives, the belt starts, and the ESP32 sends `CONV_START`.
  2. **Pause:** the Pi sends `CONV_PAUSE` when the page is centred. The ESP32 cuts the motor as soon as it reads the message, holds the page and sends `SCAN`. If no pause comes within 10 s, the belt stops, the ESP32 sends `CONV_TIMEOUT` and the LCD shows `Page not found`.
  3. **Eject:** after `SCAN_ACK`, the ESP32 runs the belt for 5 s to carry the page to the shredder. After a `NACK` the page stays in place so you can retry with `C`.
  - A page that arrives while one is held or being ejected is fed in automatically once the eject ends.
- **Shred only:** the Pi isn't involved. Each arriving page runs the belt for 5 s (restarting the 5 s if it's already running) and starts the shredder. Pi conveyor commands are ignored.
- `A` stops the belt immediately. `D` lets an eject that's already running finish.
- In any state, the Pi can run the belt for a set time (`CONV_RUN`) or stop it (`CONV_STOP`). See [Actions](#actions).
- Timings are `CONVEYOR_TIMEOUT_MS`, `CONVEYOR_EJECT_MS` and `CONVEYOR_MANUAL_MAX_MS` in `src/main.cpp`.

### Shredder

- Relay on GPIO 38, HIGH = on.
- **When it starts:**
  - Scan & Shred: each page the Pi confirms (`SCAN_ACK`).
  - Shred only: each page the entrance sensor detects.
  - Either mode: once more when the session ends with `D` or `A`, to clear the chamber.
  - Any state: the Pi's `SHRED` command (see [Actions](#actions)).
- **Cycle:** on for 5 s, then a 3 s rest.
  - A page during the 5 s restarts the 5 s; the times don't add up. In general, a new request during a run keeps it on until whichever end is later.
  - A page during the rest waits, and the shredder runs again as soon as the rest ends.
- Timings are `SHREDDER_ON_MS`, `SHREDDER_REST_MS` and `SHREDDER_MAX_MS` in `src/main.cpp`.

### Pulper

- The pulper never runs on its own. It runs only when the Pi sends `PULP`, and the ESP32 replies `PULP_DONE` when the cycle ends.
- **Cycle (about 1 min 23 s; 1 min 18 s when the pump is skipped):**
  1. **Door:** the servo opens it (90°) for 1 s, then closes it (0°).
  2. **Water:** the pump runs for 5 s. It is **skipped when the water container is low** (see [Water container](#water-container)), so the pump never runs dry.
  3. **Blend:** the blender runs 5 s, then rests 3 s, 10 times. There is no rest after the 10th run.
  4. **Done:** the ESP32 sends `PULP_DONE,<id>,ok`, or `PULP_DONE,<id>,water_low` when the pump was skipped.
- A `PULP` that arrives while a cycle is running is ignored.
- The cycle runs in the background. The keypad and LCD keep working, and the LCD doesn't show it. `STATUS` reports which step it is on.
- The door may twitch once at power-up: the servo library sends one 90° pulse before the firmware closes the door.
- The servo needs its own 5 V supply, with GND shared with the ESP32. The ESP32's 3.3 V pin can't power it.
- Timings are the `PULP_*` constants in `src/main.cpp`. The door angles are `DOOR_CLOSED_ANGLE` and `DOOR_OPEN_ANGLE` in `src/hardware.h`.

### Water container

- The water level sensor on GPIO 8 measures the container the pump draws from. The pump itself runs for a fixed time.
- `WATER_SENSOR_ANALOG` in `src/hardware.h` picks the sensor type:
  - `true` (default): analog strip module, read with the ADC.
  - `false`: float switch to GND, read with a pull-up. LOW means water at the switch.
- The level is read 5 times every 200 ms and the median is kept, so a single spike is ignored.
- **Percentage:** the raw reading (0–4095) is scaled between `WATER_RAW_EMPTY` and `WATER_RAW_FULL` in `src/main.cpp`.
- **Calibration:** check the `water raw` field of `STATUS_REPORT` with the sensor dry, then at the full mark, and set the two constants to those values.
- **Low water:** the container counts as low below 20 % and as refilled above 25 %. The gap stops the state flickering around the threshold. While it's low, the pulper skips the pump.

### Telemetry

- `TelemetryTask` sends a `STATUS_REPORT` to the Pi every second, and right away (at most every 100 ms) when the machine's state changes: mode, belt, shredder, pulper, flash, entrance sensor, page count or low water.
- The water percentage alone doesn't trigger an extra report, since it wobbles a little.
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
| `PulperTask` | Pulping cycle on the Pi's `PULP` command: door, water (skipped when low), blender |
| `TelemetryTask` | Reads the water level and sends `STATUS_REPORT` every second and on changes |

## Actions

Every thing the machine can do is one function in the **Actions** section of `src/main.cpp`. The keypad flow (FreeRTOS tasks) and the Pi's commands call the same functions, so they share the same timers and safety limits. The task that owns a part (conveyor, shredder) still makes the final call. For example, a shredder request during the 3 s rest waits for the rest to end, whoever sent it.

Below the actions, `src/hardware.h` holds the pin numbers and the raw pin I/O (`hwFlash()`, `hwConveyorDuty()`, `hwShredderRelay()`, `hwPaperAtEntrance()`). Only the actions and their owning tasks use them.

### Function reference

| Function | What it does | Pi command |
|---|---|---|
| `flashOn()` / `flashOff()` | Scan flash on / off. Stays as set until a scan session starts or ends | `FLASH,<id>,1` / `FLASH,<id>,0` |
| `flashIsOn()` | Whether the flash is on | in `STATUS` |
| `conveyorRun(ms, duty)` | Runs the belt for `ms` at PWM `duty` (0–255), in any state. Replaces whatever the belt was doing | `CONV_RUN,<id>[,<ms>[,<duty>]]` |
| `conveyorSetSpeed(duty)` | Sets the belt speed (1–255) for feeds and ejects, saves it in flash, and applies it to a run in progress | `CONV_SPEED,<id>,<duty>` |
| `conveyorStop()` | Stops the belt at once, in any state, and drops a page waiting to be fed | `CONV_STOP,<id>` |
| `conveyorPauseForScan()` | Scan mode: the page is centred. Stops a feed at once and asks the Pi to scan | `CONV_PAUSE,<id>` |
| `conveyorEject()` | Scan mode: carries the scanned page to the shredder (5 s) | — (sent after `SCAN_ACK`) |
| `paperAtEntrance()` | Whether the entrance IR sensor sees paper | in `STATUS` |
| `shredderRun(ms)` | Runs the shredder for `ms`, then the 3 s rest | `SHRED,<id>[,<ms>]` |
| `shredderStop()` | Ends a run now (the rest still follows) and drops a pending one | `SHRED_STOP,<id>` |
| `shredderBusy()` | Whether a run is pending or on | in `STATUS` |
| `pulperRun()` | One pulping cycle, then `PULP_DONE`. Ignored while one runs | `PULP,<id>` |
| `pulperBusy()` | Whether a pulping cycle is running | in `STATUS` |
| `doorOpen()` / `doorClose()` | Pulper door servo to 90° / 0° | — |
| `pumpOn()` / `pumpOff()` | Water pump on / off | — |
| `waterIsLow()` | Whether the pump's water container is low | in `STATUS` |
| `blenderOn()` / `blenderOff()` | Blender relay on / off | — |
| `scanTrigger()` | Scan mode: asks the Pi to capture whatever is under the camera (`C` key) | — |
| `sendStatus()` | Sends a `STATUS_REPORT` frame | `STATUS,<id>` |

### Command arguments and limits

- **Optional arguments:** a missing or non-numeric argument uses the default.
- **`CONV_RUN`:** `ms` defaults to 5000 (`CONVEYOR_EJECT_MS`) and is capped at 30000 (`CONVEYOR_MANUAL_MAX_MS`). `duty` defaults to the saved belt speed. A duty above 255 also uses it.
- **`SHRED`:** `ms` defaults to 5000 (`SHREDDER_ON_MS`) and is capped at 30000 (`SHREDDER_MAX_MS`).
- **`FLASH`:** anything other than `0` or `1` is ignored.
- **`CONV_SPEED`:** 1–255. Anything else is ignored.
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
STATUS_REPORT,<id>,<state>,<mode>,<flash>,<belt>,<duty>,<shredder>,<paper>,<pages>,<pulper>,<speed>,<water>,<water raw>,<water low>
```

| Field | Values |
|---|---|
| `state` | `standby`, `menu`, `pin`, `confirm_pin`, `processing`, `finishing` |
| `mode` | `scan`, `shred` (during `processing`), otherwise `none` |
| `flash` | `1` on, `0` off |
| `belt` | `off`, `feed`, `eject`, `manual` |
| `duty` | Current conveyor PWM duty, 0–255 |
| `shredder` | `idle`, `on`, `rest` |
| `paper` | `1` if the entrance sensor sees paper |
| `pages` | Pages scanned in the current session |
| `pulper` | `idle`, `door`, `water`, `blend`, `blend_rest` |
| `speed` | Saved belt speed, 1–255 |
| `water` | Water container level, 0–100 % |
| `water raw` | Raw sensor reading, 0–4095, for calibration |
| `water low` | `1` while the container is low (the pulper skips the pump) |

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
| Water level sensor (pump's container) | 8 |

## Not implemented yet

- The IR break-beam is not used yet.

## Build

PlatformIO, board `esp32-s3-devkitc-1`:

```sh
pio run -t upload
pio device monitor
```

A Wokwi simulation config is included (`wokwi.toml`, `diagram.json`).
