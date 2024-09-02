#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "soc/frc_timer_reg.h"
#include "esp_log.h"
#include "homeapp.h"
#include "statereader.h"


static struct stateInstance
{
    int gpio;
    time_t prevStateTs;
    bool prevState;
    char topic[64];
    SemaphoreHandle_t xSemaphore;
} *instances;


static uint8_t *chipid;
static int instance_count;
static const char *TAG = "STATEREADER";

static void IRAM_ATTR gpio_isr_handler(void* arg)
{

    struct stateInstance *instance = (struct stateInstance *) arg;
    xSemaphoreGive(instance->xSemaphore);
}

static void state_reader(void *arg)
{
    struct stateInstance *instance = (struct stateInstance *) arg;

    while (1) {
        if (xSemaphoreTake(instance->xSemaphore, portMAX_DELAY)) {
            vTaskDelay(100 / portTICK_PERIOD_MS); // wait for all glitches
            bool state = (gpio_get_level(instance->gpio) == 0);
            if (state != instance->prevState) {
                struct measurement meas;
                meas.gpio = instance->gpio;
                meas.id = STATE;
                meas.data.state = state;
                xQueueSendFromISR(evt_queue, &meas, NULL);
                instance->prevState = state;
            }
        } 
    }
}


void stateread_init(uint8_t *chip, int amount)
{
    ESP_LOGI(TAG,"statereader init");
    chipid = chip;
    instance_count = amount;
    instances = malloc(sizeof(struct stateInstance) * amount);

    for (int i=0; i<amount; i++) {
        time(&instances[i].prevStateTs);
        instances[i].prevState = false;
        instances[i].xSemaphore = xSemaphoreCreateBinary();
    }
    //gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    ESP_LOGI(TAG,"statereader init done");
}


bool stateread_start(char *prefix, int index, int gpio)
{
    ESP_LOGI(TAG,"statereader start index %d, gpio %d",index, gpio);
    if (index < instance_count) {
        struct stateInstance *instance = &instances[index];

        instances[index].gpio = gpio;
        gpio_reset_pin(gpio);
        xTaskCreate(state_reader, "state reader", 2048, (void*) instance, 10, NULL);
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
        gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(gpio, gpio_isr_handler, (void*) instance);
        sprintf(instance->topic,"%s/sensors/%x%x%x/parameters/state/%d",
            prefix, chipid[3],chipid[4],chipid[5],gpio);
        ESP_LOGI(TAG,"statereader start done");
        return true;
    }
    ESP_LOGD(TAG,"too big instance number given, max is %d", instance_count -1);
    return false;
}


struct stateInstance * find_instance_by_gpio(int gpio)
{
    for (int i=0; i<instance_count;i++) {
        if (instances[i].gpio == gpio) return &instances[i];
    }
    return NULL;
}


void stateread_send(struct measurement *meas, esp_mqtt_client_handle_t client)
{
    time_t now;
    int duration;
    static char *datafmt = "{\"dev\":\"%x%x%x\",\"sensor\":%d,\"id\":\"state\",\"value\":%d,\"ts\":%jd,\"duration\":%d,\"unit\":\"bool\"}";
    
    struct stateInstance *inst = find_instance_by_gpio(meas->gpio);

    if (inst == NULL) {
        ESP_LOGD(TAG,"gpio %d not found from instances", meas->gpio);
        return;
    }
    gpio_set_level(BLINK_GPIO, true);
    time(&now);
    if (inst->prevStateTs > MIN_EPOCH) {
        duration = now - inst->prevStateTs;
    }
    else {
        duration = 0;
    }
    sprintf(jsondata, datafmt,
            chipid[3],chipid[4],chipid[5],
            meas->gpio,
            meas->data.state,
            now,
            duration);
    esp_mqtt_client_publish(client, inst->topic, jsondata , 0, 0, 1);
    inst->prevState = meas->data.state;
    inst->prevStateTs = now;
    gpio_set_level(BLINK_GPIO, false);
}
