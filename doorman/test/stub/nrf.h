/* Host stand-in for the nRF52840 registers doorman/dongle-fw/src/link.c touches, so hop_test.c can build the
 * real link.c on a PC. Plain memory: nothing here behaves like the radio. Only for tests that call link.c's
 * own functions directly (never link_init / link_poll, which wait on the radio and the timer). */
#pragma once
#include <stdint.h>

typedef struct {
  volatile uint32_t TASKS_TXEN, TASKS_RXEN, TASKS_START, TASKS_DISABLE, TASKS_RSSISTART;
  volatile uint32_t EVENTS_READY, EVENTS_ADDRESS, EVENTS_DISABLED, EVENTS_RSSIEND;
  volatile uint32_t EVENTS_END, EVENTS_TXREADY, EVENTS_RXREADY, EVENTS_CRCOK, EVENTS_CRCERROR;
  volatile uint32_t SHORTS, INTENSET, INTENCLR, CRCSTATUS, RXMATCH, PACKETPTR, FREQUENCY, TXPOWER, MODE, PCNF0, PCNF1;
  volatile uint32_t BASE0, BASE1, PREFIX0, PREFIX1, TXADDRESS, RXADDRESSES, CRCCNF, CRCPOLY, CRCINIT;
  volatile uint32_t RSSISAMPLE, STATE, MODECNF0, POWER;
} NRF_RADIO_Type;
typedef struct {
  volatile uint32_t TASKS_START, TASKS_STOP, TASKS_COUNT, TASKS_CLEAR, TASKS_CAPTURE[6], EVENTS_COMPARE[6];
  volatile uint32_t MODE, BITMODE, PRESCALER, CC[6];
} NRF_TIMER_Type;
typedef struct { volatile uint32_t EVENTS_HFCLKSTARTED, TASKS_HFCLKSTART, HFCLKSTAT; } NRF_CLOCK_Type;
typedef struct { volatile uint32_t EN, DIS; } NRF_PPI_TASKS_CHG_Type;
typedef struct { volatile uint32_t EEP, TEP; } NRF_PPI_CH_Type;
typedef struct { volatile uint32_t TEP; } NRF_PPI_FORK_Type;
typedef struct {
  NRF_PPI_TASKS_CHG_Type TASKS_CHG[6];
  volatile uint32_t CHEN, CHENSET, CHENCLR;
  NRF_PPI_CH_Type CH[20];
  volatile uint32_t CHG[6];
  NRF_PPI_FORK_Type FORK[32];
} NRF_PPI_Type;
typedef struct { volatile uint32_t DEVICEID[2]; } NRF_FICR_Type;
typedef struct { volatile uint32_t READY, CONFIG, ERASEPAGE; } NRF_NVMC_Type;

extern NRF_RADIO_Type stub_radio;
extern NRF_TIMER_Type stub_timer1, stub_timer2, stub_timer3;
extern NRF_PPI_Type stub_ppi;
extern NRF_CLOCK_Type stub_clock;
extern NRF_FICR_Type stub_ficr;
extern NRF_NVMC_Type stub_nvmc;
#define NRF_RADIO  (&stub_radio)
#define NRF_TIMER1 (&stub_timer1)
#define NRF_TIMER2 (&stub_timer2)
#define NRF_TIMER3 (&stub_timer3)
#define NRF_PPI    (&stub_ppi)
#define NRF_CLOCK  (&stub_clock)
#define NRF_FICR   (&stub_ficr)
#define NRF_NVMC   (&stub_nvmc)

#define RADIO_SHORTS_READY_START_Msk       (1u << 0)
#define RADIO_SHORTS_END_DISABLE_Msk       (1u << 1)
#define RADIO_SHORTS_DISABLED_TXEN_Msk     (1u << 2)
#define RADIO_SHORTS_DISABLED_RXEN_Msk     (1u << 3)
#define RADIO_SHORTS_ADDRESS_RSSISTART_Msk (1u << 4)
#define RADIO_SHORTS_END_START_Msk         (1u << 5)
#define RADIO_INTENSET_READY_Msk           (1u << 0)
#define RADIO_INTENSET_END_Msk             (1u << 3)
#define RADIO_INTENSET_DISABLED_Msk        (1u << 4)
#define RADIO_STATE_STATE_Disabled         0
#define CLOCK_HFCLKSTAT_SRC_Msk            (1u << 0)
#define RADIO_IRQn                         1
#define RADIO_MODE_MODE_Nrf_2Mbit          1
#define RADIO_MODECNF0_RU_Fast             1
#define RADIO_MODECNF0_RU_Pos              0
#define RADIO_TXPOWER_TXPOWER_Pos8dBm      8
#define RADIO_TXPOWER_TXPOWER_Neg4dBm      0xFC
#define RADIO_PCNF0_LFLEN_Pos              0
#define RADIO_PCNF0_PLEN_Pos               24
#define RADIO_PCNF0_PLEN_8bit              0
#define RADIO_PCNF1_MAXLEN_Pos             0
#define RADIO_PCNF1_BALEN_Pos              16
#define RADIO_PCNF1_ENDIAN_Pos             24
#define RADIO_PCNF1_ENDIAN_Big             1
#define RADIO_CRCCNF_LEN_Two               2

/* Interrupts: nothing to mask or route on a PC. */
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void NVIC_SetPriority(int irq, uint32_t priority) { (void)irq; (void)priority; }
static inline void NVIC_EnableIRQ(int irq) { (void)irq; }
static inline void NVIC_ClearPendingIRQ(int irq) { (void)irq; }
