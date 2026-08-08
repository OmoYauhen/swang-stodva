#ifndef INCLUDE_MAIN_H_
#define INCLUDE_MAIN_H_

#include "common.h"
#include "button.h"

/* Typedef of unions for handy access of single bytes */
/* Access bytewise: U16 var; var.byte[x] = z; */
/* Access value: U32 var; var.u32 = 0xFFFFFFFF; */
typedef union
{
  uint16_t u16;
  uint8_t byte[2];
} U16;

typedef union
{
  uint32_t u32;
  uint8_t byte[4];
} U32;

void system_power(bool state);

uint32_t get_seconds(); // how many seconds since boot
uint32_t get_time_base_counter_1ms();

void SW102_rt_processing_stop(void);
void SW102_rt_processing_start(void);

// Recompute unit-conversion flags (kph/mph, C/F, kg/lb); defined in main.c.
void set_conversions(void);

// Per-tick graph bookkeeping; defined in main.c (real hardware has no graphs, so it is a stub).
void rt_graph_process(void);

// Index of the currently shown main screen; defined in main.c, persisted to EEPROM.
extern uint8_t g_showNextScreenIndex, g_showNextScreenPreviousIndex;

extern Button buttonM, buttonDWN, buttonUP, buttonPWR;

#endif /* INCLUDE_MAIN_H_ */
