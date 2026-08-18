/* SPI1 transport for the RA8876 display controller. */

#ifndef ELECTRA_SPI_H
#define ELECTRA_SPI_H

#include <stdint.h>

void    spi1_init(void);

void    ra_write_reg8(uint8_t reg, uint8_t value);
void    ra_write_reg16(uint8_t reg, uint16_t value);
void    ra_write_reg32(uint8_t reg, uint32_t value);
void    ra_cmd(uint8_t reg);
void    ra_data(uint8_t byte);
uint8_t ra_read_status(void);
uint8_t ra_read_data(void);
uint8_t ra_read_reg(uint8_t reg);

/* Single chip-select assertion, 0x80 prefix then the raw stream. ~2 bytes on the wire per
 * payload byte, so budget accordingly: a full frame is around 0.8 s. */
void    ra_write_bulk(const uint8_t *buf, uint32_t len);

#endif /* ELECTRA_SPI_H */
