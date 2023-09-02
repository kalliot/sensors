
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "flashmem.h"
#include "factoryreset.h"

static int reset_gpio = 23;
static SemaphoreHandle_t xSemaphore;

static void IRAM_ATTR gpio_isr_handler()
{
    xSemaphoreGive(xSemaphore);
}

static void reset_reader(void *arg)
{
    while (1) {
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY)) {
            vTaskDelay(100 / portTICK_PERIOD_MS); // wait for all glitches
            if (gpio_get_level(reset_gpio) == 0) {
                flash_erase_all();
                flash_commitchanges();
                printf("**** restarting ****\n");
                vTaskDelay(200 / portTICK_PERIOD_MS); 
                esp_restart();
            }
        } 
    }
}


void factoryreset_init()
{
    printf("factoryreset init ...\n");
    
    xSemaphore = xSemaphoreCreateBinary();
    gpio_reset_pin(reset_gpio);
    xTaskCreate(reset_reader, "reset reader", 2048, NULL, 10, NULL);
    gpio_set_direction(reset_gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(reset_gpio, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(reset_gpio, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(reset_gpio, gpio_isr_handler, NULL);

    printf("factoryreset init done\n");
}
