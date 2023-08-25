#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sntp.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "esp_wifi_types.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "statereader.h"
#include "counter.h"
#include "sensors.h"
#include "temperatures.h"
#include "flashmem.h"
#include "mqtt_client.h"

#define TEMP_BUS 25
#define STATEINPUT_GPIO 33
#define STATEINPUT_GPIO2 32
#define STATISTICS_INTERVAL 1800
#define PROGRAM_VERSION 0.13

// globals
QueueHandle_t evt_queue = NULL;
char jsondata[256];
uint16_t sendcnt = 0;

static const char *TAG = "SENSORSET";
static uint16_t connectcnt = 0;
static uint16_t disconnectcnt = 0;
uint16_t sensorerrors = 0;

static char statisticsTopic[64];
static char readTopic[64];
static time_t started;
static uint16_t maxQElements = 0;


static void sendStatistics(esp_mqtt_client_handle_t client, uint8_t *chipid, time_t now);
static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid);
static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid);

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        printf("subscribing topic %s\n", readTopic);
        msg_id = esp_mqtt_client_subscribe(client, readTopic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        sendInfo(client, (uint8_t *) handler_args);
        // implement counter setup publish to counter module.
        sendSetup(client, (uint8_t *) handler_args);
        connectcnt++;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        disconnectcnt++;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        {
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            cJSON *root;
            root = cJSON_Parse(event->data);
            uint16_t interval = cJSON_GetObjectItem(root,"interval")->valueint;
            printf("interval=%d\n",interval);
            counter_restart(interval);
            flash_write("interval", interval);
            flash_commitchanges();
            cJSON_Delete(root);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}



static void sntp_start()
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}


int getWifiStrength(void)
{
    wifi_ap_record_t ap;

    if (!esp_wifi_sta_get_ap_info(&ap))
        return ap.rssi;
    return 0;
}


//{"dev":"277998","id":"statistics","connectcnt":6,"disconnectcnt":399,"sendcnt":20186,"sensorerrors":81,"ts":1679761328}

static void sendStatistics(esp_mqtt_client_handle_t client, uint8_t *chipid, time_t now)
{
    if (now < MIN_EPOCH || started < MIN_EPOCH) return;
    gpio_set_level(BLINK_GPIO, true);

    static char *datafmt = "{\"dev\":\"%x%x%x\",\"id\":\"statistics\",\"connectcnt\":%d,\"disconnectcnt\":%d,\"sendcnt\":%d,\"sensorerrors\":%d, \"max_queued\":%d,\"ts\":%jd,\"started\":%jd,\"rssi\":%d}";
    
    sprintf(jsondata, datafmt, 
                chipid[3],chipid[4],chipid[5],
                connectcnt,
                disconnectcnt,
                sendcnt,
                sensorerrors,
                maxQElements,
                now,
                started,
                getWifiStrength());
    esp_mqtt_client_publish(client, statisticsTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char infoTopic[32];

    sprintf(infoTopic,"%s%x%x%x/info",
         CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"info\",\"memfree\":%d,\"idfversion\":\"%s\",\"progversion\":%.2f, \"tempsensors\":[%s]}",
                chipid[3],chipid[4],chipid[5],
                esp_get_free_heap_size(),
                esp_get_idf_version(),
                PROGRAM_VERSION,
                temperatures_info());
    esp_mqtt_client_publish(client, infoTopic, jsondata , 0, 0, 1);
    sendcnt++;
    printf("sending info\n");
    gpio_set_level(BLINK_GPIO, false);
}

static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char setupTopic[32];
    sprintf(setupTopic,"%s%x%x%x/setup",
         CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5]);

    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"setup\",\"interval\":%d }",
                chipid[3],chipid[4],chipid[5],
                counter_getinterval());
    esp_mqtt_client_publish(client, setupTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

static esp_mqtt_client_handle_t mqtt_app_start(uint8_t *chipid)
{
    char client_id[128];
    
    sprintf(client_id,"client_id=%s%x%x%x",
        CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5]);

    printf("built client id=[%s]",client_id);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .credentials.client_id = client_id
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, chipid);
    esp_mqtt_client_start(client);
    return client;
}


void app_main(void)
{
    uint8_t chipid[8];
    time_t now, prevStatsTs;
    //int tempSensorCnt;
    esp_efuse_mac_get_default(chipid);

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    flash_open("storage");
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    evt_queue = xQueueCreate(10, sizeof(struct measurement));
    counter_init(chipid, flash_read("interval", 10));
    temperatures_init(TEMP_BUS, chipid);

    esp_mqtt_client_handle_t client = mqtt_app_start(chipid);
    sntp_start();
    ESP_LOGI(TAG, "[APP] All init done, app_main, last line.");

    sprintf(statisticsTopic,"%s%x%x%x/statistics",
         CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5]);
    printf("statisticsTopic=[%s]\n", statisticsTopic);

    sprintf(readTopic,"%s%x%x%x/setsetup",
         CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5]);

    //sendInfo(client, chipid);
    stateread_init(chipid, 2);
    stateread_start(0, STATEINPUT_GPIO);
    stateread_start(1, STATEINPUT_GPIO2);
    // it is very propable, we will not get correct timestamp here.
    // It takes some time to get correct timestamp from ntp.
    time(&started);
    prevStatsTs = now = started;

    sendStatistics(client, chipid, now);

    while (1)
    {
        struct measurement meas;
        // send statistics after 4 hours, if nothing happens.
        // this is typical if we have only slow changing state sensor

        if(xQueueReceive(evt_queue, &meas, STATISTICS_INTERVAL * 1000 / portTICK_PERIOD_MS)) {
            time(&now);
            uint16_t qcnt = uxQueueMessagesWaiting(evt_queue);
            if (started < MIN_EPOCH)
            {
                prevStatsTs = started = now;
                sendStatistics(client, chipid , now);
            }
            if (qcnt > maxQElements)
            {
                maxQElements = qcnt;
            }
            if (now - prevStatsTs >= STATISTICS_INTERVAL) {
                sendStatistics(client, chipid, now);
                prevStatsTs = now;
            }
            switch (meas.id) {
                case COUNT:
                    counter_send(meas.data.count, client);
                break;

                case TEMPERATURE:
                    temperature_send(&meas, client);
                break;

                case STATE:
                    stateread_send(&meas, client);
                break;

                default:
                    printf("unknown data type\n" );
            }
        }
        else
        {   // timeout
            printf("timeout\n");
            time(&now);
            sendStatistics(client, chipid, now);
            prevStatsTs = now;
        }
    }
}
