/**
 * ds2450_emulator.c — DS2450 Quad A/D Converter emulator for RP2040.
 *
 * Bit-banged 1-Wire slave on GPIO 5.  Emulates a DS2450 with:
 *   - ROM ID: 20 1C 86 00 00 00 00 9E
 *   - 4 ADC channels returning configurable 16-bit values
 *   - CRC-16 (Dallas polynomial) for memory/convert transactions
 *
 * Standard speed only.  Same architecture as the DS18B20 emulator:
 * non-blocking for command reception, blocking for data transmission.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"

/* ── pin ───────────────────────────────────────────────────────────── */

#define OW_PIN  5

/* ── ROM ID (must match ESP32 expected address) ────────────────────── */

static const uint8_t ROM_ID[8] = {
    0x20, 0x1C, 0x86, 0x00, 0x00, 0x00, 0x00, 0x9E
};

/* ── timing (microseconds, middle of 1-Wire spec windows) ──────────── */

#define RESET_THRESHOLD_US   400
#define PRESENCE_GAP_US       35
#define PRESENCE_LOW_US      120
#define SLOT_SAMPLE_US        15
#define READ_RESP_DELAY_US     3
#define READ_0_HOLD_US        40
#define SLOT_TIMEOUT_US      250

/* ── emulated DS2450 memory (24 bytes, 3 pages × 8) ────────────────── */

static uint8_t memory[24];

/* Sweeping temperature: 20.0 → 30.0 → 20.0 in 0.1°C steps, every 500ms.
   ADC value = temperature × 100 (e.g. 25.3°C = 2530 = 0x09E2) */
static int      temp_tenths = 200;   // 200 = 20.0°C
static int      temp_dir    = 1;     // +1 rising, -1 falling
static uint64_t temp_timer;          // last update timestamp

static void memory_update_adc(void) {
    uint16_t raw = (uint16_t)temp_tenths * 10;
    memory[0] = raw & 0xFF;
    memory[1] = (raw >> 8);
    memory[2] = raw & 0xFF;
    memory[3] = (raw >> 8);
    memory[4] = raw & 0xFF;
    memory[5] = (raw >> 8);
    memory[6] = raw & 0xFF;
    memory[7] = (raw >> 8);
}

/** Advance temperature by 0.1°C. Call every 500ms. */
static void temp_tick(void) {
    temp_tenths += temp_dir;
    if (temp_tenths >= 300) temp_dir = -1;
    if (temp_tenths <= 200) temp_dir = 1;
    memory_update_adc();
}

static void memory_init(void) {
    memset(memory, 0, sizeof(memory));
    memory_update_adc();
    temp_timer = time_us_64();
    memory[0x08] = 0x08;
    memory[0x09] = 0x01;
    memory[0x1C] = 0x40;
}

/* ── CRC-16 (Dallas/Maxim, polynomial 0xA001) ──────────────────────── */

static uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x01)
            crc = (crc >> 1) ^ 0xA001;
        else
            crc >>= 1;
    }
    return crc;
}

static uint16_t crc16_block(const uint8_t *data, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++)
        crc = crc16_update(crc, data[i]);
    return crc;
}

/* ── line helpers ──────────────────────────────────────────────────── */

static inline bool line_high(void) { return gpio_get(OW_PIN); }
static inline bool line_low(void)  { return !gpio_get(OW_PIN); }

static inline void release(void)   { gpio_set_dir(OW_PIN, GPIO_IN); }
static inline void drive_low(void) { gpio_set_dir(OW_PIN, GPIO_OUT); gpio_put(OW_PIN, 0); }

/* ── blocking primitives ───────────────────────────────────────────── */

static bool wait_falling(uint64_t timeout_us) {
    uint64_t t0 = time_us_64();
    while (line_high())
        if (time_us_64() - t0 > timeout_us) return false;
    return true;
}

/** Transmit one bit (slave → master, read slot). Blocking. */
static void tx_bit(int bit) {
    if (!wait_falling(SLOT_TIMEOUT_US)) return;
    busy_wait_us(READ_RESP_DELAY_US);
    if (bit == 0) {
        drive_low();
        busy_wait_us(READ_0_HOLD_US);
        release();
    }
    uint64_t t0 = time_us_64();
    while (line_low())
        if (time_us_64() - t0 > SLOT_TIMEOUT_US) break;
}

/** Transmit one byte LSB-first. Blocking. */
static void tx_byte(uint8_t val) {
    for (int i = 0; i < 8; i++)
        tx_bit((val >> i) & 1);
}

/** Receive one bit (master → slave, write slot). Blocking.
 *  Returns -1 if RESET is detected (line stays low > SLOT_TIMEOUT_US). */
