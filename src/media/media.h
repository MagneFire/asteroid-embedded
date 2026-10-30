#ifndef MEDIA_H
#define MEDIA_H
#include <stdint.h>
#include <stdbool.h>

#define MEDIA_PREVIOUS		0
#define MEDIA_NEXT		1
#define MEDIA_PLAY		2
#define MEDIA_PAUSE		3
#define MEDIA_VOLUME		4
#define MEDIA_VOLUME_UP		5
#define MEDIA_VOLUME_DOWN	6

struct media {
	char title[96], album[96], artist[96];
	bool playing;
	int volume;
};

void media_write(uint16_t chr, const uint8_t *p, unsigned n);
const struct media *media_get(void);
uint32_t media_serial(void);
bool media_command(unsigned command);
#endif
