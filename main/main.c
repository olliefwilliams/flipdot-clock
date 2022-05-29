#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "xtensa/core-macros.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "include/flipdot.h"
#include "include/fill.h"
#include "include/text.h"
#include "include/main.h"
#include "include/scroll.h"

#include <time.h>
#include <sys/time.h>
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_sntp.h"

// Flipdot stuff
static const char *TAG = "Main";

static bool button_held = false;

static dotboard_t dots;
// Mode state
static sys_mode_t mode = MODE_SCROLL;

// SNTP stuff

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
//#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
//#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static void obtain_time(void);
static void initialize_sntp(void);
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

void tick_task(void *pvParameter)
{

    int minuteCounter = 0;
    int flag = 0;
    while (1)
    {
        // printf("Hello world!\n");
        char custtime[64]; // no idea how long/big to make this; 64 is a guess

        char myHours[8];
        char myMinutes[8];

        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(custtime, sizeof(custtime), "%H:%M:%S", &timeinfo);
        strftime(myHours, sizeof(myHours), "%H", &timeinfo);
        strftime(myMinutes, sizeof(myMinutes), "%M", &timeinfo);

        // if seconds past minute is zero
        if (timeinfo.tm_sec == 0)
        {
            ESP_LOGI(TAG, "New minute!");
            minuteCounter++;
            char minuteShow[8];
            itoa(minuteCounter, minuteShow, 10);
            ESP_LOGI(TAG, "Minutes counted: %s", minuteShow);

            fill_off(&dots);
            render_text_3x5(&dots, 1, 5, myHours);
            render_text_3x5(&dots, 11, 5, myMinutes);
            dots[9][6] = 1; // colon top dot
            dots[9][8] = 1; // colon bottom dot
            write_dotboard(&dots, false);
        }

        ESP_LOGI(TAG, "Custom time in London is: %s", custtime);

        /*
        fill_off(&dots);
        if (flag == 0)
        {
            render_text_4x5(&dots, 2, 0, "TICK");
            flag = 1;
        }
        else
        {
            render_text_4x5(&dots, 2, 0, "TOCK");
            flag = 0;
        }
        write_dotboard(&dots, false);
       */
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

void app_main()
{
    ESP_LOGI(TAG, "Flipdot Display Controller System startup");

    // Initialise display
    flipdot_init();
    // A clean board to write after mode changes
    dotboard_t clean_board;
    fill_off(&clean_board);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    // tm_year is relative to 1900 on all posix systems (not 1970!)
    if (timeinfo.tm_year < (2019 - 1900))
    {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }

    // Set timezone to London Time
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();

    xTaskCreate(&tick_task, "tick_task", 2048, NULL, 5, NULL);
}

static void obtain_time(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi();
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_ERROR_CHECK(esp_wifi_stop());
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    sntp_init();
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}