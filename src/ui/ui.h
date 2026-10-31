/* Slint user interface (ui/): ui_init once with a cached heap region and
 * UI_PANELS layers of stride_px * height premultiplied RGBA pixels (bytes
 * R, G, B, A), panel_px pixels apart, which it clears to transparent. Each
 * panel is one screen: the watchface, the screens beside it (see stack.h)
 * and the overlay drawn over all of them while ui_modal says a
 * notification or the pairing code is up. Per frame, feed touch events to
 * the panel on screen, call ui_tick once, then ui_update per panel
 * repaints its layer where it changed, on the GPU while it is on, and
 * returns the bounding box of the change; ui_render blends the
 * watchface, moved by (dx, dy), and overlay layers over a finished
 * XRGB8888 background on the CPU; ui_repaint repaints a whole panel, on
 * the GPU or the CPU, for measuring. weekday 0 = Sunday, month 1-12;
 * touch kind 0 down, 1 move, 2 up; a passkey below 0 hides the pairing
 * code. ui_set_clock also takes the RTC (seconds), against which the
 * notifications' times (RTC seconds) give their age; ui_set_connected
 * says whether a phone is connected. Notifications are set as a list,
 * newest first; ui_show_notification
 * pops one of them up by id. Weather days start today, temperatures in
 * degrees Celsius, a volume below 0 is unknown. ui_set_ambient switches
 * the watchface to its always-on form. The app panel shows the app set
 * with ui_set_app, none for -1. ui_set_settings shows the settings,
 * ui_set_face selects the watchface and whether nightstand mode is on
 * and in effect, ui_set_launcher the launcher layout.
 * ui_launcher_edges says which edges of the launcher lead somewhere
 * (left, right, top, bottom as bits 0-3) and, in bit 4, whether it is at
 * its start; ui_notes_edges does the same for the notifications, bit 4
 * meaning the newest is on screen; ui_launcher_colors gives its mesh
 * colours, or false to keep the mesh grey. ui_set_screen says which
 * panel the stack has on screen, under any app. ui_set_ringing shows a
 * ringing alarm or timer, ui_set_timer and ui_set_stopwatch their
 * state, ui_set_laps the stopwatch's time at each lap, newest first.
 *
 * ui_next_event hands out what the user did, oldest first, one per call,
 * with its value: UI_EVENT_SETTING an enum setting toggled or an enum
 * ui_action (UI_WATCHFACE_FIRST + n picks watchface n, UI_LAUNCHER_FIRST
 * + n launcher layout n), UI_EVENT_APP the app picked in the launcher,
 * UI_EVENT_MEDIA a music button, UI_EVENT_CLOCK a timer or stopwatch
 * button, UI_EVENT_ALARM_RING a button on a ringing alarm or timer,
 * UI_EVENT_ALARM_EDIT the alarm at that index (-1 for a new one) as
 * edited, UI_EVENT_ALARM_DELETE the alarm at that index deleted and
 * UI_EVENT_DISMISS the id of a dismissed notification, UI_EVENT_TIMER_SET
 * the timer's new duration in seconds, UI_EVENT_BRIGHTNESS the
 * brightness in per cent, UI_EVENT_DISMISS_ALL all notifications
 * dismissed, UI_EVENT_SNOOZE the id of a notification put away for the
 * hours and minutes in its alarm, UI_EVENT_SETTING_VALUE an enum
 * setting in bits 8 and up set to the value in bits 0-7,
 * UI_EVENT_TIME_SET the time of day set by hand in minutes and
 * UI_EVENT_DATE_SET a date set by hand as (year - 2000) << 9 | month << 5
 * | day.
 *
 * Every ui_* function is called from the main loop only, never from an
 * interrupt handler, as the UI keeps its state in plain statics. Strings
 * and arrays passed in are copied before the call returns, and pointers
 * passed out must be valid for the call only. */
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
enum ui_app { UI_APP_MUSIC, UI_APP_SETTINGS, UI_APP_ALARM, UI_APP_TIMER, UI_APP_STOPWATCH, UI_APP_WEATHER };
enum ui_clock_action {
	UI_TIMER_START = 20,
	UI_TIMER_RESET,
	UI_TIMER_MINUTE_UP,
	UI_TIMER_MINUTE_DOWN,
	UI_TIMER_SECONDS_UP,
	UI_TIMER_SECONDS_DOWN,
	UI_STOPWATCH_START = 30,
	UI_STOPWATCH_RESET,
	UI_STOPWATCH_LAP,
};
enum ui_alarm_action { UI_ALARM_SNOOZE, UI_ALARM_DISMISS };
enum ui_action {
	UI_BRIGHTNESS_DOWN = 32,
	UI_BRIGHTNESS_UP,
	UI_FORGET_PHONE,
	UI_WATCHFACE_FIRST,
	UI_LOCK = 37,
	UI_OPEN_SETTINGS,
	UI_REBOOT,
	UI_POWER_OFF,
	UI_LAUNCHER_FIRST = 48
};
#define UI_LAUNCHERS 7
enum ui_media_action {
	UI_MEDIA_PREVIOUS,
	UI_MEDIA_NEXT,
	UI_MEDIA_PLAY,
	UI_MEDIA_PAUSE,
	UI_MEDIA_VOLUME_UP = 5,
	UI_MEDIA_VOLUME_DOWN,
};

