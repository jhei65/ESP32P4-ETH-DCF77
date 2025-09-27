#include <stdio.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "driver/gpio.h"

#define DCF_VCC_GPIO 21  // GPIO-Pin für DCF77 VCC
#define DCF_PON_GPIO 23  // GPIO-Pin für DCF77 PON
#define DCF_TCO_GPIO 22  // GPIO-Pin für DCF77 TCO

// GPIO konfigurieren
gpio_config_t io_conf_vcc = {
    .pin_bit_mask = (1ULL << DCF_VCC_GPIO),  // Bitmaske für den Pin
    .mode = GPIO_MODE_OUTPUT,                // OUTPUT-Mode
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};

gpio_config_t io_conf_pon = {
    .pin_bit_mask = (1ULL << DCF_PON_GPIO),  // Bitmaske für den Pin
    .mode = GPIO_MODE_OUTPUT,                // OUTPUT-Mode
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};

// GPIO als Input konfigurieren
gpio_config_t io_conf_tco = {
    .pin_bit_mask = (1ULL << DCF_TCO_GPIO),  // Bitmaske für den Pin
    .mode = GPIO_MODE_INPUT,                 // INPUT-Mode
    .pull_up_en = GPIO_PULLUP_DISABLE,       // interner Pull-Up aktivieren (optional)
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_ANYEDGE,  // kein Interrupt, nur Abfrage
};

static volatile bool isr = false;
static const char* TAG = "DCF77";

// ISR (Interrupt Service Routine)
static void IRAM_ATTR gpio_isr_handler(void* arg) { isr = true; }

