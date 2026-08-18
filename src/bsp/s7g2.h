/* Renesas Synergy S7G2 register definitions — only what this firmware touches.
 *
 * Every address here was recovered from the stock Electra One Mini firmware v4.1.4 by
 * analysis, not from a datasheet we do not have. Where a register's *name* is inferred
 * rather than confirmed, it is marked. The addresses and the values written are certain: they
 * are what the shipping firmware does, and that firmware demonstrably boots this board.
 *
 * See docs/hardware-notes.md for the provenance of each value.
 */

#ifndef ELECTRA_S7G2_H
#define ELECTRA_S7G2_H

#include <stdint.h>

#define REG8(a)  (*(volatile uint8_t  *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))

/* ---------------------------------------------------------------- Cortex-M core */
#define CPACR           0xE000ED88UL   /* FPU access control */
#define SCB_VTOR        0xE000ED08UL   /* the bootloader sets this; we never write it */
#define SCB_ICSR        0xE000ED04UL   /* interrupt control/state */
#define SCB_AIRCR       0xE000ED0CUL   /* app interrupt/reset control; SYSRESETREQ */
#define ICSR_PENDSTCLR  (1UL << 25)    /* clear a latched SysTick */
#define ICSR_PENDSVCLR  (1UL << 27)    /* clear a latched PendSV */
#define SYST_CSR        0xE000E010UL   /* SysTick control/status */
#define SYST_RVR        0xE000E014UL   /* SysTick reload */
#define SYST_CVR        0xE000E018UL   /* SysTick current value */

/* ---------------------------------------------------------------- protection */
/* PRCR gates most clock/LPM registers. Group 0 = clocks, group 1 = operating power mode.
 * Unlock writes 0xA501 / 0xA502; lock writes 0xA500. Reference-counted in prcr.c. */
#define PRCR            0x4001E3FEUL
#define PRCR_KEY        0xA500U
#define PRCR_GROUP0     0x0001U
#define PRCR_GROUP1     0x0002U

/* ---------------------------------------------------------------- clock generation */
#define SCKDIVCR        0x4001E020UL   /* system clock dividers, single 32-bit store */
#define SCKDIVCR2       0x4001E024UL   /* UCK[6:4] — USB clock divider */
#define SCKSCR          0x4001E026UL   /* system clock source select */
#define PLLCCR          0x4001E028UL   /* 16-bit: PLLMUL[13:8], PLLSRCSEL[4], PLIDIV[1:0] */
#define PLLCR           0x4001E02AUL   /* bit0 PLLSTP */
#define BCKCR           0x4001E030UL   /* BCLK pin divider */
#define MOSCCR          0x4001E032UL   /* bit0 MOSTP — main oscillator stop */
#define HOCOCR          0x4001E036UL   /* bit0 HCSTP */
#define OSCSF           0x4001E03CUL   /* stabilisation flags: b0 HOCO, b3 MOSC, b5 PLL */
#define EBCKOCR         0x4001E052UL   /* external bus clock out enable */
#define SDCKOCR         0x4001E053UL   /* SDRAM clock out enable */
#define OPCCR           0x4001E0A0UL   /* operating power control */
#define MOSCWTCR        0x4001E0A2UL   /* main oscillator wait */
#define HOCOWTCR        0x4001E0A5UL   /* HOCO wait */
#define SOPCCR          0x4001E0AAUL   /* sub-osc power control */
#define MOMCR           0x4001E413UL   /* main osc mode: MODRV[5:4], MOSEL[6] */
#define SOSCCR          0x4001E480UL   /* sub-clock osc stop */
#define SOMCR           0x4001E481UL   /* sub-clock osc mode */

#define OSCSF_HOCOSF    (1U << 0)
#define OSCSF_MOSCSF    (1U << 3)
#define OSCSF_PLLSF     (1U << 5)

/* ---------------------------------------------------------------- flash / SRAM timing */
#define FCACHEE         0x4001C100UL   /* flash cache enable */
#define FCACHEIV        0x4001C104UL   /* flash cache invalidate */
#define FLWT            0x4001C11CUL   /* flash wait cycles [2:0] */
#define SRAMPRCR        0x40002004UL   /* SRAM wait-state protect: 0xF1 unlock, 0xF0 lock */
#define SRAMWTSC        0x40002008UL   /* SRAM wait states */

/* ---------------------------------------------------------------- external bus / SDRAM */
#define SDCCR           0x40003C00UL   /* bit0 EXENB, BSIZE[5:4] */
#define SDCMOD          0x40003C01UL   /* bit0 EMODE */
#define SDAMOD          0x40003C02UL   /* bit0 BE */
#define SDRFCR          0x40003C14UL   /* REFW[15:12], RFC[11:0] */
#define SDRFEN          0x40003C16UL   /* bit0 RFEN */
#define SDICR           0x40003C20UL   /* bit0 INIRQ */
#define SDIR            0x40003C24UL   /* ARFI[10:8], ARFC[7:4], PRC[3:0] */
#define SDADR           0x40003C40UL   /* MXC[1:0] */
#define SDTR            0x40003C44UL   /* timing */
#define SDMOD           0x40003C48UL   /* SDRAM mode register image */
#define SDSR            0x40003C50UL   /* status */

