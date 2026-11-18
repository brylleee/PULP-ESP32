# PULP-ESP32

ESP32-S3 controller for PULP, a paper shredder and pulper that can scan pages first. You use it with a 4x4 keypad and a 16x2 I2C LCD. When you choose to scan, a Raspberry Pi captures each page over UART before the paper is shredded.

## How to use

### Keypad layout

```
┌───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ A │   A = Cancel (any screen except PIN entry)
├───┼───┼───┼───┤
│ 4 │ 5 │ 6 │ B │   B = not used
├───┼───┼───┼───┤
│ 7 │ 8 │ 9 │ C │   C = Scan page (scan mode only)
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
C:Scan D:Finish
```

When a page reaches the feeder entrance, the entrance IR sensor starts the conveyor belt. The Pi watches through its camera and pauses the belt when the page is framed (see [Conveyor](#conveyor)).

When the page is in place, press **`C`** to scan it:

```
Scanning page 1
Please wait...
```

The ESP32 waits for the Pi to reply. Wait for the result before you press `C` again.

| Pi reply | LCD | What to do |
|---|---|---|
| Page captured | `Page 1 scanned!` / `C:Next D:Finish` | Put in the next page and press `C`, or press `D` to finish |
| Page failed | `Failed. Pages:0` / `C:Retry D:Finish` | Press `C` to retry the same page, or `D` to finish |

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
| Scan & shred | `#` `1`, 10-key PIN, the same PIN again, then for each page insert it and press `C`, then `D` |
| Cancel | `A` (not during PIN entry) |

## Background features

These run while you use the keypad and LCD.

### Raspberry Pi link (UART)

- The ESP32 uses Serial1, TX GPIO 11 → Pi RX (GPIO15) and RX GPIO 12 ← Pi TX (GPIO14), at 115200 baud.
- Messages are NMEA-0183-style frames: `$CMD,<id>,<args...>*<XOR checksum>\r\n`. `<id>` is a random message ID.
- Frames are also copied to the native USB port and the UART-bridge USB port for monitoring. The ESP32 also accepts Pi replies on those ports, so you can test from a PC without a Pi.

| Direction | Command | When |
|---|---|---|
| ESP32 → Pi | `SCANMODE,<id>,<PIN as hex>` | PIN confirmed; scan session opens |
| ESP32 → Pi | `SCAN,<id>` | `C` pressed; capture a page |
| ESP32 → Pi | `SCAN_DONE,<id>` | `D` pressed; close the file |
| ESP32 → Pi | `CONV_START,<id>` | Paper reached the entrance; belt started, watch for framing |
| ESP32 → Pi | `CONV_TIMEOUT,<id>` | Belt fed for 10 s without `CONV_PAUSE`; stopped for safety |
| ESP32 → Pi | `PULP_DONE,<id>` | Pulping cycle finished |
| Pi → ESP32 | `SCAN_ACK,...` | Page captured |
| Pi → ESP32 | `NACK,...` | Page failed |
| Pi → ESP32 | `CONV_PAUSE,<id>` | Page is framed for the camera; pause the belt |
| Pi → ESP32 | `PULP,<id>` | Run the pulper |

The PIN is sent as hex-encoded ASCII (20 characters, e.g. `1A#` → `314123`), because `*` is the NMEA checksum delimiter and would break the frame. On the Pi, decode it with `bytes.fromhex(pin_hex).decode()`.

Frames from the ESP32's tasks are sent under a mutex, so two tasks can't mix their bytes on the line.

If the Pi never replies to `SCAN`, the LCD stays on `Scanning page N` until you press `A` or `D`.

### PIN handling

The PIN is sent to the Pi once, in the `SCANMODE` frame. It is then wiped from the ESP32's RAM. Note: this frame is also copied to the USB monitoring ports, so anyone watching the serial monitor can see the PIN.

### Scan flash

GPIO 13 drives the white flash LED through a transistor. It is on for the whole scan session and turns off within about 20 ms after `D` or `A`.

### Conveyor

- A TCRT5000 IR module at the feeder entrance (GPIO 40, LOW = paper present) detects incoming paper.
- The motor driver's ENA pin is on GPIO 41 and gets PWM duty 100 by default. Direction pins (IN1/IN2) are not controlled by the ESP32, so wire them for forward.
- The belt reacts when paper *arrives* at the entrance (3 readings in a row, about 60 ms). It runs only during a session, in either mode.
- **Scan & Shred:** the ESP32 runs the belt. The Pi's only job is to say when to pause.
  1. **Feed:** paper arrives, the belt starts, and the ESP32 sends `CONV_START`.
  2. **Pause:** the Pi sends `CONV_PAUSE` when the page is framed. The belt stops and holds the page. If no pause comes within 10 s, the belt stops and the ESP32 sends `CONV_TIMEOUT`.
  3. **Eject:** after `SCAN_ACK`, the ESP32 runs the belt for 5 s to carry the page to the shredder. After a `NACK` the page stays in place so you can retry with `C`.
  - A page that arrives while one is held or being ejected is fed in automatically once the eject ends.
- **Shred only:** the Pi isn't involved. Each arriving page runs the belt for 5 s (restarting the 5 s if it's already running) and starts the shredder. Pi conveyor commands are ignored.
- `A` stops the belt immediately. `D` lets an eject that's already running finish.
- Timings are `CONVEYOR_TIMEOUT_MS` and `CONVEYOR_EJECT_MS` in `src/main.cpp`.

### Shredder

- Relay on GPIO 42, HIGH = on.
- **When it starts:**
  - Scan & Shred: each page the Pi confirms (`SCAN_ACK`).
  - Shred only: each page the entrance sensor detects.
  - Either mode: once more when the session ends with `D` or `A`, to clear the chamber.
- **Cycle:** on for 5 s, then a 3 s rest.
  - A page during the 5 s restarts the 5 s; the times don't add up.
  - A page during the rest waits, and the shredder runs again as soon as the rest ends.
- Timings are `SHREDDER_ON_MS` and `SHREDDER_REST_MS` in `src/main.cpp`.

### Pulper

- The pulper never runs on its own. It runs only when the Pi sends `PULP`, and the ESP32 replies `PULP_DONE` when the cycle ends.
- For now the cycle is a 3 s placeholder. The relay, water fill and door servo code is written but commented out.

### IR break-beam

- An IR LED on GPIO 10 sends a looping 38 kHz NEC frame to a VS1838 receiver on GPIO 9.
- `irBeamPresent()` returns false when paper blocks the beam.
- The beam runs all the time, but nothing uses it yet. It is meant to replace the manual `C` trigger and to drive the feeder rollers.

### FreeRTOS tasks

| Task | Role |
|---|---|
| `MasterTask` | State machine: handles keys and Pi replies |
| `KeypadTask` | Polls the keypad every 50 ms |
| `LcdTask` | Draws screens requested through a queue |
| `FeederTask` | Scan flash and page scan requests |
| `ConveyorTask` | Entrance sensor, conveyor PWM and timeouts |
| `ShredderTask` | Shredder relay: 5 s on / 3 s rest cycle |
| `PulperTask` | Pulping cycle on the Pi's `PULP` command. It is simulated for now |

## Hardware / pinout

| Part | Pins |
|---|---|
| 16x2 I2C LCD (addr `0x27`) | SDA 1, SCL 2 |
| 4x4 keypad | Rows 15, 16, 17, 18; Columns 4, 5, 6, 7 |
| Pi UART (Serial1) | TX 11, RX 12 |
| Scan flash | 13 |
| Entrance IR sensor (TCRT5000) | 40 |
| Conveyor motor driver ENA | 41 |
| Shredder relay | 42 |
| IR break-beam | RX 9, TX 10 |
| Pulper relay *(planned)* | 14 |
| Door servo *(planned)* | 21 |
| Water level sensor *(planned)* | 8 |
| Water pump *(planned)* | 39 |

## Not implemented yet

- The IR break-beam is not used yet, and `C` is still the manual scan trigger.
- Pulper hardware: relay, water fill, door servo. The code is written but commented out. Pulping is a timed placeholder.

## Build

PlatformIO, board `esp32-s3-devkitc-1`:

```sh
pio run -t upload
pio device monitor
```

A Wokwi simulation config is included (`wokwi.toml`, `diagram.json`).
