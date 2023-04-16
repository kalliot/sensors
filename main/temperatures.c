#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"


#include "driver/gpio.h"
#include "ds18b20.h"
#include "mqtt_client.h"
#include "sensors.h"

static int tempSensorCnt;
static uint8_t *chipid;
static DeviceAddress tempSensors[8];
static char temperatureTopic[64];

static struct oneWireSensor {
    float prev;
    char sensorname[17];
    DeviceAddress *addr;
} *sensors;            


static int temp_getaddresses(DeviceAddress *tempSensorAddresses) {
	unsigned int numberFound = 0;
    
    reset_search();
    for (int i = 0; i < 8; i++)
    {
        gpio_set_level(BLINK_GPIO, true);
        printf("searching address %d ", numberFound);
        if (search(tempSensorAddresses[numberFound], true))
        {
            if (numberFound > 0 && !memcmp(tempSensorAddresses[numberFound-1],tempSensorAddresses[numberFound],8))
            {
                printf("duplicate address, rejecting\n");
            }
            else
            {
                printf("found\n");
                numberFound++;
            }
        }
        gpio_set_level(BLINK_GPIO, false);
        if (numberFound == 2)
        {
            return numberFound;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    printf("\n");
    return numberFound;
}


bool temperature_send(struct measurement *data, esp_mqtt_client_handle_t client)
{
    time_t now;
    
    time(&now);
    if (now < 1650000000) return false;

    gpio_set_level(BLINK_GPIO, true);

    static char *datafmt = "{\"dev\":\"%x%x%x\",\"sensor\":\"%s\",\"id\":\"temperature\",\"value\":%.02f,\"ts\":%jd,\"unit\":\"C\"}";
    sprintf(temperatureTopic,"%s%x%x%x/parameters/temperature/%s", CONFIG_CLIENTID_PREFIX, chipid[3], chipid[4], chipid[5], sensors[data->gpio].sensorname);

    sprintf(jsondata, datafmt,
                chipid[3],chipid[4],chipid[5],
                sensors[data->gpio].sensorname,
                data->data.temperature,
                now);
    esp_mqtt_client_publish(client, temperatureTopic, jsondata , 0, 0, 1);
    sendcnt++;
    gpio_set_level(BLINK_GPIO, false);
    return true;
}

#define DELAY_BETWEEN_SENSORS 1000

void getFirstTemperatures()
{
    float temperature;

    ds18b20_requestTemperatures();
    for (int i=0; i < tempSensorCnt; ) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        temperature = ds18b20_getTempC((DeviceAddress *) sensors[i].addr) + 1.0; 
        if (temperature < -10.0 || temperature > 85.0) {
            printf("%s failed with initial value %f, reading again\n", sensors[i].sensorname, temperature);
        }    
        else {
            sensors[i].prev = temperature;
            i++;
        }    
    }
}

static void temp_reader(void* arg)
{
    int delay = 10000 - (tempSensorCnt) * DELAY_BETWEEN_SENSORS;
    int retry=0;
    float temperature;
    
    for(;;) {
        ds18b20_requestTemperatures();
        for (int i=0; i < tempSensorCnt;) {
            vTaskDelay(DELAY_BETWEEN_SENSORS / portTICK_PERIOD_MS); 
            temperature = ds18b20_getTempC((DeviceAddress *) sensors[i].addr);
            float diff = fabs(sensors[i].prev - temperature);

            if (temperature < -10.0 || temperature > 85.0 || diff > 20.0)
            {
                sensorerrors++;
                if (++retry > 5)
                {
                    retry = 0;
                    i++; // next sensor
                }
            }
            else
            {
                if (diff >= 0.2)
                {
                    struct measurement meas;
                    meas.id = TEMPERATURE;
                    meas.gpio = i;
                    meas.data.temperature = temperature;
                    xQueueSend(evt_queue, &meas, 0);
                    sensors[i].prev = temperature;
                }
                i++;
            }
        }    
        vTaskDelay(delay / portTICK_PERIOD_MS);
    }
}

bool temperatures_init(int gpio, uint8_t *chip)
{
    char buff[3];

    chipid = chip;
    ds18b20_init(gpio);
    tempSensorCnt = temp_getaddresses(tempSensors);
    if (!tempSensorCnt) return false;

    sensors = malloc(sizeof(struct oneWireSensor) * tempSensorCnt);
    if (sensors == NULL) {
        printf("malloc failed when allocating sensors\n");
        return false;
    }
    printf("\nfound %d temperature sensors\n", tempSensorCnt);
    for (int i=0;i<tempSensorCnt;i++) {
        sensors[i].addr = &tempSensors[i]; 
        sensors[i].prev = 0.0;
        sensors[i].sensorname[0]='\0';
        for (int j = 0; j < 8; j++) {
            sprintf(buff,"%x",tempSensors[i][j]);
            strcat(sensors[i].sensorname, buff);
        }
        printf("sensorname %s done\n", sensors[i].sensorname);
    }
    getFirstTemperatures();
    if (tempSensorCnt)
    {
        xTaskCreate(temp_reader, "temperature reader", 2048, NULL, 10, NULL);
    }
    return true;
}