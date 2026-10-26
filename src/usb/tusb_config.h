/* TinyUSB configuration: device only, one ChipIdea high-speed port, no
 * operating system, cached buffers cleaned and invalidated by the port. */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU			OPT_MCU_NONE
#define CFG_TUSB_OS			OPT_OS_NONE
#define CFG_TUSB_DEBUG			1
#define CFG_TUSB_DEBUG_PRINTF		tusb_log
int tusb_log(const char *fmt, ...);

#define TUP_USBIP_CHIPIDEA_HS
#define TUP_DCD_ENDPOINT_MAX		16

#define CFG_TUD_ENABLED			1
#define CFG_TUD_MAX_SPEED		OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE		64
#define CFG_TUD_CDC			1
#define CFG_TUD_CDC_RX_BUFSIZE		512
#define CFG_TUD_CDC_TX_BUFSIZE		4096
#define CFG_TUD_CDC_EP_BUFSIZE		512

#define CFG_TUD_MEM_DCACHE_ENABLE	1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE	64
#define CFG_TUSB_MEM_ALIGN		__attribute__((aligned(64)))

#endif
