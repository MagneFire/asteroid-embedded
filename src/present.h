#ifndef PRESENT_H
#define PRESENT_H
#include <stdint.h>
#include <stdbool.h>

void present_init(bool have_ui, bool have_haptics, bool have_hub);
void present_calc(void);
void present_game(void);
void present_agenda(void);
void present_agenda_day(uint32_t date);
void present_settings(void);
void present_events(uint32_t now);
void present_weather(void);
void present_clock(uint32_t now);
void present_clock_soon(void);
#endif
