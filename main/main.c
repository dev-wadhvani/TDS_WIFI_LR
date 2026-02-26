/*
 * Frequency + Flow + Adaptive TDS Monitor
 * Stable Input (Pulldown Enabled)
 * ESP-IDF v5.3
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"


// ================= CONFIG =================

#define WIFI_SSID "Anedya_2.4Ghz"
#define WIFI_PASS "Anedya@23!@#"

#define LAPTOP_IP "192.168.1.7"
#define UDP_PORT 3333

#define INPUT_PIN   GPIO_NUM_38
#define CONTROL_PIN GPIO_NUM_16
#define FLOW_PIN    GPIO_NUM_37

#define ALPHA 0.10f

#define SEND_INTERVAL_MS 1000
#define QUEUE_LENGTH 32

// Timing
#define SENSOR_ON_TIME_SEC       180
#define SENSOR_OFF_TIME_SEC        5
#define SENSOR_WARMUP_TIME_SEC    20

#define FLOW_THRESHOLD 0.400f

// =========================================


// Sensor States
typedef enum
{
    SENSOR_OFF = 0,
    SENSOR_WARMUP,
    SENSOR_MEASURE
} sensor_state_t;


static QueueHandle_t interval_queue;

static int udp_sock;
static struct sockaddr_in dest_addr;

static bool wifi_connected = false;

static volatile uint32_t flow_pulse_count = 0;


// ======================================================
// Adaptive TDS Model
// ======================================================

static inline float calculate_tds(float freq, float flow)
{
    float tds;

    if (flow < FLOW_THRESHOLD)
    {
        tds = (9e-7f * freq * freq)
            + (0.0683f * freq)
            + 3.5733f;
    }
    else
    {
        tds = (0.0901f * freq)
            + (501.9f * flow)
            - 266.8f;
    }

    if (tds < 0)
        tds = 0;

    return tds;
}


// ======================================================
// WIFI
// ======================================================

static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();

    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();

    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        wifi_connected = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}


static void wifi_init(void)
{
    nvs_flash_init();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL);

    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        }
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    esp_wifi_start();
}


// ======================================================
// UDP
// ======================================================

static void udp_init(void)
{
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(LAPTOP_IP);
}


// ======================================================
// ISR
// ======================================================

static void IRAM_ATTR freq_isr_handler(void *arg)
{
    static uint64_t last_time = 0;

    uint64_t now = esp_timer_get_time();

    if (last_time != 0)
    {
        uint32_t interval = (uint32_t)(now - last_time);

        BaseType_t hp = pdFALSE;
        xQueueSendFromISR(interval_queue, &interval, &hp);

        if (hp) portYIELD_FROM_ISR();
    }

    last_time = now;
}


static void IRAM_ATTR flow_isr_handler(void *arg)
{
    flow_pulse_count++;
}


// ======================================================
// TASK
// ======================================================

void frequency_task(void *pv)
{
    interval_queue = xQueueCreate(QUEUE_LENGTH,
                                  sizeof(uint32_t));

    // CONTROL PIN
    gpio_config_t ctrl = {
        .pin_bit_mask = (1ULL << CONTROL_PIN),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&ctrl);
    gpio_set_level(CONTROL_PIN, 0);


    // INPUT PIN WITH INTERNAL PULLDOWN
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << INPUT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE
    };
    gpio_config(&in);


    // FLOW PIN (leave unchanged for now)
    gpio_config_t flow = {
        .pin_bit_mask = (1ULL << FLOW_PIN),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE
    };
    gpio_config(&flow);


    gpio_install_isr_service(0);
    gpio_isr_handler_add(INPUT_PIN, freq_isr_handler, NULL);
    gpio_isr_handler_add(FLOW_PIN, flow_isr_handler, NULL);


    float raw_freq = 0;
    float filtered_freq = 0;
    float flow_rate_lmin = 0;
    float tds = 0;

    uint32_t interval;

    int64_t last_send = esp_timer_get_time();
    int64_t last_flow_calc = esp_timer_get_time();
    int64_t state_timer = esp_timer_get_time();

    sensor_state_t state = SENSOR_OFF;


    while (1)
    {
        int64_t now = esp_timer_get_time();


        // ================= STATE MACHINE =================

        switch (state)
        {
        case SENSOR_OFF:

            if (now - state_timer >=
                SENSOR_OFF_TIME_SEC * 1000000LL)
            {
                state = SENSOR_WARMUP;
                state_timer = now;

                gpio_set_level(CONTROL_PIN, 1);
            }
            break;


        case SENSOR_WARMUP:

            if (now - state_timer >=
                SENSOR_WARMUP_TIME_SEC * 1000000LL)
            {
                state = SENSOR_MEASURE;
                state_timer = now;
            }
            break;


        case SENSOR_MEASURE:

            if (now - state_timer >=
                (SENSOR_ON_TIME_SEC - SENSOR_WARMUP_TIME_SEC)
                * 1000000LL)
            {
                state = SENSOR_OFF;
                state_timer = now;

                gpio_set_level(CONTROL_PIN, 0);
            }
            break;
        }


        // ================= FREQUENCY =================

        if (state != SENSOR_OFF)
        {
            if (xQueueReceive(interval_queue,
                              &interval,
                              pdMS_TO_TICKS(50)))
            {
                raw_freq = 1000000.0f / interval;

                if (filtered_freq == 0)
                    filtered_freq = raw_freq;
                else
                    filtered_freq =
                        ALPHA * raw_freq +
                        (1 - ALPHA) * filtered_freq;
            }
        }


        // ================= FLOW =================

        if (now - last_flow_calc >= 1000000)
        {
            uint32_t pulses = flow_pulse_count;
            flow_pulse_count = 0;

            float flow_freq = pulses * 2.0f;
            flow_rate_lmin = flow_freq / 150.0f;

            last_flow_calc = now;
        }


        // ================= CSV OUTPUT =================

        if (now - last_send >= SEND_INTERVAL_MS * 1000)
        {
            char msg[128];

            if (state == SENSOR_OFF)
            {
                strcpy(msg, "-,-,-,-\n");
            }
            else if (state == SENSOR_WARMUP)
            {
                strcpy(msg, "warmup,warmup,warmup,warmup\n");
            }
            else
            {
                tds = calculate_tds(filtered_freq,
                                    flow_rate_lmin);

                snprintf(msg, sizeof(msg),
                         "%.2f,%.2f,%.3f,%.2f\n",
                         raw_freq,
                         filtered_freq,
                         flow_rate_lmin,
                         tds);
            }

            printf("%s", msg);

            if (wifi_connected)
            {
                sendto(udp_sock,
                       msg,
                       strlen(msg),
                       0,
                       (struct sockaddr *)&dest_addr,
                       sizeof(dest_addr));
            }

            last_send = now;
        }


        // Watchdog safety
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// ======================================================

void app_main(void)
{
    wifi_init();
    udp_init();

    xTaskCreate(frequency_task,
                "freq_task",
                4096,
                NULL,
                5,
                NULL);
}