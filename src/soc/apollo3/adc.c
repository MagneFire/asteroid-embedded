/* The battery's voltage on the ADC: one slot on its pad's channel, 14
 * bits against the internal 2.0 V reference, averaged in hardware, then
 * the ADC is powered down again. The divider in front of it is the
 * board's. */
#include "asteroid.h"

#define ADC_CFG		(ADC_BASE + 0x00)
#define ADC_STAT	(ADC_BASE + 0x04)
#define ADC_SWT		(ADC_BASE + 0x08)
#define ADC_SL0CFG	(ADC_BASE + 0x0c)
#define ADC_FIFO	(ADC_BASE + 0x38)
#define ADC_INTSTAT	(ADC_BASE + 0x204)
#define ADC_INTCLR	(ADC_BASE + 0x208)
#define CFG_HFRC	(1u << 24)
#define CFG_SWT		(7u << 16)
#define CFG_REF_2V0	(0u << 8)
#define CFG_ADCEN	BIT(0)
#define SL_AVG16	(4u << 24)
#define SL_14BIT	(0u << 16)
#define SL_CHANNEL(n)	((n) << 8)
#define SL_EN		BIT(0)
#define SWT_KEY		0x37u
#define INT_SCNCMP	BIT(1)
#define FIFO_COUNT(v)	((v) >> 20 & 0xff)

uint32_t adc_battery_mv(void)
{
	unsigned pad = board->battery_adc.pad, ch = board->battery_adc.channel;
	if (!board->battery_adc.mv_full)
		return 0;
	pad_config(pad, board->battery_adc.func, 0);
	periph_power(PWR_ADC, true);
	writel(0, ADC_CFG);
	writel(SL_AVG16 | SL_14BIT | SL_CHANNEL(ch) | SL_EN, ADC_SL0CFG);
	writel(~0u, ADC_INTCLR);
	writel(CFG_HFRC | CFG_SWT | CFG_REF_2V0 | CFG_ADCEN, ADC_CFG);
	delay_us(50);
	writel(SWT_KEY, ADC_SWT);
	uint32_t t0 = time_ms(), raw = 0;
	bool got = false;
	while (time_ms() - t0 < 10) {
		uint32_t v = readl(ADC_FIFO);
		if (FIFO_COUNT(v)) {
			/* 14.6 fixed point: the average's fraction below. */
			raw = (v & 0xfffff) >> 6;
			writel(0, ADC_FIFO);
			got = true;
			break;
		}
	}
	writel(0, ADC_CFG);
	periph_power(PWR_ADC, false);
	return got ? raw * board->battery_adc.mv_full / 16384 : 0;
}
