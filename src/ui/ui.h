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
 * says whether a phone is connected and whether one is paired;
 * ui_set_heart_rate gives the Heart Rate app its reading, 0 while it
 * measures, and ui_app_open the app open on the app panel, -1 for none;
 * ui_flashlight_on says whether the Flashlight app's light is on.
 * ui_set_calc gives the calculator its display text and the length of
 * the number it shows, which sets the font size; ui_set_game the
 * UI_TILES tiles of the Diamonds game, its score, best score and whether
 * it is over; ui_set_agenda the events of the day the Agenda app shows,
 * each with its index in the agenda, and ui_set_today today's events for
 * Today.
 * Notifications are set as a list, newest first; ui_show_notification
 * pops one of them up by id. Weather days start today, temperatures in
 * degrees Celsius, a volume below 0 is unknown. ui_set_ambient switches
 * the watchface to its always-on form; ui_slept cancels a countdown to
 * power off, reboot or forget the phone. The app panel shows the app set
 * with ui_set_app, none for -1. ui_set_settings shows the settings,
 * ui_set_face selects the watchface and whether nightstand mode is on
 * and in effect, ui_set_launcher the launcher layout.
 * ui_launcher_edges says which edges of the launcher lead somewhere
 * (left, right, top, bottom as bits 0-3), in bit 4 whether it is at its
 * start and in bit 5 whether the press is on the speed dial's name; ui_notes_edges does the same for the notifications, bit 4
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
 * | day, UI_EVENT_CALC a calculator key (enum ui_calc_key) and
 * UI_EVENT_GAME a move in the Diamonds game (enum ui_game_move),
 * UI_EVENT_AGENDA_DAY the day the Agenda app turned to, as a date set by
 * hand, UI_EVENT_AGENDA_EDIT an agenda event as edited, its index in
 * bits 0-7 (0xff for a new one) and its date in bits 8 and up, its start
 * and end in minutes of the day in the high and low half of arg, and
 * UI_EVENT_AGENDA_DELETE the agenda event at that index deleted.
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

#include "memmap.h"

#define UI_PANELS 7
_Static_assert(UI_PANELS * UI_LAYER_STRIDE <= UI_LAYER_SIZE, "more panels than layers");

enum ui_touch_kind { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP };
enum ui_panel { UI_PANEL_FACE, UI_PANEL_LEFT, UI_PANEL_RIGHT, UI_PANEL_BOTTOM, UI_PANEL_OVERLAY, UI_PANEL_TOP, UI_PANEL_APP };
enum ui_app {
	UI_APP_MUSIC,
	UI_APP_SETTINGS,
	UI_APP_ALARM,
	UI_APP_TIMER,
	UI_APP_STOPWATCH,
	UI_APP_WEATHER,
	UI_APP_HEART_RATE,
	UI_APP_FLASHLIGHT,
	UI_APP_CALCULATOR,
	UI_APP_DIAMONDS,
	UI_APP_AGENDA,
	UI_APP_ACTIVITY,
	UI_APP_STEPS,
	UI_APP_COMPASS,
};
/* The Compass app: asteroid-compass's "No data" page while the field's
 * accuracy is unreliable or unknown, the dial with a line asking to
 * calibrate while it is low or medium, the dial alone at high. */
enum ui_compass_state { UI_COMPASS_NO_DATA, UI_COMPASS_CALIBRATE, UI_COMPASS_READY };
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
	UI_CINEMA,
	UI_PAIR_REJECT,
	UI_PAIR_ACCEPT,
	UI_TIMEPIECE,
	UI_RESET_WATCH,
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
/* The calculator's keys: the digits are their values. */
enum ui_calc_key {
	UI_CALC_POINT = 10,
	UI_CALC_ADD,
	UI_CALC_SUBTRACT,
	UI_CALC_MULTIPLY,
	UI_CALC_DIVIDE,
	UI_CALC_EQUALS,
	UI_CALC_CLEAR,
	UI_CALC_BACK,
	UI_CALC_NEGATE,
};
enum ui_game_move { UI_GAME_LEFT, UI_GAME_RIGHT, UI_GAME_UP, UI_GAME_DOWN, UI_GAME_RESET };

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

