#include "utils.h"

const char *DAYS[7] = {
	 "Monday", "Tuesday", "Wednesday",
	 "Thursday", "Friday", "Saturday", "Sunday"};

const char *MONTHS[12] = {
	 "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const int DAYS_IN_MONTH[12] = {
	 31, 28, 31, 30, 31, 30,
	 31, 31, 30, 31, 30, 31};

bool is_leap_year(int y)
{
	return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
}

int days_in_month(int year, int month)
{
	if (month == 2 && is_leap_year(year))
		return 29;
	return DAYS_IN_MONTH[month - 1];
}

int first_weekday_of_month(int year, int month)
{
	int q = 1; // first day of month
	if (month <= 2)
	{
		month += 12;
		year -= 1;
	}
	int m = month;
	int K = year % 100;
	int J = year / 100;
	int h = (q + (13 * (m + 1)) / 5 + K + (K / 4) + (J / 4) + 5 * J) % 7;
	// Shift: Zeller's gives 0=Saturday → make Monday=0
	return (h + 5) % 7;
}

void log_boot_message(const char *tag, const char *format, ...)
{
	va_list args;
	printf("%s: ", tag);
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");
}

uint16_t rainbow565(uint8_t pos)
{
	uint8_t r, g, b;
	if (pos < 85)
	{
		r = 255 - pos * 3;
		g = pos * 3;
		b = 0;
	}
	else if (pos < 170)
	{
		pos -= 85;
		r = 0;
		g = 255 - pos * 3;
		b = pos * 3;
	}
	else
	{
		pos -= 170;
		r = pos * 3;
		g = 0;
		b = 255 - pos * 3;
	}
	return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

int round_float(float v)
{
	return (int)(v + 0.5f);
}

void test_screen(MatrixPanel_I2S_DMA *screen)
{
	// fix the screen with green
	screen->clearScreen();
	screen->fillRect(0, 0, screen->width(), screen->height(), screen->color444(0, 15, 0));
	// screen->flipDMABuffer();
	delay(250);

	// draw a box in yellow
	screen->clearScreen();
	screen->drawRect(0, 0, screen->width(), screen->height(), screen->color444(15, 15, 0));
	// screen->flipDMABuffer();
	delay(250);

	// draw an 'X' in red
	screen->drawLine(0, 0, screen->width() - 1, screen->height() - 1, screen->color444(15, 0, 0));
	screen->drawLine(screen->width() - 1, 0, 0, screen->height() - 1, screen->color444(15, 0, 0));
	// screen->flipDMABuffer();
	delay(250);

	// draw a blue circle
	screen->drawCircle(10, 10, 10, screen->color444(0, 0, 15));
	// screen->flipDMABuffer();
	delay(250);

	// fill a violet circle
	screen->fillCircle(40, 21, 10, screen->color444(15, 0, 15));
	// dma_display->flipDMABuffer();
	delay(250);
	delay(1000);
}
bool useBlackText(uint16_t color565)
{
	// Extract 5/6/5 components (same as your code)
	uint8_t r5 = (color565 >> 11) & 0x1F;
	uint8_t g6 = (color565 >> 5) & 0x3F;
	uint8_t b5 = color565 & 0x1F;

	// CHANGED: scale to 0..255 with rounding, keep as int to avoid accidental overflow
	int r = (r5 * 255 + 15) / 31; // +15 -> rounding
	int g = (g6 * 255 + 31) / 63; // +31 -> rounding
	int b = (b5 * 255 + 15) / 31;

	// CHANGED: integer luminance approximation equivalent to 0.299/0.587/0.114
	int lum = (299 * r + 587 * g + 114 * b) / 1000;

	// same decision rule as yours (bright => black)
	return lum > 128;
}

uint16_t brightenDown(uint16_t color)
{
	uint8_t r5 = (color >> 11) & 0x1F;
	uint8_t g6 = (color >> 5) & 0x3F;
	uint8_t b5 = color & 0x1F;

	// CHANGED: map 8-bit minimum 40 -> 5/6-bit equivalents (rounded)
	// 40 * 31 / 255 ≈ 4.86 -> 5 ; 40 * 63 / 255 ≈ 9.88 -> 10
	const uint8_t R_MIN = (40 * 31 + 127) / 255; // == 5
	const uint8_t G_MIN = (40 * 63 + 127) / 255; // == 10
	const uint8_t B_MIN = (40 * 31 + 127) / 255; // == 5

	while (r5 > R_MIN && g6 > G_MIN && b5 > B_MIN)
	{
		r5--;
		if (g6 > G_MIN)
			g6--;
		if (b5 > B_MIN)
			b5--;
	}
	return (r5 << 11) | (g6 << 5) | b5;
}