#define SDRAM_BASE      0x90000000UL
#define SDRAM_SIZE      (32UL * 1024 * 1024)

/* ---------------------------------------------------------------- module stop */
/* Register pointer table recovered at 0x0031DA40. Bit numbering per peripheral is in the
 * lookup table at 0x0031D9F8; the ones we need are spelled out below. */
#define MSTPCRA         0x4001E01CUL
#define MSTPCRB         0x40047000UL
#define MSTPCRC         0x40047004UL
#define MSTPCRD         0x40047008UL

#define MSTPB_QSPI      (1UL << 6)
#define MSTPB_RIIC2     (1UL << 7)
#define MSTPB_RIIC1     (1UL << 8)
#define MSTPB_RIIC0     (1UL << 9)
#define MSTPB_USBFS     (1UL << 11)
#define MSTPB_USBHS     (1UL << 12)
#define MSTPB_SPI1      (1UL << 18)
#define MSTPB_SPI0      (1UL << 19)
#define MSTPC_DRW       (1UL << 6)
#define MSTPC_SDHI0     (1UL << 12)
#define MSTPD_S12AD1    (1UL << 15)
#define MSTPD_S12AD0    (1UL << 16)

/* ------------------------------------------------------------------ RIIC
 *
 * Two buses are populated: RIIC0 carries the AT42QT2120 pot-touch sensor at 0x1C
 * (P400 SCL / P401 SDA) and RIIC2 the FT5x06 touchscreen at 0x38 (P511 SDA / P512 SCL).
 * Both run at 400 kHz. Channels are 0x100 apart. */
#define RIIC_BASE(ch)   (0x40053000UL + ((uint32_t)(ch) * 0x100UL))

#define RIIC_ICCR1(ch)  (RIIC_BASE(ch) + 0x00UL)
#define RIIC_ICCR2(ch)  (RIIC_BASE(ch) + 0x01UL)
#define RIIC_ICMR1(ch)  (RIIC_BASE(ch) + 0x02UL)
#define RIIC_ICMR2(ch)  (RIIC_BASE(ch) + 0x03UL)
#define RIIC_ICMR3(ch)  (RIIC_BASE(ch) + 0x04UL)
#define RIIC_ICFER(ch)  (RIIC_BASE(ch) + 0x05UL)
#define RIIC_ICSER(ch)  (RIIC_BASE(ch) + 0x06UL)
#define RIIC_ICIER(ch)  (RIIC_BASE(ch) + 0x07UL)
#define RIIC_ICSR1(ch)  (RIIC_BASE(ch) + 0x08UL)
#define RIIC_ICSR2(ch)  (RIIC_BASE(ch) + 0x09UL)
#define RIIC_ICBRL(ch)  (RIIC_BASE(ch) + 0x10UL)
#define RIIC_ICBRH(ch)  (RIIC_BASE(ch) + 0x11UL)
#define RIIC_ICDRT(ch)  (RIIC_BASE(ch) + 0x12UL)
#define RIIC_ICDRR(ch)  (RIIC_BASE(ch) + 0x13UL)

#define ICCR1_ICE       0x80U
#define ICCR1_IICRST    0x40U
#define ICCR1_SOWP      0x10U
#define ICCR1_SCLI      0x02U
#define ICCR1_SDAI      0x01U

#define ICCR2_BBSY      0x80U
#define ICCR2_MST       0x40U
#define ICCR2_TRS       0x20U
#define ICCR2_SP        0x08U
#define ICCR2_RS        0x04U
#define ICCR2_ST        0x02U

#define ICMR3_WAIT      0x40U
#define ICMR3_RDRFS     0x20U
#define ICMR3_ACKWP     0x10U
#define ICMR3_ACKBT     0x08U

#define ICSR2_TDRE      0x80U
#define ICSR2_TEND      0x40U
#define ICSR2_RDRF      0x20U
#define ICSR2_NACKF     0x10U
#define ICSR2_STOP      0x08U
#define ICSR2_START     0x04U
#define ICSR2_AL        0x02U
#define ICSR2_TMOF      0x01U

/* ---------------------------------------------------------------- ICU */
/* On this part the NVIC alone routes nothing: every interrupt must also have its event
 * number written into an IELSR slot. Forgetting this fails silently. */
#define ICU_NMIER       0x40006120UL
#define ICU_IELSR       0x40006300UL   /* IELSR[i] at IELSR + 4*i */
#define ICU_IELSR_COUNT 25

