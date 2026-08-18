#ifndef ELECTRA_RIIC_H
#define ELECTRA_RIIC_H

#include <stdint.h>

/* Channel numbers are the hardware's: 0 = pot-touch sensor bus, 2 = touchscreen bus. */
#define RIIC_CH_TOUCHKEY   0
#define RIIC_CH_TOUCHSCR   2

/* Return codes. Negative is failure; every one of them is distinguishable, because "I2C did
 * not work" is exactly the sort of diagnosis that wastes days. */
#define RIIC_OK             0
#define RIIC_ERR_BUSY      -1   /* bus never went idle — stuck slave holding SDA/SCL low */
#define RIIC_ERR_NACK      -2   /* addressed device did not answer */
#define RIIC_ERR_TIMEOUT   -3   /* a status flag never arrived */
#define RIIC_ERR_ARG       -4

/* Which wait a failed transfer died on. There are eight places a transfer can stall and they
 * have completely different causes; a bare -3 says none of them. */
extern volatile uint8_t g_riic_stage;
extern volatile uint8_t g_riic_fail;   /* stage at the moment of failure, before recovery */
extern volatile uint8_t g_riic_icsr2, g_riic_iccr2, g_riic_icmr3;  /* captured at failure */
#define RIIC_ST_BBSY      1
#define RIIC_ST_ADDR_TDRE 2
#define RIIC_ST_ADDR_ACK  3
#define RIIC_ST_TX_TDRE   4
#define RIIC_ST_TX_TEND   5
#define RIIC_ST_RS_TDRE   6
#define RIIC_ST_RS_ACK    7
#define RIIC_ST_RX_RDRF   8
#define RIIC_ST_RX_STOP   9
#define RIIC_ST_STOP     10

void riic_init(unsigned ch);

/* Address-only transaction: START, address, STOP. Used for bus scanning — an ACK proves a
 * device is present without assuming anything about its register map. */
int riic_probe(unsigned ch, uint8_t addr7);

int riic_write(unsigned ch, uint8_t addr7, const uint8_t *buf, uint32_t len);
int riic_read(unsigned ch, uint8_t addr7, uint8_t *buf, uint32_t len);

/* Write then repeated-START then read: the register-pointer idiom both devices use. */
int riic_write_read(unsigned ch, uint8_t addr7,
                    const uint8_t *wbuf, uint32_t wlen,
                    uint8_t *rbuf, uint32_t rlen);

/* Convenience for the common single-register-pointer case. */
int riic_read_regs(unsigned ch, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len);
int riic_write_reg(unsigned ch, uint8_t addr7, uint8_t reg, uint8_t value);

/* Recover a bus a slave is holding low, by clocking SCL until SDA releases. Costs nothing
 * when the bus is healthy and is the difference between a wedged bus and a working one after
 * a reset that interrupted a transfer mid-byte. */
void riic_bus_recover(unsigned ch);

#endif /* ELECTRA_RIIC_H */