static int rx_bit(void) {
    if (!wait_falling(SLOT_TIMEOUT_US)) return -1;
    busy_wait_us(SLOT_SAMPLE_US);
    int bit = line_high() ? 1 : 0;
    uint64_t t0 = time_us_64();
    while (line_low()) {
        if (time_us_64() - t0 > SLOT_TIMEOUT_US) {
            // RESET detected — line still low after timeout.
            // Wait for RESET to end, signal caller.
            while (line_low());
            return -2;  // RESET
        }
    }
    return bit;
}

/** Receive one byte LSB-first. Blocking.
 *  Returns -1 if RESET is detected mid-byte. */
static int rx_byte(uint8_t *out) {
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        int bit = rx_bit();
        if (bit == -2) return -1;  // RESET
        if (bit > 0) val |= (1 << i);
    }
    *out = val;
    return 0;
}

/** Transmit ROM ID (8 bytes). Blocking. */
static void tx_rom_id(void) {
    for (int i = 0; i < 8; i++)
        tx_byte(ROM_ID[i]);
}

/* ── blocking: write memory transaction (multi-byte) ───────────────── */

static uint16_t wr_addr;  // current write address (auto-increments)

static void handle_write_memory(void) {
    // Master writes: addr_lo, addr_hi, then N data bytes.
    // After each data byte, slave transmits CRC+verify.
    // Session ends when master sends RESET.
    uint8_t al, ah, data;
    if (rx_byte(&al) < 0) return;
    if (rx_byte(&ah) < 0) return;
    wr_addr = ((uint16_t)ah << 8) | al;

    while (1) {
        if (rx_byte(&data) < 0) return;  // RESET detected — exit write session

        // Store in memory
        if (wr_addr < 24)
            memory[wr_addr] = data;
        else if (wr_addr == 0x1C)
            memory[0x1C] = data;
        wr_addr++;  // auto-increment

        // CRC-16 over [0x55, addr_lo, addr_hi, data] for THIS byte
        uint16_t a = wr_addr - 1;
        uint8_t buf[4] = {0x55, a & 0xFF, a >> 8, data};
        uint16_t crc = crc16_block(buf, 4);
        tx_byte(crc & 0xFF);
        tx_byte(crc >> 8);
        tx_byte((~crc) & 0xFF);
    }
}

/* ── blocking: read memory transaction ─────────────────────────────── */

static void handle_read_memory(void) {
    uint8_t al, ah;
    if (rx_byte(&al) < 0) return;
    if (rx_byte(&ah) < 0) return;

    uint16_t crc = crc16_update(0, 0xAA);
    crc = crc16_update(crc, al);
    crc = crc16_update(crc, ah);

    uint16_t addr = ((uint16_t)ah << 8) | al;
    for (int i = 0; i < 8; i++) {
        uint8_t b = (addr + i < 24) ? memory[addr + i] : 0xFF;
        tx_byte(b);
        crc = crc16_update(crc, b);
    }
    // Inverted CRC (DS2450 READ MEMORY spec)
    tx_byte((~crc) & 0xFF);
    tx_byte((~crc) >> 8);
}

/* ── blocking: convert transaction ─────────────────────────────────── */

static uint64_t conv_start;
static bool     conv_done;
#define CONV_TIME_US  15000

static void handle_convert(void) {
    uint8_t ch, rc;
    if (rx_byte(&ch) < 0) return;
    if (rx_byte(&rc) < 0) return;

    uint8_t buf[3] = {0x3C, ch, rc};
    uint16_t crc = crc16_block(buf, 3);
    tx_byte(crc & 0xFF);
    tx_byte(crc >> 8);

    conv_start = time_us_64();
    conv_done  = false;
}

static void handle_convert_poll(void) {
    if (!conv_done && time_us_64() - conv_start >= CONV_TIME_US)
        conv_done = true;
    tx_byte(conv_done ? 0xFF : 0x00);
}

/* ── state machine ─────────────────────────────────────────────────── */

typedef enum {
    ST_IDLE,
    ST_RESET_LOW,
    ST_PRESENCE_GAP,
    ST_PRESENCE_LOW,
    ST_ROM_CMD,          // receive ROM command byte (non-blocking)
    ST_TX_ROM_ID,        // transmit ROM ID (blocking)
    ST_RX_MATCH_ADDR,    // receive 8 match-address bytes (non-blocking)
    ST_FUNC_CMD,         // receive function command byte (non-blocking)
    ST_WRITE_MEM,        // write memory transaction (blocking)
    ST_READ_MEM,         // read memory transaction (blocking)
    ST_CONVERT_CMD,      // convert command (blocking)
    ST_CONVERT_POLL,     // conversion polling (blocking, one byte)
} state_t;

static state_t   state;
static uint64_t  t_ref;
static int       bit_count;
static uint8_t   cmd_byte;
static int       match_idx;
static uint8_t   match_buf[8];
static bool      selected;

/* ── init ──────────────────────────────────────────────────────────── */

