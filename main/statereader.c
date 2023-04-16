#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "soc/frc_timer_reg.h"
#include "sensors.h"
#include "statereader.h"


#define ESP_INTR_FLAG_DEFAULT 0


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
    printf("statereader init ...\n");
    chipid = chip;
    instance_count = amount;
    instances = malloc(sizeof(struct stateInstance) * amount);

    for (int i=0; i<amount; i++) {
        time(&instances[i].prevStateTs);
        instances[i].prevState = false;
        instances[i].xSemaphore = xSemaphoreCreateBinary();
    }
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    printf("statereader init done\n");
}


bool stateread_start(int index, int gpio)
{
    printf("statereader start index %d, gpio %d\n",index, gpio);
    if (index < instance_count) {
        struct stateInstance *instance = &instances[index];

        instances[index].gpio = gpio;
        gpio_reset_pin(gpio);
        xTaskCreate(state_reader, "state reader", 2048, (void*) instance, 10, NULL);
        gpio_set_direction(gpio, GPIO_MODE_INPUT);
        gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(gpio, gpio_isr_handler, (void*) instance);
        sprintf(instance->topic,"%s%x%x%x/parameters/state/%d",
            CONFIG_CLIENTID_PREFIX,chipid[3],chipid[4],chipid[5],gpio);
        printf("statereader start done\n");
        return true;
    }
    printf("too big instance number given, max is %d\n", instance_count -1);
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
        printf("gpio %d not found from instances\n", meas->gpio);
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
    sendcnt++;
    inst->prevState = meas->data.state;
    inst->prevStateTs = now;
    gpio_set_level(BLINK_GPIO, false);
}
