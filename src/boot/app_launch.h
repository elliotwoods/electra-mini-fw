#ifndef ELECTRA_APP_LAUNCH_H
#define ELECTRA_APP_LAUNCH_H

/* Application region, must match app.ld. */
#define APP_BASE_ADDR   0x00120000UL
#define APP_MAX_BYTES   (2944UL * 1024UL)

/* 1 if the image at APP_BASE_ADDR looks like a launchable Cortex-M image:
 * plausible initial SP in SRAM, Thumb-bit reset vector inside the region. */
int  app_image_valid(void);

/* Quiesces interrupts, points VTOR at the application and branches. Never returns. */
void app_launch(void);

/* Warm reset. The boot handshake in SRAM survives it. */
void bsp_system_reset(void);

#endif /* ELECTRA_APP_LAUNCH_H */
