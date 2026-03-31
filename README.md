# FlipIRFreq

FlipIRFreq is a Flipper Zero external app for sending a raw IR carrier burst or continuous carrier with a user-selected frequency, duty cycle, burst length, and output pin.

## Features

- Adjustable carrier frequency from `10 kHz` to `1 MHz` with `100 Hz` fine tuning
- Adjustable duty cycle from `1%` to `99%`
- Configurable burst length from `1 ms` to `5000 ms`
- Transmit mode selection for `Burst` or `Continuous`
- Output selection for `Auto`, `Internal`, or `External (PA7)`
- Dedicated on-screen `Send` control
- Live transmit indicator while IR is broadcasting
- Persisted settings across launches
- Single-screen workflow optimized for quick testing

## Controls

- `Up` / `Down`: select the field to edit
- `Left` / `Right`: change the selected value
- Frequency adjusts in `100 Hz` steps; hold `Left` / `Right` for faster `1 kHz` changes
- Move to `Send` and press `OK`: start transmitting
- Press `OK` again while transmitting: stop transmitting
- `Back`: stop transmission or exit the app

## Notes

- `Auto` output uses the firmware's IR output detection to pick the active transmitter.
- Burst transmission uses the Flipper infrared stack with a raw single-mark signal at the selected carrier settings.
- This app is intended for bench testing, tuning, and experimentation with IR carrier behavior.

## Project Layout

- `flipirfreq.c` - main application source
- `application.fam` - Flipper Zero app manifest
- `icon.png` - Flipper package icon

## Building

This repository contains a standard Flipper Zero external app layout and includes a helper script, `build.ps1`, that mirrors the project into your firmware tree and runs `fbt`.

Default usage:

```powershell
.\build.ps1
```

Preview the sync without copying, deleting, or building:

```powershell
.\build.ps1 -PreviewSync
```

Override the source or target directories explicitly:

```powershell
.\build.ps1 `
  -SourceDir C:\Users\Joe\Projects\FlipIRFreq `
  -FirmwareDir C:\Users\Joe\Projects\flipperzero-firmware `
  -TargetDir C:\Users\Joe\Projects\flipperzero-firmware\applications_user\flipirfreq
```

## Installing

1. Copy the generated `.fap` to your Flipper Zero SD card.
2. Launch `FlipIRFreq` from the Apps menu.

## Author

Created by ConsultingJoe.
