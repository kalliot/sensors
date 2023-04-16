#ifndef __SENSORS__
#define __SENSORS__

enum meastype
{
    COUNT,
    TEMPERATURE,
    STATE
};

struct measurement {
    enum meastype id;
    int gpio;
    union {
        int count;
        bool state;
        float temperature;
    } data;
};

extern QueueHandle_t evt_queue;
extern char jsondata[256];
extern uint16_t sendcnt;
extern uint16_t sensorerrors;

#define BLINK_GPIO 2
#define COUNTER_GPIO 26
#define MIN_EPOCH 1650000000

#endif