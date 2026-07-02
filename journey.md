# Journey — Building a DS2450 Emulator on RP2040

## Day 1 — Understanding the target

We started with an ESP32 + SSD1306 OLED that reads a DS2450 1-Wire quad A/D converter. The ESP32 code was a working thermometer display — but the real DS2450 probe was gone. The goal: make an RP2040 pretend to be that missing probe.

First step: read the ESP32 code. It used Joe Bechter's DS2450 Arduino library, which revealed the full protocol:

1. **ROM level** — MATCH ROM (0x55) with address `20 1C 86 00 00 00 00 9E`
2. **Configuration** — WRITE MEMORY (0x55) to set VCC control (addr 0x001C) and resolution (addr 0x0008, 8 bytes)
3. **Conversion** — CONVERT (0x3C) with channel mask 0x0F, then poll for completion (0xFF)
4. **Read** — READ MEMORY (0xAA) from addr 0x0000, read 8 bytes + inverted CRC-16

This was significantly more complex than the DS18B20 emulator we'd already built. The DS18B20 only needed ROM-level responses. The DS2450 needed memory writes, memory reads, CRC-16, and a conversion state machine.

## Day 2 — First attempt, first failure

We applied the same architecture as the DS18B20 emulator: non-blocking state machine for RESET/PRESENCE/command, blocking busy-wait for data transmission.

The ROM level worked immediately — MATCH ROM matched, the slave was selected. But the ESP32 kept reporting errors. The OLED showed "No Probe."

We captured the bus with the DSLogic and discovered three bugs:

**Bug 1: Inverted CRC-16.** The DS2450 datasheet says READ MEMORY transmits the CRC-16 in *inverted* form. We were sending normal CRC. The ESP32's CRC check was failing.

**Bug 2: Multi-byte WRITE MEMORY.** The ESP32's `begin()` function sends a single WRITE MEMORY command followed by 8 data bytes (not 8 separate commands). Our handler only accepted one data byte, then returned to IDLE. The remaining data bytes were misinterpreted as RESET pulses, corrupting the state machine.

**Bug 3: State machine stuck.** After function commands, we returned to ST_FUNC_CMD to wait for the next command. But the master does RESET before each new MATCH ROM. The slave in ST_FUNC_CMD couldn't detect RESET — it interpreted the long low period as a normal write slot, timed out, and lost sync.

## Day 2 — Fixes

The fixes were straightforward once we saw the bus traces:

1. **Inverted CRC**: `tx_byte((~crc) & 0xFF)` instead of `tx_byte(crc & 0xFF)` for READ MEMORY.
2. **Multi-byte writes**: Rewrote `handle_write_memory()` to loop, reading data bytes until a RESET was detected (line low > 250µs timeout). Auto-incremented the write address.
3. **Return to IDLE**: After every function command, go to ST_IDLE instead of ST_FUNC_CMD. Wait for the next RESET. The master always does RESET before MATCH ROM.

**Temperature sweep:** Added a 500ms timer in the poll loop that advances a `temp_tenths` counter from 200 (20.0°C) to 300 (30.0°C) and back, updating the emulated ADC memory on each tick.

## What we learned

The DS2450 is a much richer protocol than the DS18B20. The DS18B20 emulator was essentially a ROM-level device — respond to MATCH ROM, ignore everything else. The DS2450 needed:

- Bidirectional data flow (master writes config, slave returns data)
- CRC-16 (the Dallas 16-bit polynomial 0xA001, distinct from the 8-bit CRC used for ROM IDs)
- A memory model (24 bytes, 3 pages)
- Auto-incrementing addresses during WRITE MEMORY
- Inverted CRC for READ MEMORY (per DS2450 datasheet)
- Conversion state machine with polling

The same core architecture worked: non-blocking for command reception, blocking (busy-wait) for data transmission where the 2-3 µs read-slot deadline applies. No PIO, no interrupts — just `busy_wait_us()` and tight polling.

## Timeline

| Phase | What | Time |
|-------|------|------|
| Research | Read ESP32 DS2450 driver, understand protocol | ~30 min |
| Build v1 | First emulator — ROM level only | ~20 min |
| Debug | Three bugs found via DSView capture | ~45 min |
| Fix | CRC, multi-byte write, state machine | ~30 min |
| Polish | Temperature sweep, README, docs | ~20 min |
