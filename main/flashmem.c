#include <stdio.h>
#include "esp_log.h"
#include "flashmem.h"


static const char *TAG = "FLASHMEM";


nvs_handle flash_open(char *name)
{
    esp_err_t err;
    nvs_handle nvsh;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err) ESP_LOGE(TAG,"Nvs_flash_init returned %d", err);

    ESP_LOGI(TAG,"Opening Non-Volatile Storage (NVS) handle... ");
    err = nvs_open(name, NVS_READWRITE, &nvsh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,"Error (%d) opening NVS handle!", err);
        return -1;
    } else {
        ESP_LOGI(TAG,"Done");
    }    
    return nvsh;
}


void flash_erase_all(nvs_handle nvsh)
{
    esp_err_t err;

    err = nvs_erase_all(nvsh);
    if (err != ESP_OK) ESP_LOGD(TAG,"flash erase failed");
}

char *flash_read_str(nvs_handle nvsh, char *name, char *def, int len)
{
    esp_err_t err;
    unsigned int readlen = len;
    char *ret;

    ESP_LOGI(TAG,"Reading %s from NVS", name);
    ret = (char *) malloc(len);
    err = nvs_get_str(nvsh, name , ret, &readlen);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG,"%s = %s", name, ret);
        break;

        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG,"%s is not initialized yet!");
            free(ret);
            ret = def;
        break;

        default :
            ESP_LOGI(TAG,"Error (%d) reading!\n", err);
            free(ret);
            ret = def;
    }
    return ret;
}

void flash_write_str(nvs_handle nvsh, char *name, char *value)
{
    esp_err_t err;

    err = nvs_set_str(nvsh, name, value);
    if (err != ESP_OK) ESP_LOGD(TAG,"Updating %s in NVS failed", name);
}


uint16_t flash_read(nvs_handle nvsh, char *name, uint16_t def)
{
    esp_err_t err;
    uint16_t ret;

    ESP_LOGD(TAG,"Reading %s from NVS", name);
    err = nvs_get_u16(nvsh, name , &ret);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG,"%s = %d", name, ret);
        break;

        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG,"%s is not initialized yet!", name);
            ret = def;
        break;

        default :
            ESP_LOGD(TAG,"Error (%d) reading!", err);
            ret = def;
    }
    return ret;
}

void flash_write(nvs_handle nvsh, char *name, uint16_t value)
{
    esp_err_t err;

    err = nvs_set_u16(nvsh, name, value);
    if (err != ESP_OK) ESP_LOGD(TAG,"failed to write %s",name);
}

uint32_t flash_read32(nvs_handle nvsh, char *name, uint32_t def)
{
    esp_err_t err;
    uint32_t ret;

    ESP_LOGD(TAG,"Reading %s from NVS", name);
    err = nvs_get_u32(nvsh, name , &ret);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG,"%s = %d", name, ret);
        break;

        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG,"%s is not initialized yet!", name);
            ret = def;
        break;

        default :
            ESP_LOGD(TAG,"Error (%d) reading!", err);
            ret = def;
    }
    return ret;
}

void flash_write32(nvs_handle nvsh, char *name, uint32_t value)
{
    esp_err_t err;

    err = nvs_set_u32(nvsh, name, value);
    if (err != ESP_OK) ESP_LOGD(TAG,"failed to write %s",name);
}

float flash_read_float(nvs_handle nvsh, char *name, float def)
{
    esp_err_t err;
    float ret;
    uint32_t readval;

    err = nvs_get_u32(nvsh, name , &readval);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG, "reading %s from flash, value = %d", name, readval);
            ret = (float) readval / 100.0;
        break;

        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG, "%s is not initialized yet!", name);
            ret = def;
        break;

        default :
            ESP_LOGI(TAG, "Error (%d) when reading %s!", err, name);
            ret = def;
    }
    return ret;
}


void flash_write_float(nvs_handle nvsh, char *name, float value)
{
    esp_err_t err;
    uint32_t writevalue;

    writevalue = value * 100;
    ESP_LOGI(TAG,"Updating %s in NVS value = %d ", name, writevalue);
    err = nvs_set_u32(nvsh, name, writevalue);
    ESP_LOGI(TAG,"%s", (err != ESP_OK) ? "Failed!" : "Done");
}

void flash_commitchanges(nvs_handle nvsh)
{
    esp_err_t err;

    err = nvs_commit(nvsh);
    if (err != ESP_OK) ESP_LOGD(TAG,"commit failed.");
}