/* ---------------------------------------------------------------- MSP monitor */
/* Renesas main-stack-pointer monitor — NOT the ARM MPU. Raises NMI on stack overrun. */
#define MSPMPUOAD       0x40000D00UL   /* key 0xA500 + operation-after-detection */
#define MSPMPUCTL       0x40000D04UL   /* bit0 enable */
#define MSPMPUSA        0x40000D08UL   /* region start */
#define MSPMPUEA        0x40000D0CUL   /* region end */
#define NMIER_SPEEN     (1UL << 12)

/* ---------------------------------------------------------------- IOPORT / PFS */
/* PmnPFS is a 32-bit register per pin: PFS_BASE + port*0x40 + pin*4.
 *
 * Conveniently, SSP's ioport_cfg_options_t bitfield IS the PmnPFS layout, so the recovered
 * 100-entry table in electra_pin_cfg.c can be written straight through with no translation:
 *   bit0 PODR, bit2 PDR, bit4 PCR(pull-up), bit6 NCODR, bits[11:10] DSCR,
 *   bits[13:12] EOFR, bit14 ISEL(IRQ), bit15 ASEL(analog), bit16 PMR(peripheral),
 *   bits[28:24] PSEL.
 * Verified against the decoded table: 0x0B010C00 = PSEL 0xB | PMR | DSCR high-drive. */
#define PFS_BASE        0x40040800UL
#define PWPR            0x40040D03UL   /* bit7 B0WI, bit6 PFSWE */
#define PFS(port, pin)  (PFS_BASE + ((uint32_t)(port) * 0x40UL) + ((uint32_t)(pin) * 4UL))

/* PmnPFS fields, named. The recovered pin table is written through verbatim, but pins the
 * table does not declare — the display reset line among them — have to be built by hand. */
#define PFS_PODR        (1UL << 0)     /* output data */
#define PFS_PDR         (1UL << 2)     /* 1 = output */
#define PFS_PCR         (1UL << 4)     /* pull-up */
#define PFS_ISEL        (1UL << 14)    /* IRQ input */
#define PFS_ASEL        (1UL << 15)    /* analog */
#define PFS_PMR         (1UL << 16)    /* peripheral, not GPIO */

/* PORT registers, used for plain GPIO once pins are muxed. PCNTR3 carries the atomic
 * set/reset pair: POSR in the low halfword, PORR in the high halfword. */
#define PORT_BASE       0x40040000UL
#define PORT_PCNTR1(p)  (PORT_BASE + ((uint32_t)(p) * 0x20UL) + 0x00UL)
#define PORT_PCNTR2(p)  (PORT_BASE + ((uint32_t)(p) * 0x20UL) + 0x04UL)
#define PORT_PCNTR3(p)  (PORT_BASE + ((uint32_t)(p) * 0x20UL) + 0x08UL)
#define PORT_SET(p, b)  (REG32(PORT_PCNTR3(p)) = (1UL << (b)))            /* POSR */
#define PORT_CLR(p, b)  (REG32(PORT_PCNTR3(p)) = (1UL << ((b) + 16)))     /* PORR */
#define PORT_GET(p, b)  ((REG32(PORT_PCNTR2(p)) >> (b)) & 1U)             /* PIDR */

/* ---------------------------------------------------------------- SPI1 (display) */
#define SPI1_BASE       0x40072100UL
#define SPI1_SPCR       (SPI1_BASE + 0x00UL)
#define SPI1_SPSR       (SPI1_BASE + 0x03UL)
#define SPI1_SPDR       (SPI1_BASE + 0x04UL)
#define SPI1_SPSCR      (SPI1_BASE + 0x08UL)
#define SPI1_SPBR       (SPI1_BASE + 0x0AUL)
#define SPI1_SPDCR      (SPI1_BASE + 0x0BUL)
#define SPI1_SPCMD0     (SPI1_BASE + 0x10UL)

#define SPCR_SPE        0x40U
#define SPCR_MSTR       0x08U
#define SPSR_OVRF       0x01U    /* receive overrun — see spi.c, this one bites */
#define SPSR_IDLNF      0x02U
#define SPSR_MODF       0x04U
#define SPSR_PERF       0x08U
#define SPSR_UDRF       0x10U
#define SPSR_ERRORS     (SPSR_OVRF | SPSR_MODF | SPSR_PERF | SPSR_UDRF)
#define SPDCR_SPLW      0x20U

/* ---------------------------------------------------------------- clock frequencies */
/* Derived from PLLCCR 0x2701: 24 MHz crystal / 2 * 20 = 240 MHz, then SCKDIVCR 0x20011221. */
#define F_XTAL_HZ       24000000UL
#define F_PLL_HZ        240000000UL
#define F_ICLK_HZ       240000000UL    /* /1  */
#define F_PCLKA_HZ      120000000UL    /* /2  */
#define F_PCLKB_HZ       60000000UL    /* /4  */
#define F_PCLKC_HZ       60000000UL    /* /4  */
#define F_PCLKD_HZ      120000000UL    /* /2  */
#define F_BCLK_HZ       120000000UL    /* /2  */
#define F_FCLK_HZ        60000000UL    /* /4  */

#endif /* ELECTRA_S7G2_H */
