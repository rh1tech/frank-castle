# frank-castle

Castlevania: Symphony of the Night for the RP2350 (FRANK / Murmulator M2 boards):
HDMI via HSTX, 8 MB PSRAM, I2S audio, USB keyboard, game data read straight from
your own disc image on the SD card.

Built on the [sotn-decomp](https://github.com/Xeeynamo/sotn-decomp) community
decompilation and [psyz](https://github.com/Xeeynamo/psyz). Work in progress:
currently a hardware bring-up firmware (`frank-castle-diag`).

## SD card

Copy your dump of the USA disc (SLUS-00067, BIN/CUE, MODE2/2352) into `/castle/`:

```
/castle/Castlevania - Symphony of the Night (USA).cue
/castle/Castlevania - Symphony of the Night (USA) (Track 1).bin
```

No game data is included in this repository or in its builds' sources.

## Building

```
./build.sh            # M2, 504 MHz CPU, 133 MHz PSRAM, 66 MHz flash
./flash.sh            # picotool, board in BOOTSEL
./swd_flash.sh        # or over SWD with a debugprobe (1 MHz)
```

Debug console: UART0 on GPIO0/1, 115200.

## License

AGPL-3.0-or-later (inherited from sotn-decomp). Drivers carry their own
headers: frank-fodder drivers (GPL-3.0-or-later), pico_hdmi (public domain),
FatFs (BSD-style), sdcard (BSD-2).
