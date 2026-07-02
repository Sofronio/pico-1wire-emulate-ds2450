# pico-1wire-emulate-ds2450

RP2040 (Raspberry Pi Pico) emulating a DS2450 Quad A/D Converter on the 1-Wire bus. Works with an ESP32 master that reads 4 ADC channels and displays them on an OLED.

## What it does

The Pico pretends to be a DS2450 chip — it responds to MATCH ROM, accepts WRITE MEMORY configuration, runs CONVERT commands, and returns ADC data via READ MEMORY. The temperature sweeps 20.0 → 30.0 → 20.0 in 0.1°C steps every 0.5 seconds.

```
ESP32 (master)          RP2040 Pico (DS2450 emulator)
    │                          │
    ├── RESET ────────────────►│
    │◄─────── PRESENCE ────────┤
    ├── MATCH ROM (0x55) ─────►│   ROM ID: 20 1C 86 00 00 00 00 9E
    ├── WRITE MEMORY (0x55) ──►│   configure VCC + resolution
    ├── CONVERT (0x3C) ───────►│   start fake conversion
    │◄─────── CRC-16 ──────────┤
    │◄── poll 0x00/0xFF ───────┤   0xFF = conversion complete
    ├── READ MEMORY (0xAA) ───►│
    │◄── ADC data + CRC-16 ────┤   20.0 → 30.0 → 20.0 ...
    │                          │
         OLED: "Solid  25.3"
               "Milk   24.0"
```

## Project structure

```
├── rp2040-firmware/        Pico firmware (DS2450 emulator)
│   ├── ds2450_emulator.c   State machine + protocol handlers
│   ├── ds2450_emulator.h   Public API
│   ├── main.c              Entry point
│   └── CMakeLists.txt
├── esp32-firmware/         ESP32 master (Arduino)
│   ├── esp32-firmware.ino  Main sketch with OLED display
│   ├── DS2450.cpp/h        DS2450 driver (Joe Bechter)
│   └── README.md           ESP32 wiring
└── plan.md                 Development plan
```

## Wiring

```
3.3V ──┬── 4.7kΩ ──┬────── 1-Wire bus
       │            │
  ESP32 GPIO 5  Pico GPIO 5
       │            │
      GND ───────── GND
```

## Quick start

```bash
# Build RP2040 firmware
export PATH="/path/to/arm-gnu-toolchain/bin:$PATH"
cd rp2040-firmware
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4

# Flash
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000" \
  -c "program ds2450_emulator.elf verify reset exit"

# Build & flash ESP32 master
arduino-cli compile --fqbn esp32:esp32:esp32 esp32-firmware
arduino-cli upload -p /dev/cu.wchusbserial* --fqbn esp32:esp32:esp32 esp32-firmware
```

## DS2450 protocol handled

| Phase | Command | Status |
|-------|---------|--------|
| ROM | READ ROM (0x33) | ✓ |
| ROM | MATCH ROM (0x55) — receive 8 address bytes, compare | ✓ |
| ROM | SKIP ROM (0xCC) | ✓ |
| Function | WRITE MEMORY (0x55) — multi-byte, auto-increment address | ✓ |
| Function | READ MEMORY (0xAA) — return 8 data bytes + inverted CRC-16 | ✓ |
| Function | CONVERT (0x3C) — fake conversion, poll 0xFF when done | ✓ |

## Emulated memory

24-byte DS2450 memory model (3 pages × 8 bytes). ADC values for all 4 channels are updated every 500ms, sweeping 20.0 ↔ 30.0 in 0.1°C steps. VCC control and resolution registers accept writes but don't affect behavior.

## Architecture

Same proven pattern as the DS18B20 emulator: non-blocking state machine for RESET/PRESENCE/command reception, blocking (busy-wait) for data transmission where the 2-3 µs read-slot deadline applies. CRC-16 for memory/convert integrity.