/* A tile of the Diamonds game: its column and row, -1 while unused, its
 * value, whether it just merged (pop) and whether it slid there (anim). */
struct ui_tile {
	int8_t x, y;
	uint16_t val;
	bool pop, anim;
};
#define UI_TILES 16

/* An event of the Agenda app: its index in the agenda, its start and end
 * in minutes of the day and its title, NUL-terminated. */
#define UI_AGENDA_TITLE 24
struct ui_agenda {
	int16_t index;
	uint16_t start, end;
	char title[UI_AGENDA_TITLE];
};

/* Brightnesses in per cent, the nightstand delay in seconds, burn_in
 * 0 off, 1 high, 2 medium, 3 low, tilt an enum tilt_mode,
 * backlight_color an enum backlight_color. */
struct ui_settings {
	uint8_t brightness, nightstand_brightness, nightstand_delay, burn_in;
	bool bluetooth, vibration, aod, clock12;
	uint8_t tilt;
	bool tap, fahrenheit, nightstand_aod, nightstand_rotate, auto_brightness;
	uint8_t height_cm, weight_kg, hr_interval, age;
	bool male;
	uint8_t backlight_color;
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
	UI_EVENT_CALC,
	UI_EVENT_GAME,
	UI_EVENT_AGENDA_DAY,
	UI_EVENT_AGENDA_EDIT,
	UI_EVENT_AGENDA_DELETE,
	UI_EVENT_WORKOUT,
};

struct ui_event {
	uint32_t kind;
	int32_t value, arg;	/* arg: the value of a UI_EVENT_SETTING_VALUE, value its setting */
	struct ui_alarm alarm;
};

_Static_assert(sizeof(struct ui_alarm) == 4 && sizeof(struct ui_colors) == 6 && sizeof(struct ui_settings) == 20 &&
	       sizeof(struct ui_tile) == 6 && sizeof(struct ui_agenda) == 30, "ui types");
_Static_assert(sizeof(struct ui_event) == 16, "ui_event");

bool ui_init(void *heap, uint32_t heap_size, uint32_t *layers, uint32_t panel_px, uint32_t stride_px, uint32_t width,
	     uint32_t height);
void ui_set_clock(uint32_t hour, uint32_t minute, uint32_t second, uint32_t weekday, uint32_t day, uint32_t month,
		  uint32_t year, uint32_t rtc);
/* The watch's codename and whether it has a heart rate sensor, a hub that works out the distance
 * and calories from the wearer's age and sex as well, and one that logs the heart rate by itself. */
/* The power page offers Timepiece, where the hub shows the time with the
 * watch off. */
void ui_set_timepiece(bool on);
void ui_set_tilt_light(bool on);
/* The watch has a magnetometer: the Compass app is there. */
void ui_set_magnetometer(bool on);
void ui_set_board(const char *name, uint32_t len, bool heart_rate, bool round, bool light_sensor, bool burn_in,
		  bool profile, bool passive);
/* The lines at the bottom of the picture the panel does not show. */
void ui_set_chin(uint32_t px);
#ifdef SGX_BASE
/* Text of min_px and over at bins (1, 2 or 4) positions per pixel. */
void ui_glyph_bins(uint32_t bins, uint32_t min_px);
#endif
void ui_set_connected(bool connected, bool bonded);
void ui_set_heart_rate(uint32_t bpm);
/* The Compass app's heading in whole degrees and its enum ui_compass_state. */
void ui_set_heading(uint32_t degrees, uint32_t state);
/* Steps today, the goal, what today's steps come to, and the steps of the last n days, oldest
 * first and today last. */
/* The running workout, or with running false the last one, all 0 for none: its start (RTC) and
 * length, its steps and what they come to, and its average and highest heart rate. */
void ui_set_workout(bool running, uint32_t start, uint32_t seconds, uint32_t steps, uint32_t distance_m, uint32_t kcal,
		    uint32_t avg_bpm, uint32_t max_bpm);
/* The logged heart rates of the last day, oldest first: when, in thousandths of the day back
 * from now, and the rate. */
