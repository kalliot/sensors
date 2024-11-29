#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <stdlib.h>

#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"


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
#include "homeapp.h"
#include "temperature/temperatures.h"
#include "flashmem.h"
#include "ota/ota.h"
#include "device/device.h"
#include "statistics/statistics.h"
#include "mqtt_client.h"
#include "apwebserver/server.h"
#include "factoryreset.h"


#define TEMP_BUS 25
#define STATEINPUT_GPIO 33
#define STATEINPUT_GPIO2 32
#define STATISTICS_INTERVAL 1800
#define ESP_INTR_FLAG_DEFAULT 0



#if CONFIG_EXAMPLE_WIFI_SCAN_METHOD_FAST
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_FAST_SCAN
#elif CONFIG_EXAMPLE_WIFI_SCAN_METHOD_ALL_CHANNEL
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#endif

#if CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SIGNAL
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_EXAMPLE_WIFI_CONNECT_AP_BY_SECURITY
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#endif

#define WIFI_RECONNECT_RETRYCNT 50

#define HEALTHYFLAGS_WIFI 1
#define HEALTHYFLAGS_MQTT 2
#define HEALTHYFLAGS_TEMP 4
#define HEALTHYFLAGS_NTP  8

struct netinfo {
    char *ssid;
    char *password;
    char *mqtt_server;
    char *mqtt_port;
    char *mqtt_prefix;
};


// globals

struct netinfo *comminfo;
QueueHandle_t evt_queue = NULL;
char jsondata[512];

static const char *TAG = "SENSORSET";
static bool isConnected = false;
static uint8_t healthyflags = 0;

static char statisticsTopic[64];
static char readTopic[64];
static char otaUpdateTopic[64];
static int retry_num = 0;
static char *program_version = "";
static char appname[20];



static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid);
static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid);


static char *getJsonStr(cJSON *js, char *name)
{
    cJSON *item = cJSON_GetObjectItem(js, name);
    if (item != NULL)
    {
        if (cJSON_IsString(item))
        {
            return item->valuestring;
        }
        else ESP_LOGI(TAG, "%s is not a string", name);
    }
    else ESP_LOGI(TAG,"%s not found from json", name);
    return "\0";
}


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void sensorFriendlyName(cJSON *root)
{
    char *sensorname;
    char *friendlyname;

    sensorname   = getJsonStr(root, "sensor");
    friendlyname = getJsonStr(root, "name");
    if (temperature_set_friendlyname(sensorname, friendlyname))
    {
        ESP_LOGD(TAG, "writing sensor %s, friendlyname %s to flash",sensorname, friendlyname);
        flash_write_str(sensorname,friendlyname);
        flash_commitchanges();
    }
}


static bool handleJson(esp_mqtt_event_handle_t event)
{
    cJSON *root = cJSON_Parse(event->data);
    bool ret = false;
    char id[20];

    if (root != NULL)
    {
        strcpy(id,getJsonStr(root,"id"));
    }
    if (!strcmp(id,"otaupdate"))
    {
        char *fname = getJsonStr(root,"file");
        if (strlen(fname) > 5)
        {
            ota_start(fname);
        }
    }
    else if (!strcmp(id,"counter"))
    {
        uint16_t interval = cJSON_GetObjectItem(root,"interval")->valueint;
        counter_restart(interval);
        flash_write("interval", interval);
        flash_commitchanges();
        ret = true;
    }
    else if (!strcmp(id,"sensorfriendlyname"))
    {
        sensorFriendlyName(root);
        ret = true;
    }

    cJSON_Delete(root);
    return ret;
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
        ESP_LOGI(TAG,"subscribing topic %s", readTopic);
        msg_id = esp_mqtt_client_subscribe(client, readTopic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, otaUpdateTopic , 0);
        ESP_LOGI(TAG, "sent subscribe %s successful, msg_id=%d", otaUpdateTopic, msg_id);

        gpio_set_level(MQTTSTATUS_GPIO, true);
        isConnected = true;
        device_sendstatus(client, comminfo->mqtt_prefix, appname, (uint8_t *) handler_args);
        sendInfo(client, (uint8_t *) handler_args);
        sendSetup(client, (uint8_t *) handler_args);
        statistics_getptr()->connectcnt++;
        healthyflags |= HEALTHYFLAGS_MQTT;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        statistics_getptr()->disconnectcnt++;
        isConnected = false;
        gpio_set_level(MQTTSTATUS_GPIO, false);
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
        if (handleJson(event)) sendSetup(client, (uint8_t *) handler_args);
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


void sntp_callback(struct timeval *tv)
{
    (void) tv;
    static bool firstSyncDone = false;

    if (!firstSyncDone)
    {
        firstSyncDone = true;
        healthyflags |= HEALTHYFLAGS_NTP;
    }
}

static void sntp_start()
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    sntp_set_time_sync_notification_cb(sntp_callback);
}


