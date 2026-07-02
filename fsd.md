# Functional Specification — RP2040 DS2450 Emulator

## 1. Purpose

Emulate a Maxim DS2450 1-Wire Quad A/D Converter on a Raspberry Pi Pico (RP2040). The emulator responds to an ESP32 master running Joe Bechter's DS2450 Arduino library, providing simulated ADC readings that display on an SSD1306 OLED.

## 2. System Architecture

```
┌──────────────────┐        1-Wire (GPIO 5)        ┌──────────────────┐
│   ESP32 Master   │◄─────────────────────────────►│  RP2040 Pico     │
│                  │   RESET / PRESENCE             │  (DS2450 Emu)    │
│  DS2450 Library  │   MATCH ROM (0x55)            │                  │
│  U8g2 OLED       │   WRITE MEMORY (0x55)         │  ROM ID:         │
│  OneWire         │   CONVERT (0x3C)              │  20 1C 86 00     │
│                  │   READ MEMORY (0xAA)          │  00 00 00 9E     │
└──────────────────┘                               └──────────────────┘
```

**Hardware:** Both devices on GPIO 5 with a shared 4.7kΩ pull-up to 3.3V.

## 3. ROM ID

| Byte | Value | Field |
|------|-------|-------|
| 0 | `0x20` | Family code (DS2450) |
| 1–6 | `1C 86 00 00 00 00` | Serial number |
| 7 | `0x9E` | CRC-8 over bytes 0–6 |

Matches the address hardcoded in the ESP32 sketch.

## 4. Protocol Handled

### ROM Level

| Command | Code | Behavior |
|---------|------|----------|
| READ ROM | 0x33 | Transmit 8-byte ROM ID |
| MATCH ROM | 0x55 | Receive 8 bytes, compare to ROM ID. Match → selected. Mismatch → idle. |
| SKIP ROM | 0xCC | Selected unconditionally |

### Function Level (when selected)

| Command | Code | Behavior |
|---------|------|----------|
| WRITE MEMORY | 0x55 | Receive 2-byte address + N data bytes. Store in 24-byte memory. Respond with CRC-16 + verify after each byte. Auto-increment address. Session ends on RESET. |
| READ MEMORY | 0xAA | Receive 2-byte address. Transmit 8 data bytes from memory + inverted CRC-16. |
| CONVERT | 0x3C | Receive channel mask + readout control. Transmit CRC-16. Start 15ms fake conversion timer. During polling, transmit 0x00 (converting) then 0xFF (done). |

## 5. Memory Model

24 bytes (3 pages × 8 bytes). Structure:

| Address | Content |
|---------|---------|
| 0x00-0x07 | ADC results: Ch A-D, 16-bit each, LSB first. Value = temperature × 100. |
| 0x08 | Control/Status (8-bit resolution) |
| 0x09 | POR off, no alarms, 5V range |
| 0x1C | VCC control (0x40 = powered) |

ADC values update every 500ms, sweeping 20.0 → 30.0 → 20.0 in 0.1°C increments.

## 6. CRC-16

Dallas/Maxim polynomial 0xA001. Used for:
- **WRITE MEMORY:** CRC over [0x55, addr_lo, addr_hi, data]. Transmitted normally. Verify byte = inverted CRC LSB.
- **READ MEMORY:** CRC over [0xAA, addr_lo, addr_hi, data...]. Transmitted **inverted** per DS2450 datasheet.
- **CONVERT:** CRC over [0x3C, ch_mask, rd_ctrl]. Transmitted normally.

## 7. Timing

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| RESET threshold | 400 µs | Below 480 µs spec for tolerance |
| PRESENCE gap | 35 µs | Middle of 15-60 µs window |
| PRESENCE low | 120 µs | Middle of 60-240 µs window |
| Write-slot sample | 15 µs | Standard 1-Wire sample point |
| Read-response delay | 3 µs | Let master finish init pulse |
| READ-0 hold | 40 µs | Well above 15 µs minimum |
| Conversion time | 15 ms | Fast enough for responsive display |
| Temperature step | 500 ms | Visible sweep rate on OLED |

## 8. Acceptance Criteria

| ID | Criterion |
|----|-----------|
| AC-1 | ESP32 detects PRESENCE from emulator |
| AC-2 | MATCH ROM with address `20 1C 86 00 00 00 00 9E` succeeds |
| AC-3 | WRITE MEMORY accepts multi-byte writes with correct CRC-16 |
| AC-4 | READ MEMORY returns valid ADC data with correct inverted CRC-16 |
| AC-5 | CONVERT completes within timeout, ESP32 receives 0xFF |
| AC-6 | Temperature sweeps 20.0 → 30.0 → 20.0 in visible steps |
| AC-7 | OLED displays temperature values from the emulated DS2450 |
| AC-8 | No CRC errors on ESP32 serial output |
