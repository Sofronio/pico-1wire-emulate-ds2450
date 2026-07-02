#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "ds2450_emulator.h"

#define LED_PIN 25

int main(void) {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    ds2450_emulator_init();
    sleep_ms(500);

    uint64_t last_blink = time_us_64();
    bool     led_state  = false;

    while (true) {
        ds2450_emulator_poll();

        uint64_t now = time_us_64();
        if (now - last_blink > 500000) {
            last_blink = now;
            led_state  = !led_state;
            gpio_put(LED_PIN, led_state);
        }
    }
    return 0;
}
