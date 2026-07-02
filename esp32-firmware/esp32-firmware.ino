#include <Wire.h>
#include <OneWire.h>
#include <U8g2lib.h>
#include <DS2450.h>
// define the Arduino digital I/O pin to be used for the 1-Wire network here
const uint8_t ONE_WIRE_PIN = 5;
// define the 1-Wire address of the DS2450 quad a/d here (lsb first)
uint8_t DS2450_address[] = { 0x20, 0x1C, 0x86, 0x00, 0x00, 0x00, 0x00, 0x9E };
float celsius, fahrenheit;
OneWire ow(ONE_WIRE_PIN);
DS2450 ds2450(&ow, DS2450_address);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);
long t_oled = 0;
void setup(void) {
  Serial.begin(115200);
  Wire.begin(32, 33);
  ds2450.begin();
  u8g2.begin();
}

// Corrects infrared temperature (in Celsius) based on emissivity.
// Both input and output temperatures are in degrees Celsius.
float correctTemperatureByEmissivity(float measuredCelsius, float emissivity) {
  if (emissivity <= 0.0 || emissivity > 1.0) {
    return NAN;  // Invalid emissivity
  }

  // Convert Celsius to Kelvin
  float measuredKelvin = measuredCelsius + 273.15;

  // Apply Stefan-Boltzmann correction: T_corrected^4 = T_measured^4 / emissivity
  float correctedKelvin = pow(pow(measuredKelvin, 4) / emissivity, 0.25);

  // Convert back to Celsius
  return correctedKelvin - 273.15;
}


void loop(void) {
  ds2450.update();
  if (ds2450.isError()) {
    Serial.println(millis());
    //Serial.println("Error reading from DS2450 device");
    if (millis() - t_oled > 1000) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso24_tf);  // 可根据需要换字体
      u8g2.drawStr(0, 28, "No Probe");
      u8g2.sendBuffer();
    }
  } else {
    celsius = ds2450.getVoltage(0);
    Serial.print(millis());
    Serial.print(" ");
    Serial.println(celsius, 1);

    float emissivity = 0.95;

    // Get corrected temperature
    float correctedTemp = correctTemperatureByEmissivity(celsius, emissivity);

    //fahrenheit = celsius * 1.8 + 32.0;


    char tempC[16];
    char tempCorrected[16];
    sprintf(tempC, "%.1f", celsius);
    sprintf(tempCorrected, "%.1f", correctedTemp);
    if (millis() - t_oled > 500) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso24_tf);  // 可根据需要换字体
      u8g2.drawStr(45, 28, tempC);
      u8g2.drawStr(45, 60, tempCorrected);
      u8g2.setFont(u8g2_font_logisoso16_tf);
      u8g2.drawStr(0, 28, "Solid");
      u8g2.drawStr(0, 60, "Milk ");
      u8g2.sendBuffer();
      t_oled = millis();
    }
  }
  //delay(100);


  /*
  byte i;
  byte present = 0;
  byte type_s;
  byte data[9];
  byte addr[8];
  float celsius, fahrenheit;

  if (!ds.search(addr)) {
    Serial.println("No more addresses.");
    Serial.println();
    if (millis() - t_oled > 1000) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso24_tf);  // 可根据需要换字体
      u8g2.drawStr(0, 28, "No Probe");
      u8g2.sendBuffer();
    }
    ds.reset_search();
    //delay(250);

    return;
  }

  Serial.print("ROM =");
  for (i = 0; i < 8; i++) {
    Serial.write(' ');
    Serial.print(addr[i], HEX);
  }

  if (OneWire::crc8(addr, 7) != addr[7]) {
    Serial.println("CRC is not valid!");
    return;
  }
  Serial.println();

  if (addr[0] != 0x22) {
    Serial.println("Only DS1822 is supported.");
    return;
  }
  Serial.println("  Chip = DS1822");
  type_s = 0;

  ds.reset();
  ds.select(addr);
  ds.write(0x44, 1);  // start conversion, with parasite power on at the end

  delay(1000);  // maybe 750ms is enough, maybe not
  // we might do a ds.depower() here, but the reset will take care of it.

  present = ds.reset();
  ds.select(addr);
  ds.write(0xBE);  // Read Scratchpad

  Serial.print("  Data = ");
  Serial.print(present, HEX);
  Serial.print(" ");
  for (i = 0; i < 9; i++) {  // we need 9 bytes
    data[i] = ds.read();
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.print(" CRC=");
  Serial.print(OneWire::crc8(data, 8), HEX);
  Serial.println();

  // Convert the data to actual temperature
  // because the result is a 16 bit signed integer, it should
  // be stored to an "int16_t" type, which is always 16 bits
  // even when compiled on a 32 bit processor.
  int16_t raw = (data[1] << 8) | data[0];
  if (type_s) {
    raw = raw << 3;  // 9 bit resolution default
    if (data[7] == 0x10) {
      // "count remain" gives full 12 bit resolution
      raw = (raw & 0xFFF0) + 12 - data[6];
    }
  } else {
    byte cfg = (data[4] & 0x60);
    // at lower res, the low bits are undefined, so let's zero them
    if (cfg == 0x00) raw = raw & ~7;       // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw & ~3;  // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw & ~1;  // 11 bit res, 375 ms
    //// default is 12 bit resolution, 750 ms conversion time
  }
  celsius = (float)raw / 16.0;
  fahrenheit = celsius * 1.8 + 32.0;
  Serial.print("  Temperature = ");
  Serial.print(celsius);
  Serial.print(" Celsius, ");
  Serial.print(fahrenheit);
  Serial.println(" Fahrenheit");

  // OLED 显示
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso24_tf);  // 可根据需要换字体
  char tempC[16];
  char tempF[16];
  sprintf(tempC, "%.1f C", celsius);
  sprintf(tempF, "%.1f F", fahrenheit);

  u8g2.drawStr(0, 28, tempC);
  u8g2.drawStr(0, 60, tempF);
  u8g2.sendBuffer();
  t_oled = millis();
  */
}
