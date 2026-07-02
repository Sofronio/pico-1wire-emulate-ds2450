# Plan — RP2040 DS2450 Emulator

## What we're building

A **DS2450 Quad A/D Converter emulator** running on an RP2040 (Raspberry Pi Pico), bit-banged on a single GPIO. The ESP32 master talks to it as if it were a real DS2450 chip, reading 4 ADC channels via 1-Wire.

## How it differs from the DS18B20 emulator

The DS18B20 emulator only needed 3 protocol features:

| Feature | DS18B20 | DS2450 |
|---------|---------|--------|
| ROM level | READ ROM (0x33), SKIP ROM (0xCC) | MATCH ROM (0x55) — receive 8 address bytes, compare |
| Function commands | Ignored (went idle) | WRITE MEMORY (0x55), READ MEMORY (0xAA), CONVERT (0x3C) |
| Data direction | Slave → master only (read ROM ID) | Bidirectional — master writes config, slave returns ADC data |
| CRC | CRC-8 (Dallas, 8-bit) | CRC-16 (Dallas, 16-bit) |
| Memory model | None | 24-byte memory (3 pages × 8 bytes) |

The DS2450 emulator needs a **memory model**, **bidirectional data flow**, **CRC-16**, and a **conversion state machine**.

## DS2450 Protocol (what we must handle)

### ROM Level

After RESET + PRESENCE, the master sends one ROM command:

| Command | Code | Slave action |
|---------|------|-------------|
| READ ROM | 0x33 | Transmit 8-byte ROM ID (like DS18B20) |
| MATCH ROM | 0x55 | Receive 8 bytes, compare to our ROM ID. If match: selected. If no match: go idle. |
| SKIP ROM | 0xCC | Selected immediately (single-device bus) |
| SEARCH ROM | 0xF0 | Not implemented for now |

### Function Level — DS2450 Commands

Once selected, the master sends function commands:

#### WRITE MEMORY (0x55)

Master sends:
```
0x55 → address_lo → address_hi → data_byte → (CRC16_lsb) → (CRC16_msb) → (master reads verify byte)
```

The CRC-16 covers the command byte + address + data. The master then reads one "verify" byte (inverted CRC16 MSB? or FF?).

Slave must:
1. Receive command (0x55)
2. Receive 2-byte target address
3. Receive 1 data byte
4. Compute CRC-16 over [command, addr_lo, addr_hi, data]
5. Transmit CRC-16 LSB
6. Transmit CRC-16 MSB
7. Transmit verify byte (inverted CRC-16 LSB per DS2450 datasheet)

Actually, looking at the ESP32 code more carefully:

```cpp
_ow->write(DS2450_VCC_CONTROL_BYTE_ADDR_LO, 0);  // address byte 0
_ow->write(DS2450_VCC_CONTROL_BYTE_ADDR_HI, 0);  // address byte 1
_ow->write(DS2450_VCC_POWERED, 0);                // data byte
_ow->read();  // crc lsb
_ow->read();  // crc msb
_ow->read();  // verify
```

The master WRITES the address and data, then READS the CRC and verify bytes. This means the slave must TRANSMIT the CRC bytes. The OneWire library's `write()` with power=0 means no parasite power, standard write slot.

Wait — the `_ow->write()` function sends data FROM master TO slave. But then `_ow->read()` reads FROM slave TO master. So:

1. Master writes command (0x55) → slave receives it
2. Master writes addr_lo → slave receives it
3. Master writes addr_hi → slave receives it
4. Master writes data → slave receives it
5. Slave computes CRC-16
6. Master reads CRC LSB → slave transmits it
7. Master reads CRC MSB → slave transmits it
8. Master reads verify → slave transmits it (inverted CRC LSB)

This is tricky because step 6-8 are read slots (slave→master), immediately after write slots (master→slave). The OneWire library interleaves reads and writes transparently — each call is blocking.

#### READ MEMORY (0xAA)

Master sends:
```
0xAA → address_lo → address_hi
```
Then reads N data bytes + CRC-16. The ESP32 reads 10 bytes (8 data + 2 CRC).

Slave must:
1. Receive command + 2 address bytes
2. Transmit memory bytes starting from that address, wrapping within the page
3. Transmit CRC-16 over [command, addr_lo, addr_hi, data_bytes...]

#### CONVERT (0x3C)

Master sends:
```
0x3C → channel_mask → readout_control
```
Then reads CRC-16 (2 bytes). Then polls with read slots until it sees 0xFF (conversion complete).

Slave must:
1. Receive command + channel_mask + readout_control
2. Transmit CRC-16 over [command, mask, control]
3. Start "conversion" (just a timer — 10-20 ms)
4. During polling, transmit 0x00 while "converting", then 0xFF when "done"

## Architecture

Same pattern as the DS18B20 emulator:

```
onewire_slave_poll():
  ├─ RESET / PRESENCE detection    (non-blocking)
  ├─ ROM command receive            (non-blocking)
  ├─ Function command receive       (non-blocking)
  ├─ Memory read: transmit data     (BLOCKING — same 2-3µs deadline)
  └─ Conversion polling: respond    (non-blocking)
```

### New modules needed

| Module | Purpose |
|--------|---------|
| `ds2450_memory.h/c` | 24-byte memory model, CRC-16 computation |
| `onewire_slave.c` | Extended state machine — MATCH ROM, function commands |

### ROM ID

```
Family: 0x20 (DS2450)
Serial: [match the ESP32's expected address]
CRC-8: [computed over bytes 0-6]
```

The ESP32 expects: `{ 0x20, 0x1C, 0x86, 0x00, 0x00, 0x00, 0x00, 0x9E }`

We'll use this as our ROM ID so the ESP32 finds "its" DS2450.

### Memory layout (emulated)

```
Addr  | Content
------|----------------------------------------
0x00  | Ch A LSB (ADC result, low byte)
0x01  | Ch A MSB
0x02  | Ch B LSB
0x03  | Ch B MSB
0x04  | Ch C LSB
0x05  | Ch C MSB
0x06  | Ch D LSB
0x07  | Ch D MSB
0x08  | Control/Status (8-bit resolution, etc.)
0x09-0x0B | More config
0x0C-0x0F | Alarm thresholds (ignored for now)
0x10-0x17 | Page 2 — more config
0x18-0x1B | Reserved
0x1C  | VCC control (0x40 = VCC powered)
0x1D-0x1F | Reserved
```

The ADC values are 16-bit unsigned integers scaled to millivolts × 100 (based on the ESP32 code: `raw / 100 = voltage`). We'll return configurable fake values.

## Phases

### Phase 1 — ROM level only

ROM ID matching the ESP32's expected address. Respond to:
- READ ROM (0x33): return ROM ID
- MATCH ROM (0x55): receive 8 bytes, compare, set selected flag
- SKIP ROM (0xCC): set selected flag

### Phase 2 — Memory writes

Receive WRITE MEMORY (0x55) commands, parse address + data, update internal memory, return CRC-16 + verify.

### Phase 3 — Memory reads

Receive READ MEMORY (0xAA) commands, transmit memory bytes + CRC-16.

### Phase 4 — Conversion

Handle CONVERT (0x3C), run a fake "conversion" timer, respond 0xFF when done.

### Phase 5 — End-to-end test

Flash RP2040, connect to ESP32 + OLED display. Verify the ESP32 reads ADC values and displays them.

## Deliverables

1. **`pico-ds2450-emulator`** — Standalone RP2040 firmware, ready to flash
2. **Updated `pico-onewire-slave-library`** — Add DS2450 support as a new mode or separate module
