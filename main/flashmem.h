
#ifndef __FLASHMEM__
#define __FLASHMEM__

#include "nvs_flash.h"

extern nvs_handle flash_open(char *name);
extern void flash_erase_all(nvs_handle nvsh);
extern uint16_t flash_read(nvs_handle nvsh, char *name, uint16_t def);
extern void flash_write(nvs_handle nvsh, char *name, uint16_t value);
extern uint32_t flash_read32(nvs_handle nvsh, char *name, uint32_t def);
extern void flash_write32(nvs_handle nvsh, char *name, uint32_t value);
extern char *flash_read_str(nvs_handle nvsh, char *name, char *def, int len);
extern void flash_write_str(nvs_handle nvsh, char *name, char *value);
extern float flash_read_float(nvs_handle nvsh, char *name, float def);
extern void flash_write_float(nvs_handle nvsh, char *name, float value);
extern void flash_commitchanges(nvs_handle nvsh);

#endif
