#ifndef SIM_HAL_H
#define SIM_HAL_H
#include <stdint.h>
void sim_reset(void);
uint16_t *sim_framebuffer(void);
uint16_t *sim_vram(void);
#endif
