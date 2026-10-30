/* Slint user interface (ui/): ui_init once with a cached heap region and
 * UI_PANELS layers of stride_px * height premultiplied RGBA pixels (bytes
 * R, G, B, A), panel_px pixels apart, which it clears to transparent. Each
 * panel is one screen: the watchface, the screens beside it (see stack.h)
 * and the overlay drawn over all of them while ui_modal says a
 * notification or the pairing code is up. Per frame, feed touch events to
 * the panel on screen, call ui_tick once, then ui_update per panel
 * repaints its layer where it changed, on the GPU while it is on, and
 * returns the bounding box of the change; ui_render blends the
 * watchface and overlay layers over a finished XRGB8888 background on
 * the CPU; ui_repaint repaints a whole panel, on the GPU or the CPU,
 * for measuring. weekday 0 = Sunday, month 1-12;
 * touch kind 0 down, 1 move, 2 up; a passkey below 0 hides the pairing
 * code. Notifications are set as a list, newest first; ui_show_notification
 * pops one of them up by id, and ui_notification_dismissed hands out, one
 * per call, the ids the user dismissed. ui_media_action returns the next
 * pressed music button or -1. Weather days start today, temperatures in
 * degrees Celsius, a volume below 0 is unknown. ui_set_ambient switches
 * the watchface to its always-on form. The app panel shows the app set
 * with ui_set_app; ui_app_opened returns the next app picked in the
 * launcher, or -1. ui_set_settings shows the settings (brightness in
 * per cent) and ui_setting_toggled returns the next one the user
 * changed, as an enum setting, or -1. */
#ifndef UI_H
#define UI_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_HEAP_BASE 0x06000000u
#define UI_HEAP_SIZE 0x04000000u
#define UI_LAYER_BASE 0x0a000000u
#define UI_LAYER_STRIDE 0x00100000u
#define UI_PANELS 7

enum ui_touch_kind { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP };
enum ui_panel { UI_PANEL_FACE, UI_PANEL_LEFT, UI_PANEL_RIGHT, UI_PANEL_BOTTOM, UI_PANEL_OVERLAY, UI_PANEL_TOP, UI_PANEL_APP };
enum ui_app { UI_APP_MUSIC };
enum ui_media_action {
	UI_MEDIA_PREVIOUS,
	UI_MEDIA_NEXT,
	UI_MEDIA_PLAY,
	UI_MEDIA_PAUSE,
	UI_MEDIA_VOLUME_UP = 5,
	UI_MEDIA_VOLUME_DOWN,
};

bool ui_init(void *heap, uint32_t heap_size, uint32_t *layers, uint32_t panel_px, uint32_t stride_px, uint32_t width,
	     uint32_t height);
void ui_set_clock(uint32_t hour, uint32_t minute, uint32_t second, uint32_t weekday, uint32_t day, uint32_t month);
void ui_set_battery(uint32_t percent, bool charging);
void ui_set_ambient(bool ambient);
void ui_set_passkey(int32_t passkey);
void ui_clear_notifications(void);
void ui_add_notification(int32_t id, const char *app, uint32_t app_len, const char *summary, uint32_t summary_len,
			 const char *body, uint32_t body_len, const char *icon,
			 uint32_t icon_len);
void ui_show_notification(int32_t id);
void ui_hide_notification(void);
bool ui_notification_dismissed(int32_t *id);
void ui_set_media(const char *title, uint32_t title_len, const char *artist, uint32_t artist_len, const char *album,
		  uint32_t album_len, bool playing, int32_t volume);
int ui_media_action(void);
int ui_app_opened(void);
void ui_set_settings(uint32_t brightness, bool bluetooth, bool vibration, bool aod, bool clock12);
int ui_setting_toggled(void);
void ui_set_app(int app);
void ui_set_weather(const char *city, uint32_t city_len, uint32_t days, const int32_t *ids, const int32_t *min_c,
		    const int32_t *max_c);
bool ui_modal(void);
void ui_touch(uint32_t panel, int kind, int x, int y);
void ui_tick(void);
bool ui_update(uint32_t panel, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
void ui_render(uint32_t *buf, uint32_t stride_px);
void ui_repaint(uint32_t panel, bool gpu);
void ui_panic(const char *msg, size_t len) __attribute__((noreturn));
#endif