void ds2450_emulator_init(void) {
    gpio_init(OW_PIN);
    gpio_set_dir(OW_PIN, GPIO_IN);
    gpio_pull_up(OW_PIN);
    memory_init();
    state     = ST_IDLE;
    bit_count = 0;
    cmd_byte  = 0;
    selected  = false;
}

/* ── poll ──────────────────────────────────────────────────────────── */

void ds2450_emulator_poll(void) {
    bool high = line_high();
    uint64_t now = time_us_64();

    switch (state) {

    case ST_IDLE:
        if (line_low()) { t_ref = now; state = ST_RESET_LOW; }
        break;

    case ST_RESET_LOW:
        if (high) {
            if (now - t_ref >= RESET_THRESHOLD_US) {
                t_ref = now; state = ST_PRESENCE_GAP;
            } else { state = ST_IDLE; }
        }
        break;

    case ST_PRESENCE_GAP:
        if (now - t_ref >= PRESENCE_GAP_US) {
            drive_low(); t_ref = now; state = ST_PRESENCE_LOW;
        }
        break;

    case ST_PRESENCE_LOW:
        if (now - t_ref >= PRESENCE_LOW_US) {
            release();
            bit_count = 0; cmd_byte = 0; selected = false;
            state = ST_ROM_CMD;
        }
        break;

    /* ── ROM command: receive 8 write-slot bits ──────────────────── */
    case ST_ROM_CMD:
        if (line_low()) {
            // Wait for sample point
            busy_wait_us(SLOT_SAMPLE_US);
            int bit = line_high() ? 1 : 0;
            cmd_byte |= (bit << bit_count);
            bit_count++;
            // Wait for slot end
            uint64_t t0 = time_us_64();
            while (line_low())
                if (time_us_64() - t0 > SLOT_TIMEOUT_US) break;

            if (bit_count >= 8) {
                uint8_t cmd = cmd_byte;
                cmd_byte = 0; bit_count = 0;
                if (cmd == 0x33) {
                    state = ST_TX_ROM_ID;
                } else if (cmd == 0x55) {
                    match_idx = 0; state = ST_RX_MATCH_ADDR;
                } else if (cmd == 0xCC) {
                    selected = true; state = ST_FUNC_CMD;
                } else {
                    state = ST_IDLE;  // SEARCH ROM etc — not handled
                }
            }
        }
        break;

    /* ── Transmit ROM ID (blocking) ──────────────────────────────── */
    case ST_TX_ROM_ID:
        tx_rom_id();
        state = ST_IDLE;
        break;

    /* ── Receive 8 match-address bytes ───────────────────────────── */
    case ST_RX_MATCH_ADDR:
        for (int i = 0; i < 8; i++) {
            uint8_t b;
            if (rx_byte(&b) < 0) { state = ST_IDLE; break; }
            match_buf[i] = b;
        }
        selected = true;
        for (int i = 0; i < 8; i++)
            if (match_buf[i] != ROM_ID[i]) { selected = false; break; }
        bit_count = 0; cmd_byte = 0;
        state = selected ? ST_FUNC_CMD : ST_IDLE;
        break;

    /* ── Function command: receive 1 byte ────────────────────────── */
    case ST_FUNC_CMD:
        if (line_low()) {
            busy_wait_us(SLOT_SAMPLE_US);
            int bit = line_high() ? 1 : 0;
            cmd_byte |= (bit << bit_count);
            bit_count++;
            uint64_t t0 = time_us_64();
            while (line_low())
                if (time_us_64() - t0 > SLOT_TIMEOUT_US) break;

            if (bit_count >= 8) {
                uint8_t func = cmd_byte;
                cmd_byte = 0; bit_count = 0;
                if (func == 0x55) {
                    handle_write_memory();  // blocking
                    state = ST_IDLE;        // wait for next RESET
                } else if (func == 0xAA) {
                    handle_read_memory();   // blocking
                    state = ST_IDLE;
                } else if (func == 0x3C) {
                    handle_convert();       // blocking, goes to ST_CONVERT_POLL
                    // state set inside handle_convert path
                } else {
                    state = ST_IDLE;        // unknown command
                }
            }
        }
        break;

    /* ── Conversion polling ──────────────────────────────────────── */
    case ST_CONVERT_POLL:
        // Master is reading bytes.  Each read() = one byte = 8 read slots.
        // Blocking: transmit one byte (0x00 or 0xFF).
        handle_convert_poll();
        // If conversion done, next thing master does is RESET.
        // But master might read the 0xFF byte first, then reset.
        // Stay in poll until next RESET (detected via ST_RESET_LOW).
        if (conv_done)
            state = ST_IDLE;  // conversion complete, wait for reset
        // else: stay in ST_CONVERT_POLL for next read
        break;

    default:
        state = ST_IDLE;
        break;
    }

    // ── 500ms temperature tick ────────────────────────────────────
    if (time_us_64() - temp_timer >= 500000) {
        temp_timer += 500000;
        temp_tick();
    }
}