/* Shared with the Rust side (ui/src/watch.rs), which lays them out the
 * same way and checks their sizes. */
struct ui_alarm {
	uint8_t hour, minute, days;
	bool on;
};

struct ui_rgb {
	uint8_t r, g, b;
};

struct ui_colors {
	struct ui_rgb center, outer;
};

/* Brightnesses in per cent, the nightstand delay in seconds, burn_in
 * 0 off, 1 high, 2 medium, 3 low. */
struct ui_settings {
	uint8_t brightness, nightstand_brightness, nightstand_delay, burn_in;
	bool bluetooth, vibration, aod, clock12, tilt, tap, fahrenheit, nightstand_aod;
};

enum ui_event_kind {
	UI_EVENT_SETTING,
	UI_EVENT_APP,
	UI_EVENT_MEDIA,
	UI_EVENT_CLOCK,
	UI_EVENT_ALARM_RING,
	UI_EVENT_ALARM_EDIT,
	UI_EVENT_ALARM_DELETE,
	UI_EVENT_DISMISS,
	UI_EVENT_TIMER_SET,
	UI_EVENT_BRIGHTNESS,
	UI_EVENT_DISMISS_ALL,
	UI_EVENT_SNOOZE,
	UI_EVENT_SETTING_VALUE,
	UI_EVENT_TIME_SET,
	UI_EVENT_DATE_SET,
};

struct ui_event {
	uint32_t kind;
	int32_t value;
	struct ui_alarm alarm;
};

_Static_assert(sizeof(struct ui_alarm) == 4 && sizeof(struct ui_colors) == 6 && sizeof(struct ui_settings) == 12,
	       "ui types");
_Static_assert(sizeof(struct ui_event) == 12, "ui_event");

bool ui_init(void *heap, uint32_t heap_size, uint32_t *layers, uint32_t panel_px, uint32_t stride_px, uint32_t width,
	     uint32_t height);
void ui_set_clock(uint32_t hour, uint32_t minute, uint32_t second, uint32_t weekday, uint32_t day, uint32_t month,
		  uint32_t year, uint32_t rtc);
void ui_set_connected(bool connected);
void ui_set_battery(uint32_t percent, bool charging);
void ui_set_ambient(bool ambient);
void ui_set_passkey(int32_t passkey);
void ui_clear_notifications(void);
void ui_add_notification(int32_t id, const char *app, uint32_t app_len, const char *summary, uint32_t summary_len,
			 const char *body, uint32_t body_len, const char *icon, uint32_t icon_len, uint32_t time);
void ui_show_notification(int32_t id);
void ui_hide_notification(void);
void ui_set_media(const char *title, uint32_t title_len, const char *artist, uint32_t artist_len, const char *album,
		  uint32_t album_len, bool playing, int32_t volume);
void ui_app_peek(int px);
void ui_app_peek_end(bool back);
int ui_app_depth(void);
void ui_set_screen(unsigned panel);
bool ui_launcher_colors(struct ui_colors *out);
void ui_set_launcher(int layout);
unsigned ui_launcher_edges(void);
unsigned ui_notes_edges(void);
bool ui_app_colors(int app, struct ui_colors *out);
void ui_set_settings(const struct ui_settings *s);
void ui_set_build(const char *build);
void ui_set_face(int watchface, bool nightstand_setting, bool nightstand);
void ui_set_alarms(const struct ui_alarm *alarms, uint32_t n);
bool ui_next_event(struct ui_event *e);
void ui_set_ringing(bool ringing, bool timer);
void ui_set_timer(uint32_t ms, uint32_t set_ms, bool running);
void ui_set_stopwatch(uint32_t ms, bool running);
void ui_set_laps(const uint32_t *ms, uint32_t n);
void ui_set_app(int app);
void ui_set_weather(const char *city, uint32_t city_len, uint32_t days, const int32_t *ids, const int32_t *min_c,
		    const int32_t *max_c);
bool ui_modal(void);
void ui_touch(uint32_t panel, int kind, int x, int y);
void ui_tick(void);
bool ui_update(uint32_t panel, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
void ui_render(uint32_t *buf, uint32_t stride_px, int dx, int dy);
void ui_repaint(uint32_t panel, bool gpu);
void ui_panic(const char *msg, size_t len) __attribute__((noreturn));
#endif