void ui_set_heart_log(const uint16_t *x, const uint8_t *bpm, uint32_t n);
void ui_set_activity(uint32_t today, uint32_t goal, uint32_t distance_m, uint32_t kcal, const uint32_t *days, uint32_t n);
int ui_app_open(void);
bool ui_flashlight_on(void);
void ui_set_calc(const char *shown, uint32_t shown_len, uint32_t length);
void ui_set_game(const struct ui_tile *tiles, uint32_t score, uint32_t best, bool over);
void ui_set_agenda(const struct ui_agenda *events, uint32_t n);
void ui_set_today(const struct ui_agenda *events, uint32_t n);
void ui_set_battery(uint32_t percent, bool charging);
void ui_set_ambient(bool ambient);
void ui_slept(void);
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
/* Whether the build has the panel's screen, the app, the watchface and
 * the launcher layout: the Cortex-M build leaves some out. */
bool ui_has_panel(unsigned panel);
bool ui_has_app(int app);
bool ui_has_face(int watchface);
bool ui_has_layout(int layout);
bool ui_launcher_colors(struct ui_colors *out);
void ui_set_launcher(int layout);
unsigned ui_launcher_edges(void);
unsigned ui_notes_edges(void);
bool ui_app_colors(int app, struct ui_colors *out);
void ui_set_settings(const struct ui_settings *s);
void ui_set_build(const char *build);
void ui_set_info(const char *bt_address, uint32_t uptime_s);
void ui_set_face(int watchface, bool nightstand_setting, bool nightstand);
void ui_set_alarms(const struct ui_alarm *alarms, uint32_t n);
bool ui_next_event(struct ui_event *e);
void ui_set_ringing(bool ringing, bool timer);
void ui_set_timer(uint32_t ms, uint32_t set_ms, bool running);
void ui_set_stopwatch(uint32_t ms, bool running);
void ui_set_laps(const uint32_t *ms, uint32_t n);
void ui_set_app(int app);
/* The app on page i of the strip a build without a launcher has below
 * the watchface, -1 past its end, and whether it takes sideways drags. */
int ui_page(unsigned i, bool *sideways);
void ui_set_weather(const char *city, uint32_t city_len, uint32_t days, const int32_t *ids, const int32_t *min_c,
		    const int32_t *max_c);
bool ui_modal(void);
/* The notification pop-up is up: the Cortex-M build draws it without
 * the screen under it, for the display to shade the mesh by 80 %. */
bool ui_popup(void);
void ui_touch(uint32_t panel, int kind, int x, int y);
/* When the touch handed to ui_touch next was reported, in ms. */
void ui_set_touch_ms(uint32_t ms);
void ui_tick(void);
/* How long until ui_tick has a timer to run, 0 while an animation runs. */
uint32_t ui_next_ms(void);
bool ui_update(uint32_t panel, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
void ui_render(uint32_t *buf, uint32_t stride_px, int dx, int dy);
/* Without layers (ui_init given none): ui_draw renders what changed on
 * that panel, or all of it if whole, straight to the display, between
 * ui_draw_begin with the box it covers and ui_draw_end, as runs of
 * RGB565 pixels through ui_draw_line, which the display provides; false
 * if nothing changed. Each run starts as ui_line_under fills it, and
 * ui_line_over draws over it once the UI has (flatmesh_mcu.c provides
 * both); ui_mark_dirty has the next ui_draw draw that box again too.
 * ui_set_shift moves the always-on face by (dx, dy). ui_heap_peak is the
 * most of the heap the UI has used. */
bool ui_draw(uint32_t panel, bool whole);
void ui_draw_begin(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void ui_draw_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *rgb565);
void ui_draw_end(void);
void ui_line_under(uint32_t y, uint32_t x, uint32_t w, uint16_t *rgb565);
void ui_line_over(uint32_t y, uint32_t x, uint32_t w, uint16_t *rgb565);
void ui_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void ui_set_shift(int dx, int dy);
uint32_t ui_heap_peak(void);
void ui_repaint(uint32_t panel, bool gpu);
void ui_panic(const char *msg, size_t len) __attribute__((noreturn));
#endif
