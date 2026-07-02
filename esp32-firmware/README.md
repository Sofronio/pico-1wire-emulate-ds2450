# esp32-thermo-display

ESP32 (Arduino) DS2450 1-Wire thermometer reader with 1.3" SSD1306 OLED.

## Hardware

| ESP32 Pin | Connected To |
|-----------|-------------|
| GPIO 5 | 1-Wire data (4.7k pull-up to 3.3V) from DS2450 probe |
| GPIO 32 | OLED SDA (I2C) |
| GPIO 33 | OLED SCL (I2C) |

## Build & Flash

```bash
# Arduino CLI
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload -p /dev/ttyUSB1 --fqbn esp32:esp32:esp32 .
```

## Libraries

- **U8g2** (olikraus/U8g2) — OLED display
- **OneWire** (paulstoffregen/OneWire) — 1-Wire master
- **DS2450** (local) — DS2450 quad A/D reader (Joe Bechter)

## OLED Display

```
+------------------------+
| Solid       31.8       |  <- raw temp (DS2450 ch0)
| Milk        30.2       |  <- emissivity corrected (0.95)
+------------------------+
```
