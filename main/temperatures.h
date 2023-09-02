#ifndef __TEMPERATURES__
#define __TEMPERATURES__

#include "sensors.h"

bool temperature_send(char *prefix, struct measurement *data, esp_mqtt_client_handle_t client);
bool temperatures_init(int gpio, uint8_t *chip);
char *temperatures_info();

#endif