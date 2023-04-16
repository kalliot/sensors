
#ifndef __FLASHMEM__
#define __FLASHMEM__


extern void flash_open(char *name);
extern uint16_t flash_read(char *name, uint16_t def);
extern void flash_write(char *name, uint16_t value);
extern void flash_commitchanges(void);

#endif