int getWifiStrength(void)
{
    wifi_ap_record_t ap;

    if (!esp_wifi_sta_get_ap_info(&ap))
        return ap.rssi;
    return 0;
}


static void sendInfo(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char infoTopic[42];

    sprintf(infoTopic,"%s/sensors/%x%x%x/info",
         comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"info\",\"memfree\":%d,\"idfversion\":\"%s\",\"progversion\":%s}",
                chipid[3],chipid[4],chipid[5],
                esp_get_free_heap_size(),
                esp_get_idf_version(),
                program_version);
    esp_mqtt_client_publish(client, infoTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}


static void sendSetup(esp_mqtt_client_handle_t client, uint8_t *chipid)
{
    gpio_set_level(BLINK_GPIO, true);

    char setupTopic[42];
    sprintf(setupTopic,"%s/sensors/%x%x%x/setup",
         comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);

    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"setup\",\"interval\":%d }",
                chipid[3],chipid[4],chipid[5],
                counter_getinterval());
    esp_mqtt_client_publish(client, setupTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;

    sprintf(setupTopic,"%s/%s/%x%x%x/tempsensors",
        comminfo->mqtt_prefix, appname, chipid[3],chipid[4],chipid[5]);
    sprintf(jsondata, "{\"dev\":\"%x%x%x\",\"id\":\"tempsensors\",\"names\":[",
        chipid[3],chipid[4],chipid[5]);

    char sensorname[40];
    for (int i = 0; ; i++)
    {
        char *sensoraddr = temperature_getsensor(i);

        if (sensoraddr == NULL) break;
        sprintf(sensorname,"{\"addr\":\"%s\",\"name\":\"%s\"},",
            sensoraddr, temperature_get_friendlyname(i));
        strcat(jsondata,sensorname);
    }
    jsondata[strlen(jsondata)-1] = 0; // cut last comma
    strcat(jsondata,"]}");
    esp_mqtt_client_publish(client, setupTopic, jsondata , 0, 0, 1);
    statistics_getptr()->sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
}

static esp_mqtt_client_handle_t mqtt_app_start(uint8_t *chipid)
{
    char client_id[128];
    char uri[64];
    char deviceTopic[42];
    
    sprintf(client_id,"client_id=%s%x%x%x",
        comminfo->mqtt_prefix ,chipid[3],chipid[4],chipid[5]);
    sprintf(uri,"mqtt://%s:%s",comminfo->mqtt_server, comminfo->mqtt_port);

    ESP_LOGI(TAG,"built client id=[%s]",client_id);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = client_id,
        .session.last_will.topic = device_topic(comminfo->mqtt_prefix, deviceTopic, chipid),
        .session.last_will.msg = device_data(jsondata, chipid, appname, 0),
        .session.last_will.msg_len = strlen(jsondata),
        .session.last_will.qos = 0,
        .session.last_will.retain = 1
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, chipid);
    esp_mqtt_client_start(client);
    return client;
}

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,void *event_data)
{
    switch (event_id)
    {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG,"WIFI CONNECTING");
        break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG,"WiFi CONNECTED");
        break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG,"WiFi lost connection");
            gpio_set_level(WLANSTATUS_GPIO, false);
            if(retry_num < WIFI_RECONNECT_RETRYCNT)
            {
                esp_wifi_connect();
                retry_num++;
                ESP_LOGI(TAG,"Retrying to Connect");
            }
        break;

        case IP_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG,"Wifi got IP\n");
            gpio_set_level(WLANSTATUS_GPIO, true);
            retry_num = 0;
            healthyflags |= HEALTHYFLAGS_WIFI;
        break;
    }
}


void wifi_connect(char *ssid, char *password)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        }
    };
    strcpy((char*)wifi_configuration.sta.ssid, ssid);
    strcpy((char*)wifi_configuration.sta.password, password);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    esp_wifi_start();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_connect();
}


struct netinfo *get_networkinfo()
{
    static struct netinfo ni;
    char *default_ssid = "XXXXXXXX";

    ni.ssid = flash_read_str("ssid",default_ssid, 20);
    if (!strcmp(ni.ssid,"XXXXXXXX"))
        return NULL;

    ni.password    = flash_read_str("password","pass", 20);
    ni.mqtt_server = flash_read_str("mqtt_server","test.mosquitto.org", 20);
    ni.mqtt_port   = flash_read_str("mqtt_port","1883", 6);
    ni.mqtt_prefix = flash_read_str("mqtt_prefix","home/esp", 20);
    return &ni;
}


static void get_appname(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(appname,app_desc->project_name,20);
}


