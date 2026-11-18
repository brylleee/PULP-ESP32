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
| `2` | **Shred only**: shreds and pulps without scanning (go to step 6) |
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

When a page reaches the feeder entrance, the entrance IR sensor starts the conveyor belt. The Pi watches through its camera and tells the ESP32 when to stop or adjust the belt (see [Conveyor](#conveyor)).

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

### 5. Finishing a scan session

| Press | Result |
|---|---|
| `D` | Tells the Pi to close the scan file. The LCD shows `Done` for 2 s, then returns to standby |
| `A` | Cancels the session and returns to standby. **The Pi is not told to close the file** |

Both keys stop the conveyor and turn off the flash.

```
[=] PULP Status:
      Done
```

### 6. Shred only

This runs by itself and needs no input:

```
[=] PULP Status:        [=] PULP Status:
    Shredding     ──►       Pulping       ──►  Standby
     (2 s)                  (3.5 s)
```

Keys you press during this sequence are saved and run after it ends. For example, pressing `#` during pulping opens the menu as soon as the device is back in standby.

### Quick reference

| Goal | Keys |
|---|---|
| Shred only | `#` `2` |
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
| ESP32 → Pi | `CONV_START,<id>` | Paper reached the entrance; belt started |
| ESP32 → Pi | `CONV_TIMEOUT,<id>` | Belt ran 10 s without a Pi command; stopped for safety |
| Pi → ESP32 | `SCAN_ACK,...` | Page captured |
| Pi → ESP32 | `NACK,...` | Page failed |
| Pi → ESP32 | `CONV_STOP,<id>` | Stop the belt |
| Pi → ESP32 | `CONV_RUN,<id>` | Run the belt at the default speed (duty 100) |
| Pi → ESP32 | `CONV_SPEED,<id>,<0-255>` | Run the belt at this PWM duty (`0` = stop) |

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
- **Only active during a Scan & Shred session.** Shred only does not use the conveyor yet.
- The belt starts when paper *arrives* at the entrance (3 readings in a row, about 60 ms). A page that is still at the entrance after the Pi stopped the belt does not restart it.
- After the belt starts, the Pi controls it with `CONV_STOP`, `CONV_RUN` and `CONV_SPEED`. If the Pi sends nothing for 10 s after the last start or command, the belt stops and the ESP32 sends `CONV_TIMEOUT`.
- `D`, `A`, or anything else that ends the session stops the belt immediately.

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
| `ConveyorTask` | Entrance sensor, conveyor PWM and the safety timeout |
| `PulperTask` | Pulping cycle. It is simulated for now |

## Hardware / pinout

| Part | Pins |
|---|---|
| 16x2 I2C LCD (addr `0x27`) | SDA 1, SCL 2 |
| 4x4 keypad | Rows 15, 16, 17, 18; Columns 4, 5, 6, 7 |
| Pi UART (Serial1) | TX 11, RX 12 |
| Scan flash | 13 |
| Entrance IR sensor (TCRT5000) | 40 |
| Conveyor motor driver ENA | 41 |
| IR break-beam | RX 9, TX 10 |
| Pulper relay *(planned)* | 14 |
| Door servo *(planned)* | 21 |
| Water level sensor *(planned)* | 8 |
| Water pump *(planned)* | 39 |

## Not implemented yet

- The IR break-beam is not used yet, and `C` is still the manual scan trigger.
- Conveyor in Shred only mode.
- Pulper hardware: relay, water fill, door servo. The code is written but commented out.
- Shredding and pulping are timed placeholders.

## Build

PlatformIO, board `esp32-s3-devkitc-1`:

```sh
pio run -t upload
pio device monitor
```

A Wokwi simulation config is included (`wokwi.toml`, `diagram.json`).
