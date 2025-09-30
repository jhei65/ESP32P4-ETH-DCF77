#include <time.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_sntp.h"

#define DCF_VCC_GPIO 14  // GPIO-Pin für DCF77 VCC
#define DCF_PON_GPIO 16  // GPIO-Pin für DCF77 PON
#define DCF_TCO_GPIO 15  // GPIO-Pin für DCF77 TCO

static volatile bool isr = false;
static const char* TAG = "DCF77";

// ISR (Interrupt Service Routine)
void IRAM_ATTR gpio_isr_handler(void* arg) { isr = true; }

void dcf77(void* pvParameters) {
    // GPIO konfigurieren
    gpio_config_t io_conf_vcc = {
        .pin_bit_mask = (1ULL << DCF_VCC_GPIO),  // Bitmaske für den Pin
        .mode = GPIO_MODE_OUTPUT,                // OUTPUT-Mode
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf_vcc);
    gpio_set_level(DCF_VCC_GPIO, 1);

    gpio_config_t io_conf_pon = {
        .pin_bit_mask = (1ULL << DCF_PON_GPIO),  // Bitmaske für den Pin
        .mode = GPIO_MODE_OUTPUT,                // OUTPUT-Mode
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf_pon);
    gpio_set_level(DCF_PON_GPIO, 0);

    // GPIO als Input konfigurieren
    gpio_config_t io_conf_tco = {
        .pin_bit_mask = (1ULL << DCF_TCO_GPIO),  // Bitmaske für den Pin
        .mode = GPIO_MODE_INPUT,                 // INPUT-Mode
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf_tco);

    // ISR-Service config
    ESP_ERROR_CHECK(gpio_install_isr_service(0));  // default configuration
    // ISR-Handler for this pin added
    ESP_ERROR_CHECK(gpio_isr_handler_add(DCF_TCO_GPIO, gpio_isr_handler, NULL));

    ESP_LOGI(TAG, "Initializing GPTimer...");

    // GPTimer config (1 MHz = 1 µs pro Tick)
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,  // 1 MHz
    };
    gptimer_handle_t gptimer = NULL;
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    while (1) {
        static bool new_data = false;
        static uint64_t time_diff = 0;
        static uint64_t time_diff2 = 0;
        static struct tm tm_time = {0};
        if (isr) {
            isr = false;
            uint64_t now;
            static uint64_t time_diff_high = 0;
            static uint64_t time_diff_low = 0;
            // read GPTimer-counter
            gptimer_get_raw_count(gptimer, &now);
            if (gpio_get_level(DCF_TCO_GPIO) == 1) {
                time_diff_high = now;
                time_diff2 = now - time_diff_low;
            } else {
                time_diff = now - time_diff_high;
                time_diff_low = now;
                new_data = true;
            }
        }
        if (new_data) {
            new_data = false;
            static bool startTimeOK = false;
            static uint8_t second = 0;
            static bool minuteOK = false;
            static bool hourOK = false;
            static bool calenderOK = false;
            static uint8_t level = 0;

            if (time_diff2 > 1600000 && time_diff2 < 2000000) {  // detects the gap of 1.8s between two minutes
                if (calenderOK && minuteOK && hourOK && startTimeOK && second == 58) {
                    ESP_LOGI(TAG, "Valid time: %02d:%02d 20%02d-%02d-%02d Weekday: %d DST: %s", tm_time.tm_hour,
                             tm_time.tm_min, tm_time.tm_year - 100, tm_time.tm_mon + 1, tm_time.tm_mday,
                             tm_time.tm_wday, tm_time.tm_isdst == 1 ? "Yes" : "No");

                    time_t t = mktime(&tm_time);  // converts struct tm in time_t (Unix-Timestamp)
                    if (t == -1) {
                        ESP_LOGE(TAG, "Error: time couldn't convert\n");
                    } else {
                        struct timeval now = {.tv_sec = t, .tv_usec = 0};
                        settimeofday(&now, NULL);  // Systemtime set on RTC
                    }

                } else {
                    ESP_LOGE(TAG, "Not a valid time received");
                }
                second = 0;
            } else {
                if (time_diff > 25000 && time_diff < 150000) {
                    level = 0;  // Logic '1'
                } else if (time_diff > 150000 && time_diff < 300000) {
                    level = 1;  // Logic '0'
                } else {
                    // not valid pulse, ignore and
                    continue;
                }
                second++;
            }

            switch (second) {
                static uint8_t parity = 0;  // parity Minute
                case 0:
                    ESP_LOGI(TAG, "New frame");
                    break;
                case 15:
                    ESP_LOGI(TAG, "Call bit (until mid-2003 reserve antenna)  %u", level);
                    break;
                case 16:
                    ESP_LOGI(TAG, "'1': At the end of this hour DST starts or ends; otherwise '0'. %u", level);
                    break;
                case 17:
                    ESP_LOGI(TAG, "'10': Following time in CEST %u", level);
                    tm_time.tm_isdst = level;
                    break;
                case 18:
                    ESP_LOGI(TAG, "'01': Following time in CET %u", level);
                    // tm_time.tm_isdst = level == 0;
                    break;
                case 19:
                    ESP_LOGI(TAG, "'1': At the end of this hour a leap second is inserted; otherwise '0'.  %u", level);
                    break;
                case 20:
                    ESP_LOGI(TAG, "'1': Start of time information %u", level);
                    startTimeOK = level;
                    break;
                case 21:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 1 %u", level);
                    tm_time.tm_min = level;  // initialize
                    parity = level;          // Parity initialisieren
                    break;
                case 22:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 2 %u", level);
                    tm_time.tm_min += 2 * level;
                    parity ^= level;
                    break;
                case 23:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 4 %u", level);
                    tm_time.tm_min += 4 * level;
                    parity ^= level;
                    break;
                case 24:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 8 %u", level);
                    tm_time.tm_min += 8 * level;
                    parity ^= level;
                    break;
                case 25:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 10 %u", level);
                    tm_time.tm_min += 10 * level;
                    parity ^= level;
                    break;
                case 26:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 20 %u", level);
                    tm_time.tm_min += 20 * level;
                    parity ^= level;
                    break;
                case 27:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 40 %u", level);
                    tm_time.tm_min += 40 * level;
                    parity ^= level;
                    break;
                case 28:
                    ESP_LOGI(TAG, "Parity bit %u", level);
                    ESP_LOGI(TAG, "Minute: %d", tm_time.tm_min);
                    minuteOK = parity == level && tm_time.tm_min < 60;
                    ESP_LOGI(TAG, "MinuteOK:%s", minuteOK ? "true" : "false");
                    break;
                case 29:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 1 %u", level);
                    tm_time.tm_hour = level;  // initialisieren
                    parity = level;           // Parity initialisieren
                    break;
                case 30:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 2 %u", level);
                    tm_time.tm_hour += 2 * level;
                    parity ^= level;
                    break;
                case 31:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 4 %u", level);
                    tm_time.tm_hour += 4 * level;
                    parity ^= level;
                    break;
                case 32:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 8 %u", level);
                    tm_time.tm_hour += 8 * level;
                    parity ^= level;
                    break;
                case 33:
                    ESP_LOGI(TAG, "Hour tens (BCD) bit 10 %u", level);
                    tm_time.tm_hour += 10 * level;
                    parity ^= level;
                    break;
                case 34:
                    ESP_LOGI(TAG, "Hour tens (BCD) bit 20 %u", level);
                    tm_time.tm_hour += 20 * level;
                    parity ^= level;
                    break;
                case 35:
                    ESP_LOGI(TAG, "Parity bit %u", level);
                    ESP_LOGI(TAG, "Hour: %d", tm_time.tm_hour);
                    hourOK = parity == level && tm_time.tm_hour < 24;
                    ESP_LOGI(TAG, "hourOK:%s", hourOK ? "true" : "false");
                    break;
                case 36:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 1 %u", level);
                    tm_time.tm_mday = level;  // initialize
                    parity = level;           // Parity initialize
                    break;
                case 37:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 2 %u", level);
                    tm_time.tm_mday += 2 * level;
                    parity ^= level;
                    break;
                case 38:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 4 %u", level);
                    tm_time.tm_mday += 4 * level;
                    parity ^= level;
                    break;
                case 39:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 8 %u", level);
                    tm_time.tm_mday += 8 * level;
                    parity ^= level;
                    break;
                case 40:
                    ESP_LOGI(TAG, "Day tens (BCD) bit 10 %u", level);
                    tm_time.tm_mday += 10 * level;
                    parity ^= level;
                    break;
                case 41:
                    ESP_LOGI(TAG, "Day tens (BCD) bit 20 %u", level);
                    tm_time.tm_mday += 20 * level;
                    parity ^= level;
                    break;
                case 42:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 1 %u", level);
                    tm_time.tm_wday = level;  // initialize
                    parity ^= level;
                    break;
                case 43:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 2 %u", level);
                    tm_time.tm_wday += 2 * level;
                    parity ^= level;
                    break;
                case 44:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 4 %u", level);
                    tm_time.tm_wday += 4 * level;
                    parity ^= level;
                    break;
                case 45:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 1 %u", level);
                    tm_time.tm_mon = level;  // initialize
                    parity ^= level;
                    break;
                case 46:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 2 %u", level);
                    tm_time.tm_mon += 2 * level;
                    parity ^= level;
                    break;
                case 47:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 4 %u", level);
                    tm_time.tm_mon += 4 * level;
                    parity ^= level;
                    break;
                case 48:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 8 %u", level);
                    tm_time.tm_mon += 8 * level;
                    parity ^= level;
                    break;
                case 49:
                    ESP_LOGI(TAG, "Month tens (BCD) bit 10 %u", level);
                    tm_time.tm_mon += 10 * level;
                    parity ^= level;
                    break;
                case 50:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 1 %u", level);
                    tm_time.tm_year = level;  // initialize
                    parity ^= level;
                    break;
                case 51:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 2 %u", level);
                    tm_time.tm_year += 2 * level;
                    parity ^= level;
                    break;
                case 52:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 4 %u", level);
                    tm_time.tm_year += 4 * level;
                    parity ^= level;
                    break;
                case 53:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 8 %u", level);
                    tm_time.tm_year += 8 * level;
                    parity ^= level;
                    break;
                case 54:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 10 %u", level);
                    tm_time.tm_year += 10 * level;
                    parity ^= level;
                    break;
                case 55:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 20 %u", level);
                    tm_time.tm_year += 20 * level;
                    parity ^= level;
                    break;
                case 56:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 40 %u", level);
                    tm_time.tm_year += 40 * level;
                    parity ^= level;
                    break;
                case 57:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 80 %u", level);
                    tm_time.tm_year += 80 * level;
                    parity ^= level;
                    break;
                case 58:
                    ESP_LOGI(TAG, "Paritybit %u", level);
                    ESP_LOGI(TAG, "Calendar: %02u.%02u.20%02u Weekday: %u", tm_time.tm_mday, tm_time.tm_mon,
                             tm_time.tm_year, tm_time.tm_wday);
                    ESP_LOGI(TAG, "DST: %s", tm_time.tm_isdst ? "Yes" : "No");
                    calenderOK = parity == level && tm_time.tm_year < 100 && tm_time.tm_year > 24 &&
                                 tm_time.tm_mon >= 1 && tm_time.tm_mon <= 12 && tm_time.tm_mday >= 1 &&
                                 tm_time.tm_mday <= 31 && tm_time.tm_wday >= 1 && tm_time.tm_wday <= 7;
                    ESP_LOGI(TAG, "calendarOK:%s", calenderOK ? "true" : "false");
                    tm_time.tm_year += 100;  // adjust year since 1900
                    tm_time.tm_mon -= 1;     // adjust month since January
                    tm_time.tm_sec = 0;
                    break;
                default:
                    ESP_LOGI(TAG, "second %u", second);
                    break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}
