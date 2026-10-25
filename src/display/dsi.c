/* DCS commands through the DSI controller's command DMA: the packet
 * (data, data, type, flags) is read from memory, the transfer is started
 * with the software trigger and completion is the DMA-done bit in
 * INT_CTRL. Type 0x05 is a DCS short write without parameter, 0x15 with
 * one, 0x39 a long write; flag 0x80 marks the last packet. */
#include <string.h>
#include "asteroid.h"
#include "display.h"

static uint8_t packet[64] __attribute__((aligned(8)));

int dsi_dcs_write(uint8_t cmd, const uint8_t *params, unsigned n)
{
	uint32_t len;
	if (n == 0) {
		packet[0] = cmd; packet[1] = 0; packet[2] = 0x05; packet[3] = 0x80;
		len = 4;
	} else if (n == 1) {
		packet[0] = cmd; packet[1] = params[0]; packet[2] = 0x15; packet[3] = 0x80;
		len = 4;
	} else {
		if (n + 1 > sizeof packet - 4)
			return -1;
		packet[0] = (uint8_t)(n + 1); packet[1] = 0; packet[2] = 0x39; packet[3] = 0xc0;
		packet[4] = cmd;
		memcpy(packet + 5, params, n);
		len = (4 + n + 1 + 3) & ~3u;
	}
	dcache_clean_range(packet, sizeof packet);
	writel((uint32_t)(uintptr_t)packet, DSI_DMA_CMD_OFFSET);
	writel(len, DSI_DMA_CMD_LENGTH);
	writel(0x03030303, DSI_INT_CTRL);
	writel(1, DSI_CMD_MODE_DMA_SW_TRIGGER);
	dsb();
	uint32_t t0 = time_us();
	while (!(readl(DSI_INT_CTRL) & 1)) {
		if (time_us() - t0 > 20000) {
			log_printf("dsi: command %02x timed out, int_ctrl %08lx\n", cmd, readl(DSI_INT_CTRL));
			return -2;
		}
	}
	delay_us(80);
	return 0;
}
