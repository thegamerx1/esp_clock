#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <WString.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cstdarg>
#include <cstdio>

// Global string arrays
extern const char *DAYS[7];
extern const char *MONTHS[12];
extern const int DAYS_IN_MONTH[12];

// Leap year check
bool is_leap_year(int y);

// Get days in a month
int days_in_month(int year, int month);

// Get weekday of first day of given month
// Returns: 0=Monday, 1=Tuesday, ..., 6=Sunday
int first_weekday_of_month(int year, int month);

void log_boot_message(const char *tag, const char *format, ...);
uint16_t rainbow565(uint8_t pos);

int round_float(float v);

void test_screen(MatrixPanel_I2S_DMA *screen);

struct Frame
{
	uint8_t *data = nullptr;
	size_t size = 0;
};

bool useBlackText(uint16_t color565);

#endif