void app_main(void)
{
    uint8_t chipid[8];
    time_t now, prevStatsTs;
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

    gpio_reset_pin(BLINK_GPIO);
    gpio_reset_pin(WLANSTATUS_GPIO);
    gpio_reset_pin(SETUP_GPIO);
    gpio_reset_pin(MQTTSTATUS_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(WLANSTATUS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(SETUP_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(MQTTSTATUS_GPIO, GPIO_MODE_OUTPUT);

    get_appname();
    flash_open("storage");
    comminfo = get_networkinfo();
    if (comminfo == NULL)
    {
        gpio_set_level(SETUP_GPIO, true);
        server_init();
    }
    else
    {
        gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
        factoryreset_init();
        wifi_connect(comminfo->ssid, comminfo->password);
        evt_queue = xQueueCreate(10, sizeof(struct measurement));
        counter_init(comminfo->mqtt_prefix, chipid, flash_read("interval", 10));
        int sensorcnt = temperature_init(TEMP_BUS, appname, chipid, 8);
        if (sensorcnt)
        {
            char *sensoraddr;
            char *friendlyname;

            for (int i = 0; i < sensorcnt; i++)
            {
                sensoraddr   = temperature_getsensor(i);
                if (sensoraddr == NULL) break;
                friendlyname = flash_read_str(sensoraddr, sensoraddr, 20);
                if (strcmp(friendlyname, sensoraddr))
                {
                    if (!temperature_set_friendlyname(sensoraddr, friendlyname))
                    {
                        ESP_LOGD(TAG, "Set friendlyname for %s failed", sensoraddr);
                    }
                    free(friendlyname); // flash_read_str does dynamic allocation
                }
            }
        }

        esp_mqtt_client_handle_t client = mqtt_app_start(chipid);
        sntp_start();
        ESP_LOGI(TAG, "[APP] All init done, app_main, last line.");

        sprintf(statisticsTopic,"%s/sensors/%x%x%x/statistics",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);
        ESP_LOGI(TAG,"statisticsTopic=[%s]", statisticsTopic);

        sprintf(otaUpdateTopic,"%s/sensors/%x%x%x/otaupdate",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);

        sprintf(readTopic,"%s/sensors/%x%x%x/setsetup",
            comminfo->mqtt_prefix, chipid[3],chipid[4],chipid[5]);

        prevStatsTs = 0;
        program_version = ota_init(comminfo->mqtt_prefix, "sensors", chipid);
        stateread_init(chipid, 2);
        if (!statistics_init(comminfo->mqtt_prefix, appname, chipid))
        {
            ESP_LOGE(TAG,"failed in statistics init");
        }
        stateread_start(comminfo->mqtt_prefix, 0, STATEINPUT_GPIO);
        stateread_start(comminfo->mqtt_prefix, 1, STATEINPUT_GPIO2);

        ESP_LOGI(TAG, "gpios: mqtt=%d wlan=%d", MQTTSTATUS_GPIO, WLANSTATUS_GPIO);
        while (1)
        {
            struct measurement meas;

            time(&now);
            if ((now - statistics_getptr()->started > 20) &&
                (healthyflags == (HEALTHYFLAGS_WIFI | HEALTHYFLAGS_MQTT | HEALTHYFLAGS_NTP | HEALTHYFLAGS_TEMP)))
            {
                ota_cancel_rollback();
            }

            if (now > MIN_EPOCH)
            {
                if (statistics_getptr()->started < MIN_EPOCH)
                {
                    statistics_getptr()->started = now;
                }
                if (now - prevStatsTs >= STATISTICS_INTERVAL)
                {
                    if (isConnected)
                    {
                        statistics_send(client);
                        prevStatsTs = now;
                    }
                }
            }

            if(xQueueReceive(evt_queue, &meas, STATISTICS_INTERVAL * 1000 / portTICK_PERIOD_MS)) {
                time(&now);
                uint16_t qcnt = uxQueueMessagesWaiting(evt_queue);
                if (qcnt > statistics_getptr()->maxQElements)
                {
                    statistics_getptr()->maxQElements = qcnt;
                }

                switch (meas.id) {
                    case COUNT:
                        if (isConnected) counter_send(meas.data.count, client);
                    break;

                    case TEMPERATURE:
                        healthyflags |= HEALTHYFLAGS_TEMP;
                        if (isConnected) temperature_send(comminfo->mqtt_prefix, &meas, client);
                    break;

                    case STATE:
                        if (isConnected)
                        {
                            stateread_send(&meas, client);
                            statistics_getptr()->sendcnt++;
                        }
                    break;

                    case OTA:
                        ota_status_publish(&meas, client);
                    break;

                    default:
                        ESP_LOGD(TAG, "unknown data type" );
                }
            }
            else
            {   // timeout
                ESP_LOGI(TAG,"timeout");
            }
        }
    }
}
