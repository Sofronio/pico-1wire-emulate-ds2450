# Implementation Document — RP2040 DS2450 Emulator

## 1. Project Structure

```
pico-1wire-emulate-ds2450/
├── rp2040-firmware/
│   ├── CMakeLists.txt           pico-sdk build
│   ├── ds2450_emulator.h         Public API (2 functions)
│   ├── ds2450_emulator.c         Core implementation (~280 lines)
│   └── main.c                    Entry point + heartbeat LED
├── esp32-firmware/
│   ├── esp32-firmware.ino        ESP32 Arduino sketch (DS2450 + OLED)
│   ├── DS2450.h/cpp              Joe Bechter's DS2450 driver
│   └── README.md
├── plan.md                       Development roadmap
├── fsd.md                        Functional specification
├── impl.md                       This file
├── journey.md                    Development log
└── README.md
```

## 2. RP2040 Firmware Architecture

### State Machine

```
ST_IDLE ──(line low > 400µs)──► ST_RESET_LOW ──(line high)──► ST_PRESENCE_GAP
  ▲                                                               │
  │                          ┌──── 35µs elapsed ──────────────────┘
  │                          ▼
  │                     ST_PRESENCE_LOW ──(120µs elapsed)──► release
  │                                                               │
  │                          ┌────────────────────────────────────┘
  │                          ▼
  │                     ST_ROM_CMD ←── (receive 8 write-slot bits)
  │                          │
  │              ┌───────────┼───────────┐
  │              ▼           ▼           ▼
  │         0x33 READ    0x55 MATCH   0xCC SKIP
  │         ROM           ROM          │
  │              │           │           │
  │              ▼           ▼           │
  │         ST_TX_ROM_ID  ST_RX_MATCH   │
  │         (blocking)    _ADDR         │
  │              │        (blocking)    │
  │              │           │           │
  │              │      ┌────┴────┐     │
  │              │      ▼         ▼     │
  │              │    match     no match│
  │              │      │         │     │
  │              │      ▼         ▼     │
  │              │  ST_FUNC_CMD  ST_IDLE│
  │              │      │               │
  │              │  ┌───┼───────────┐   │
  │              │  ▼   ▼           ▼   │
  │              │ 0x55 0xAA       0x3C │
  │              │ WRITE READ     CONVERT│
  │              │ MEM   MEM            │
  │              │ (blk) (blk)    (blk) │
  │              │  │               │   │
  │              │  ▼               ▼   │
  │              │ ST_IDLE     ST_CONVERT│
  │              │             _POLL     │
  │              │           (blk, poll │
  │              │            for 0xFF) │
  │              │               │      │
  │              └───────────────┘      │
  │                    │                │
  └────────────────────┴────────────────┘
                   ST_IDLE
```

### Key design decisions

**Non-blocking vs blocking split:**
- RESET, PRESENCE, ROM command (ST_ROM_CMD): **non-blocking** — each poll() call does one micro-step
- All data transmission (TX_ROM_ID, RX_MATCH_ADDR, WRITE_MEM, READ_MEM, CONVERT, CONVERT_POLL): **blocking** — busy-wait inside poll() until operation completes

**Multi-byte WRITE MEMORY:**
The ESP32 sends one WRITE MEMORY command (0x55) + address, then streams multiple data bytes. Our handler loops: read byte → store → transmit CRC+verify → repeat. Exit when `rx_byte()` detects RESET (line low > 250µs timeout).

**Temperature sweep:**
A 500ms timer in `ds2450_emulator_poll()` calls `temp_tick()` which advances `temp_tenths` and updates the ADC memory. Independent of 1-Wire activity — the current value is returned on every READ MEMORY.

### Critical: rx_byte() RESET detection

```c
static int rx_bit(void) {
    if (!wait_falling(SLOT_TIMEOUT_US)) return -1;
    busy_wait_us(SLOT_SAMPLE_US);
    int bit = line_high() ? 1 : 0;
    uint64_t t0 = time_us_64();
    while (line_low()) {
        if (time_us_64() - t0 > SLOT_TIMEOUT_US) {
            // RESET: line still low after 250µs
            while (line_low());  // wait for release
            return -2;  // signal RESET to caller
        }
    }
    return bit;
}
```

This is how the write-memory handler knows to exit — `rx_byte()` returns -1 when it detects a RESET during bit reception.

## 3. ESP32 Firmware

Modified from the original (which used `ESP32_DS2450_ThermometerReader.ino`). Key changes:
- Renamed `.ino` to match directory (Arduino CLI requirement)
- Pinout: GPIO 5 (1-Wire), GPIO 32/33 (I2C OLED)
- Expected DS2450 address: `20 1C 86 00 00 00 00 9E`
- Displays raw temp and emissivity-corrected temp on SSD1306

## 4. CRC-16 Implementation

```c
static uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x01)
            crc = (crc >> 1) ^ 0xA001;  // Dallas polynomial
        else
            crc >>= 1;
    }
    return crc;
}
```

## 5. Memory Update

```c
static void temp_tick(void) {
    temp_tenths += temp_dir;
    if (temp_tenths >= 300) temp_dir = -1;  // 30.0 → go down
    if (temp_tenths <= 200) temp_dir = 1;   // 20.0 → go up
    uint16_t raw = (uint16_t)temp_tenths * 10;  // e.g. 253 → 2530
    memory[0] = raw & 0xFF;        // Ch A LSB
    memory[1] = (raw >> 8);        // Ch A MSB
    // ... same for channels B, C, D
}
```

## 6. Key bugs found and fixed

| Bug | Symptom | Fix |
|-----|---------|-----|
| Inverted CRC | ESP32 CRC check fails, "No Probe" | READ MEMORY transmits `~crc` per DS2450 datasheet |
| Multi-byte write | State machine corrupted after begin() | Loop in write handler until RESET detected |
| State stuck after function | Slave misses next RESET | Return to ST_IDLE after each function command |
| Temperature never changes | Stuck at initial value | 500ms timer advances temp independently |
