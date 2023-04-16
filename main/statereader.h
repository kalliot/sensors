#ifndef __STATEREADER__
#define __STATEREADER__

#include "mqtt_client.h"
#include "sensors.h"

extern void stateread_init(uint8_t *chip, int amount);
extern bool stateread_start(int index, int gpio);
extern void stateread_send(struct measurement *meas, esp_mqtt_client_handle_t client);

#endif