void dcf77(void* pvParameters) {
    gptimer_handle_t gptimer = NULL;
    uint64_t time_diff_high = 0;
    uint64_t time_diff_low = 0;
    uint64_t time_diff = 0;
    uint64_t time_diff2 = 0;
    uint8_t level = 0;
    bool new_data = false;

    gpio_config(&io_conf_vcc);
    gpio_set_level(DCF_VCC_GPIO, 1);

    gpio_config(&io_conf_pon);
    gpio_set_level(DCF_PON_GPIO, 0);

    gpio_config(&io_conf_tco);

    // ISR-Dienst config
    ESP_ERROR_CHECK(gpio_install_isr_service(0));  // Standardkonfiguration
    // ISR-Handler for this pin added
    ESP_ERROR_CHECK(gpio_isr_handler_add(DCF_TCO_GPIO, gpio_isr_handler, NULL));

    ESP_LOGI(TAG, "Initializing GPTimer...");

    // GPTimer config (1 MHz = 1 µs pro Tick)
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,  // 1 MHz
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    ESP_LOGI(TAG, "Waiting for GPIO interrupts ...");

    while (1) {
        if (isr) {
            isr = false;
            uint64_t now;
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
            static uint8_t minute = 0;
            static uint8_t pMinute = 0;  // parity Minute
            static bool minuteOK = false;
            static uint8_t hour = 0;
            static uint8_t pHour = 0;  // parity Hour
            static bool hourOK = false;
            static uint8_t calendarDay = 0;
            static uint8_t weekday = 0;
            static uint8_t month = 0;
            static uint8_t year = 0;
            static uint8_t pCalendar = 0;
            static bool calenderOK = false;
            static bool summertime = false;

            if (time_diff2 > 1600000 && time_diff2 < 2000000) { // detects the gap of 1.8s between two minutes
                if (calenderOK && minuteOK && hourOK && startTimeOK && second == 58) {
                    ESP_LOGI(TAG, "Valid time: %02u:%02u 20%02u-%02u-%02u Weekday: %u DST: %s", hour,
                             minute, year, month, calendarDay, weekday, summertime ? "Yes" : "No");
                    struct tm tm_time = {
                        .tm_year = year + 100,   // Years since 1900
                        .tm_mon = month - 1,     // Monaths since Januar (0–11) -> 8 = September
                        .tm_mday = calendarDay,  // Day of Months (1–31)
                        .tm_hour = hour,         // Hour (0–23)
                        .tm_min = minute,        // Minute (0–59)
                        .tm_sec = 0              // Second (0–60)
                    };

                    time_t t = mktime(&tm_time);  // converts struct tm in time_t (Unix-Timestamp)
                    if (t == -1) {
                        ESP_LOGE(TAG, "Error: time couldn't convert\n");
                    } else {
                        struct timeval now = {.tv_sec = t, .tv_usec = 0};
                        settimeofday(&now, NULL);  // Systemtime set
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
                case 0:
                    minuteOK = false;
                    hourOK = false;
                    calenderOK = false;
                    startTimeOK = false;
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
                    summertime = level == 1;
                    break;
                case 18:
                    ESP_LOGI(TAG, "'01': Following time in CET %u", level);
                    summertime = level == 0;
                    break;
                case 19:
                    ESP_LOGI(TAG, "'1': At the end of this hour a leap second is inserted; otherwise '0'.  %u",
                             level);
                    break;
                case 20:
                    ESP_LOGI(TAG, "'1': Start of time information %u", level);
                    startTimeOK = level == 1;
                    break;
                case 21:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 1 %u", level);
                    minute = level;   // initialisieren
                    pMinute = level;  // Parity initialisieren
                    break;
                case 22:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 2 %u", level);
                    minute += 2 * level;
                    pMinute += level;
                    break;
                case 23:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 4 %u", level);
                    minute += 4 * level;
                    pMinute += level;
                    break;
                case 24:
                    ESP_LOGI(TAG, "Minute ones (BCD) bit 8 %u", level);
                    minute += 8 * level;
                    pMinute += level;
                    break;
                case 25:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 10 %u", level);
                    minute += 10 * level;
                    pMinute += level;
                    break;
                case 26:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 20 %u", level);
                    minute += 20 * level;
                    pMinute += level;
                    break;
                case 27:
                    ESP_LOGI(TAG, "Minute tens (BCD) bit 40 %u", level);
                    minute += 40 * level;
                    pMinute += level;
                    break;
                case 28:
                    ESP_LOGI(TAG, "Parity bit %u", level);
                    ESP_LOGI(TAG, "Minute: %u", minute);
                    minuteOK = pMinute % 2 == level && minute < 60;
                    ESP_LOGI(TAG, "MinuteOK:%s", minuteOK ? "true" : "false");
                    break;
                case 29:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 1 %u", level);
                    hour = level;   // initialisieren
                    pHour = level;  // Parity initialisieren
                    break;
                case 30:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 2 %u", level);
                    hour += 2 * level;
                    pHour += level;
                    break;
                case 31:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 4 %u", level);
                    hour += 4 * level;
                    pHour += level;
                    break;
                case 32:
                    ESP_LOGI(TAG, "Hour ones (BCD) bit 8 %u", level);
                    hour += 8 * level;
                    pHour += level;
                    break;
                case 33:
                    ESP_LOGI(TAG, "Hour tens (BCD) bit 10 %u", level);
                    hour += 10 * level;
                    pHour += level;
                    break;
                case 34:
                    ESP_LOGI(TAG, "Hour tens (BCD) bit 20 %u", level);
                    hour += 20 * level;
                    pHour += level;
                    break;
                case 35:
                    ESP_LOGI(TAG, "Parity bit %u", level);
                    ESP_LOGI(TAG, "Hour: %u", hour);
                    hourOK = pHour % 2 == level && hour < 24;
                    ESP_LOGI(TAG, "hourOK:%s", hourOK ? "true" : "false");
                    break;
                case 36:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 1 %u", level);
                    calendarDay = level;  // initialize
                    pCalendar = level;    // Parity initialize
                    break;
                case 37:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 2 %u", level);
                    calendarDay += 2 * level;
                    pCalendar += level;
                    break;
                case 38:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 4 %u", level);
                    calendarDay += 4 * level;
                    pCalendar += level;
                    break;
                case 39:
                    ESP_LOGI(TAG, "Day ones (BCD) bit 8 %u", level);
                    calendarDay += 8 * level;
                    pCalendar += level;
                    break;
                case 40:
                    ESP_LOGI(TAG, "Day tens (BCD) bit 10 %u", level);
                    calendarDay += 10 * level;
                    pCalendar += level;
                    break;
                case 41:
                    ESP_LOGI(TAG, "Day tens (BCD) bit 20 %u", level);
                    calendarDay += 20 * level;
                    pCalendar += level;
                    break;
                case 42:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 1 %u", level);
                    weekday = level; // initialize
                    pCalendar += level;
                    break;
                case 43:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 2 %u", level);
                    weekday += 2 * level;
                    pCalendar += level;
                    break;
                case 44:
                    ESP_LOGI(TAG, "Weekday ones (BCD) bit 4 %u", level);
                    weekday += 4 * level;
                    pCalendar += level;
                    break;
                case 45:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 1 %u", level);
                    month = level;       // initialize
                    pCalendar += level;  // Parity initialize
                    break;
                case 46:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 2 %u", level);
                    month += 2 * level;
                    pCalendar += level;
                    break;
                case 47:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 4 %u", level);
                    month += 4 * level;
                    pCalendar += level;
                    break;
                case 48:
                    ESP_LOGI(TAG, "Month ones (BCD) bit 8 %u", level);
                    month += 8 * level;
                    pCalendar += level;
                    break;
                case 49:
                    ESP_LOGI(TAG, "Month tens (BCD) bit 10 %u", level);
                    month += 10 * level;
                    pCalendar += level;
                    break;
                case 50:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 1 %u", level);
                    year = level;        // initialize
                    pCalendar += level;  // Parity initialize
                    break;
                case 51:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 2 %u", level);
                    year += 2 * level;
                    pCalendar += level;
                    break;
                case 52:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 4 %u", level);
                    year += 4 * level;
                    pCalendar += level;
                    break;
                case 53:
                    ESP_LOGI(TAG, "Year ones (BCD) bit 8 %u", level);
                    year += 8 * level;
                    pCalendar += level;
                    break;
                case 54:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 10 %u", level);
                    year += 10 * level;
                    pCalendar += level;
                    break;
                case 55:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 20 %u", level);
                    year += 20 * level;
                    pCalendar += level;
                    break;
                case 56:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 40 %u", level);
                    year += 40 * level;
                    pCalendar += level;
                    break;
                case 57:
                    ESP_LOGI(TAG, "Year tens (BCD) bit 80 %u", level);
                    year += 80 * level;
                    pCalendar += level;
                    break;
                case 58:
                    ESP_LOGI(TAG, "Paritybit %u", level);
                    ESP_LOGI(TAG, "Calendar: %02u.%02u.20%02u Weekday: %u", calendarDay, month, year, weekday);
                    ESP_LOGI(TAG, "DST: %s", summertime ? "Yes" : "No");
                    calenderOK = pCalendar % 2 == level && year < 100 && year > 24 && month >= 1 && month <= 12 &&
                                 calendarDay >= 1 && calendarDay <= 31 && weekday >= 1 && weekday <= 7;
                    ESP_LOGI(TAG, "calendarOK:%s", calenderOK ? "true" : "false");
                    break;
                default:
                    ESP_LOGI(TAG, "second %u", second);
                    break;
            }
        } // if (new_data)
    }
}
