// Slint platform on the firmware: every screen (the watchface, the screens beside it and the
// overlay) is its own Slint window rendered into its own persistent premultiplied RGBA layer, only
// where it changed and on the GPU while it is on; the caller places and composites the layers on
// the GPU. All screens get the same state through their Face global.
use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell, UnsafeCell};
use core::time::Duration;
use crate::paint::{self, GpuLayer};
use slint::platform::software_renderer::{MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType};
use slint::platform::{Platform, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, LogicalPosition, Model, PhysicalSize, PlatformError, VecModel};

slint::include_modules!();

unsafe extern "C" {
    fn time_ms() -> u32;
    fn dcache_clean_invalidate_range(addr: *const u8, size: u32);
}

const SCREENS: usize = 7;
const NOTES: usize = 1;
const LAUNCHER: usize = 3;
const OVERLAY: usize = 4;
const APP: usize = 6;

struct Static<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Static<T> {}

static WINDOWS: Static<RefCell<Vec<Rc<MinimalSoftwareWindow>>>> = Static(UnsafeCell::new(RefCell::new(Vec::new())));
static UI: Static<Option<Ui>> = Static(UnsafeCell::new(None));

/// The fonts text is drawn with, scaled to whatever size it asks for, registered in the font
/// context all windows share before any text is laid out.
static FONTS: [&[u8]; 11] = [
    include_bytes!("../fonts/NotoSans-Light.ttf"),
    include_bytes!("../fonts/NotoSans-Bold.ttf"),
    include_bytes!("../fonts/NotoSans-SemiCondensedLight.ttf"),
    include_bytes!("../fonts/NotoSans-SemiCondensedExtraLight.ttf"),
    include_bytes!("../fonts/NotoSans-SemiCondensedSemiBold.ttf"),
    include_bytes!("../fonts/NotoSans-SemiCondensedBold.ttf"),
    include_bytes!("../fonts/NotoSans-CondensedMedium.ttf"),
    include_bytes!("../fonts/NotoSans-ExtraCondensedExtraLight.ttf"),
    include_bytes!("../fonts/Roboto-Medium.ttf"),
    include_bytes!("../fonts/Roboto-Light.ttf"),
    include_bytes!("../fonts/Raleway-Light.ttf"),
];

struct Firmware {
    last_ms: Cell<u32>,
    elapsed_ms: Cell<u64>,
}

impl Platform for Firmware {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
        unsafe { &*WINDOWS.0.get() }.borrow_mut().push(window.clone());
        Ok(window)
    }

    fn duration_since_start(&self) -> Duration {
        let now = unsafe { time_ms() };
        let elapsed = self.elapsed_ms.get() + now.wrapping_sub(self.last_ms.get()) as u64;
        self.last_ms.set(now);
        self.elapsed_ms.set(elapsed);
        Duration::from_millis(elapsed)
    }
}

struct Panel {
    window: Rc<MinimalSoftwareWindow>,
    layer: &'static mut [PremultipliedRgbaColor],
}

type Queue = Rc<RefCell<Vec<i32>>>;

/// The types shared with C, laid out as ui.h declares them.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiAlarm {
    hour: u8,
    minute: u8,
    days: u8,
    on: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiRgb {
    r: u8,
    g: u8,
    b: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiColors {
    center: UiRgb,
    outer: UiRgb,
}

/// The settings as the firmware keeps them: brightnesses in per cent, the nightstand delay in
/// seconds, burn_in 0 off, 1 high, 2 medium, 3 low.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiSettings {
    brightness: u8,
    nightstand_brightness: u8,
    nightstand_delay: u8,
    burn_in: u8,
    bluetooth: bool,
    vibration: bool,
    aod: bool,
    clock12: bool,
    tilt: bool,
    tap: bool,
    fahrenheit: bool,
    nightstand_aod: bool,
}

/// Something the user did, for the firmware to act on: kind is one of the EVENT_* values, value
/// the setting, action, app, notification or alarm index it is about, and alarm the alarm as
/// edited, or for a snoozed notification how long in its hour and minute.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct UiEvent {
    kind: u32,
    value: i32,
    arg: i32,
    alarm: UiAlarm,
}

mod codes {
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/codes.rs"));
}
use codes::*;

const _: () = assert!(core::mem::size_of::<UiAlarm>() == 4 && core::mem::size_of::<UiColors>() == 6);
const _: () = assert!(core::mem::size_of::<UiEvent>() == 16 && core::mem::size_of::<UiSettings>() == 12);

type Events = Rc<RefCell<VecDeque<UiEvent>>>;

fn event(events: &Events, kind: u32, value: i32) {
    events.borrow_mut().push_back(UiEvent { kind, value, arg: 0, alarm: UiAlarm::default() });
}

fn rgb(c: slint::Color) -> UiRgb {
    UiRgb { r: c.red(), g: c.green(), b: c.blue() }
}

struct Ui {
    screens: Vec<Screen>,
    panels: Vec<Panel>,
    notes: Rc<VecModel<Note>>,
    forecast: Rc<VecModel<Day>>,
    alarms: Rc<VecModel<AlarmItem>>,
    opened: Queue,
    events: Events,
    stride: usize,
    width: usize,
    height: usize,
}

static BUSY: Static<Cell<bool>> = Static(UnsafeCell::new(Cell::new(false)));

/// The UI, for the length of f, to one caller at a time: the ui_* functions run from the main
/// loop only, and one that called another while holding the UI would hand out a second &mut to
/// it, so that is a panic (a fault record) instead.
fn with_ui<R>(f: impl FnOnce(&mut Ui) -> R) -> Option<R> {
    let busy = unsafe { &*BUSY.0.get() };
    assert!(!busy.replace(true), "ui re-entered");
    let r = unsafe { (*UI.0.get()).as_mut() }.map(f);
    busy.set(false);
    r
}

fn each(f: impl Fn(Face)) {
    with_ui(|ui| {
        for screen in &ui.screens {
            f(screen.global::<Face>());
        }
    });
}

fn pop(queue: &Queue) -> Option<i32> {
    let mut queue = queue.borrow_mut();
    if queue.is_empty() { None } else { Some(queue.remove(0)) }
}

fn open(component: &impl ComponentHandle, layer: *mut u32, stride: usize, width: usize, height: usize) -> Option<Panel> {
    let window = unsafe { &*WINDOWS.0.get() }.borrow_mut().pop()?;
    window.set_size(PhysicalSize::new(width as u32, height as u32));
    component.show().ok()?;
    let layer = unsafe { core::slice::from_raw_parts_mut(layer as *mut PremultipliedRgbaColor, stride * height) };
    layer.fill(PremultipliedRgbaColor::default());
    unsafe { dcache_clean_invalidate_range(layer.as_ptr() as *const u8, (layer.len() * 4) as u32) };
    window.request_redraw();
    Some(Panel { window, layer })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_init(
    heap: *mut u8,
    heap_size: u32,
    layers: *mut u32,
    panel_px: u32,
    stride_px: u32,
    width: u32,
    height: u32,
) -> bool {
    if with_ui(|_| ()).is_some()
        || heap.is_null()
        || heap_size == 0
        || layers.is_null()
        || width == 0
        || height == 0
        || stride_px < width
        || panel_px < stride_px * height
    {
        return false;
    }
    unsafe { crate::heap_init(heap, heap_size as usize) };
    let platform = Firmware { last_ms: Cell::new(unsafe { time_ms() }), elapsed_ms: Cell::new(0) };
    if slint::platform::set_platform(Box::new(platform)).is_err() {
        return false;
    }
    let fonts = i_slint_core::context::with_global_context(
        || Err(PlatformError::NoPlatform),
        |ctx| FONTS.iter().for_each(|font| ctx.font_context().borrow_mut().register_static_font(font)),
    );
    if fonts.is_err() {
        return false;
    }
    let (stride, width, height) = (stride_px as usize, width as usize, height as usize);
    let notes = Rc::new(VecModel::default());
    let forecast = Rc::new(VecModel::default());
    let alarms = Rc::new(VecModel::default());
    let (opened, events): (Queue, Events) = Default::default();
    let mut screens = Vec::new();
    let mut panels = Vec::new();
    for kind in 0..SCREENS {
        let Ok(screen) = Screen::new() else {
            return false;
        };
        screen.set_kind(kind as i32);
        let face = screen.global::<Face>();
        face.set_notes(notes.clone().into());
        face.set_weather_forecast(forecast.clone().into());
        face.set_alarms(alarms.clone().into());
        let queue = events.clone();
        face.on_alarm_saved(move |index, hour, minute, days, on| {
            let alarm = UiAlarm { hour: hour as u8, minute: minute as u8, days: days as u8, on };
            queue.borrow_mut().push_back(UiEvent { kind: UI_EVENT_ALARM_EDIT, value: index, arg: 0, alarm });
        });
        let queue = events.clone();
        face.on_alarm_deleted(move |index| event(&queue, UI_EVENT_ALARM_DELETE, index));
        let queue = events.clone();
        face.on_alarm_action(move |action| event(&queue, UI_EVENT_ALARM_RING, action));
        let queue = events.clone();
        face.on_clock_action(move |action| event(&queue, UI_EVENT_CLOCK, action));
        let queue = opened.clone();
        face.on_notification_opened(move |id| queue.borrow_mut().push(id));
        let queue = events.clone();
        face.on_notification_dismissed(move |id| event(&queue, UI_EVENT_DISMISS, id));
        let queue = events.clone();
        face.on_notifications_cleared(move || event(&queue, UI_EVENT_DISMISS_ALL, 0));
        let queue = events.clone();
        face.on_notification_snoozed(move |id, minutes| {
            let alarm = UiAlarm { hour: (minutes / 60) as u8, minute: (minutes % 60) as u8, ..Default::default() };
            queue.borrow_mut().push_back(UiEvent { kind: UI_EVENT_SNOOZE, value: id, arg: 0, alarm });
        });
        let queue = events.clone();
        face.on_media_action(move |action| event(&queue, UI_EVENT_MEDIA, action));
        let queue = events.clone();
        face.on_app_opened(move |app| event(&queue, UI_EVENT_APP, app));
        let queue = events.clone();
        face.on_setting_toggled(move |setting| event(&queue, UI_EVENT_SETTING, setting));
        let queue = events.clone();
        face.on_timer_set(move |seconds| event(&queue, UI_EVENT_TIMER_SET, seconds));
        let queue = events.clone();
        face.on_brightness_set(move |percent| event(&queue, UI_EVENT_BRIGHTNESS, percent));
        let queue = events.clone();
        face.on_setting_set(move |setting, value| {
            queue.borrow_mut().push_back(UiEvent { kind: UI_EVENT_SETTING_VALUE, value: setting, arg: value, alarm: UiAlarm::default() })
        });
        let queue = events.clone();
        face.on_time_set(move |minutes| event(&queue, UI_EVENT_TIME_SET, minutes));
        let queue = events.clone();
        face.on_calc_key(move |key| event(&queue, UI_EVENT_CALC, key));
        let queue = events.clone();
        face.on_date_set(move |day, month, year| event(&queue, UI_EVENT_DATE_SET, (year - 2000) << 9 | month << 5 | day));
        let layer = unsafe { layers.add(kind * panel_px as usize) };
        let Some(panel) = open(&screen, layer, stride, width, height) else {
            return false;
        };
        screens.push(screen);
        panels.push(panel);
    }
    unsafe {
        *UI.0.get() =
            Some(Ui { screens, panels, notes, forecast, alarms, opened, events, stride, width, height });
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_clock(hour: u32, minute: u32, second: u32, weekday: u32, day: u32, month: u32, year: u32, rtc: u32) {
    each(|face| {
        face.set_now(rtc as i32);
        face.set_hour(hour.min(23) as i32);
        face.set_minute(minute.min(59) as i32);
        face.set_second(second.min(59) as i32);
        face.set_weekday(weekday.min(6) as i32);
        face.set_day(day.clamp(1, 31) as i32);
        face.set_month(month.clamp(1, 12) as i32);
        face.set_year(year.clamp(2000, 2099) as i32);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_connected(connected: bool, bonded: bool) {
    each(|face| {
        face.set_bluetooth_connected(connected);
        face.set_bluetooth_bonded(bonded);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_heart_rate(bpm: u32) {
    each(|face| face.set_heart_rate(bpm as i32));
}

/// The app on the app panel, -1 for none.
#[unsafe(no_mangle)]
pub extern "C" fn ui_app_open() -> i32 {
    with_ui(|ui| ui.screens[APP].global::<Face>().get_app()).unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_calc(shown: *const u8, shown_len: u32, length: u32) {
    let shown = unsafe { text(shown, shown_len) };
    with_ui(|ui| {
        let face = ui.screens[APP].global::<Face>();
        face.set_calc_shown(shown);
        face.set_calc_length(length as i32);
    });
}

/// Whether the Flashlight app has its light on.
#[unsafe(no_mangle)]
pub extern "C" fn ui_flashlight_on() -> bool {
    with_ui(|ui| ui.screens[APP].global::<Face>().get_flashlight_on()).unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_battery(percent: u32, charging: bool) {
    each(|face| {
        face.set_battery_percent(percent.min(100) as i32);
        face.set_charging(charging);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_ambient(ambient: bool) {
    each(|face| face.set_ambient(ambient));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_passkey(passkey: i32) {
    each(|face| face.set_passkey(passkey));
}

unsafe fn text(p: *const u8, len: u32) -> slint::SharedString {
    if p.is_null() {
        return slint::SharedString::new();
    }
    let bytes = unsafe { core::slice::from_raw_parts(p, len as usize) };
    match core::str::from_utf8(bytes) {
        Ok(s) => s.into(),
        Err(e) => core::str::from_utf8(&bytes[..e.valid_up_to()]).unwrap_or("").into(),
    }
}

include!(concat!(env!("OUT_DIR"), "/note_icons.rs"));

fn note_icon(name: &str) -> i32 {
    NOTE_ICON_NAMES.binary_search(&name).map_or(-1, |i| i as i32)
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_clear_notifications() {
    with_ui(|ui| ui.notes.set_vec(Vec::new()));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_add_notification(
    id: i32,
    app: *const u8,
    app_len: u32,
    summary: *const u8,
    summary_len: u32,
    body: *const u8,
    body_len: u32,
    icon: *const u8,
    icon_len: u32,
    time: u32,
) {
    let note = Note {
        id,
        icon: note_icon(unsafe { text(icon, icon_len) }.as_str()),
        app: unsafe { text(app, app_len) },
        summary: unsafe { text(summary, summary_len) },
        body: unsafe { text(body, body_len) },
        time: time as i32,
    };
    with_ui(|ui| ui.notes.push(note));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_show_notification(id: i32) {
    let Some(note) = with_ui(|ui| ui.notes.iter().find(|n| n.id == id)).flatten() else { return };
    each(|face| {
        face.set_notification_id(note.id);
        face.set_notification_icon(note.icon);
        face.set_notification_app(note.app.clone());
        face.set_notification_summary(note.summary.clone());
        face.set_notification_body(note.body.clone());
        face.set_notification_shown(true);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_hide_notification() {
    each(|face| face.set_notification_shown(false));
}


#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_media(
    title: *const u8,
    title_len: u32,
    artist: *const u8,
    artist_len: u32,
    album: *const u8,
    album_len: u32,
    playing: bool,
    volume: i32,
) {
    let (title, artist, album) = unsafe { (text(title, title_len), text(artist, artist_len), text(album, album_len)) };
    each(|face| {
        face.set_media_title(title.clone());
        face.set_media_artist(artist.clone());
        face.set_media_album(album.clone());
        face.set_media_playing(playing);
        face.set_media_volume(volume.clamp(-1, 100));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_launcher(layout: i32) {
    each(|face| face.set_launcher(layout));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_notes_edges() -> u32 {
    with_ui(|ui| ui.screens[NOTES].global::<Face>().get_notes_edges() as u32).unwrap_or(2 | 16)
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_launcher_edges() -> u32 {
    with_ui(|ui| ui.screens[LAUNCHER].global::<Face>().get_launcher_edges() as u32).unwrap_or(4)
}

/// Which panel the stack has on screen, under any app.
#[unsafe(no_mangle)]
pub extern "C" fn ui_set_screen(panel: u32) {
    each(|face| face.set_screen(panel as i32));
}

/// The mesh colours of the launcher page on screen, blended between the two pages it is between;
/// false for the layouts that keep the mesh grey.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_launcher_colors(out: *mut UiColors) -> bool {
    with_ui(|ui| {
        let screen = &ui.screens[LAUNCHER];
        let apps = screen.global::<Apps>().get_list();
        let n = apps.row_count();
        let layout = screen.global::<Face>().get_launcher();
        if n == 0 || out.is_null() || layout == 4 || layout == 5 {
            return false;
        }
        let pos = screen.global::<Face>().get_launcher_position().clamp(0.0, (n - 1) as f32);
        let (i, t) = (pos as usize, pos - (pos as usize) as f32);
        let a = apps.row_data(i).unwrap();
        let b = apps.row_data((i + 1).min(n - 1)).unwrap();
        let mix = |x: slint::Color, y: slint::Color| {
            let c = |p: u8, q: u8| (p as f32 * (1.0 - t) + q as f32 * t) as u8;
            UiRgb { r: c(x.red(), y.red()), g: c(x.green(), y.green()), b: c(x.blue(), y.blue()) }
        };
        unsafe { *out = UiColors { center: mix(a.center, b.center), outer: mix(a.outer, b.outer) } };
        true
    }).unwrap_or(false)
}

/// The background colours of an app; false for an unknown app.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_app_colors(app: i32, out: *mut UiColors) -> bool {
    with_ui(|ui| {
        let apps = ui.screens[LAUNCHER].global::<Apps>().get_list();
        let Some(a) = apps.iter().find(|a| a.id == app) else { return false };
        if out.is_null() {
            return false;
        }
        unsafe { *out = UiColors { center: rgb(a.center), outer: rgb(a.outer) } };
        true
    }).unwrap_or(false)
}


#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_settings(settings: *const UiSettings) {
    if settings.is_null() {
        return;
    }
    let s = unsafe { *settings };
    each(|face| {
        face.set_brightness(s.brightness.min(100) as i32);
        face.set_bluetooth_on(s.bluetooth);
        face.set_vibration_on(s.vibration);
        face.set_aod_on(s.aod);
        face.set_clock12(s.clock12);
        face.set_tilt_on(s.tilt);
        face.set_tap_on(s.tap);
        face.set_fahrenheit(s.fahrenheit);
        face.set_nightstand_brightness(s.nightstand_brightness as i32);
        face.set_nightstand_delay(s.nightstand_delay as i32);
        face.set_nightstand_aod(s.nightstand_aod);
        face.set_burn_in(s.burn_in as i32);
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_build(build: *const u8) {
    let len = if build.is_null() { 0 } else { unsafe { core::ffi::CStr::from_ptr(build as *const core::ffi::c_char) }.count_bytes() };
    let build = unsafe { text(build, len as u32) };
    each(|face| face.set_build(build.clone()));
}

/// The Bluetooth address as text and the seconds since boot, for About.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_info(bt_address: *const u8, uptime: u32) {
    let len = if bt_address.is_null() { 0 } else { unsafe { core::ffi::CStr::from_ptr(bt_address as *const core::ffi::c_char) }.count_bytes() };
    let address = unsafe { text(bt_address, len as u32) };
    each(|face| {
        face.set_bt_address(address.clone());
        face.set_uptime(uptime as i32);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_face(watchface: i32, nightstand_setting: bool, nightstand: bool) {
    each(|face| {
        face.set_watchface(watchface);
        face.set_nightstand_on(nightstand_setting);
        face.set_nightstand(nightstand);
    });
}


#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_alarms(alarms: *const UiAlarm, n: u32) {
    let alarms = if alarms.is_null() { &[][..] } else { unsafe { core::slice::from_raw_parts(alarms, n as usize) } };
    let items = alarms
        .iter()
        .map(|a| AlarmItem { hour: a.hour as i32, minute: a.minute as i32, days: a.days as i32, on: a.on })
        .collect::<Vec<_>>();
    with_ui(|ui| ui.alarms.set_vec(items));
}


/// The next thing the user did, oldest first; false when there is none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_next_event(out: *mut UiEvent) -> bool {
    with_ui(|ui| {
        let Some(e) = ui.events.borrow_mut().pop_front() else { return false };
        if !out.is_null() {
            unsafe { *out = e };
        }
        true
    }).unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_ringing(ringing: bool, timer: bool) {
    each(|face| {
        face.set_ringing_timer(timer);
        face.set_alarm_ringing(ringing);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_timer(ms: u32, set_ms: u32, running: bool) {
    each(|face| {
        face.set_timer_ms(ms as i32);
        face.set_timer_set_ms(set_ms as i32);
        face.set_timer_running(running);
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_laps(ms: *const u32, n: u32) {
    let laps = if ms.is_null() { &[][..] } else { unsafe { core::slice::from_raw_parts(ms, n as usize) } };
    let laps: Vec<i32> = laps.iter().map(|&m| m as i32).collect();
    each(|face| face.set_stopwatch_laps(Rc::new(VecModel::from(laps.clone())).into()));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_stopwatch(ms: u32, running: bool) {
    each(|face| {
        face.set_stopwatch_ms(ms as i32);
        face.set_stopwatch_running(running);
    });
}



#[unsafe(no_mangle)]
pub extern "C" fn ui_set_app(app: i32) {
    each(|face| {
        face.set_app(app);
        face.set_app_depth(0);
    });
}

/// How many pages deep the open app is.
#[unsafe(no_mangle)]
pub extern "C" fn ui_app_depth() -> i32 {
    with_ui(|ui| ui.screens[APP].global::<Face>().get_app_depth()).unwrap_or(0)
}

/// Drags the open app's top page px to the right, showing the page below.
#[unsafe(no_mangle)]
pub extern "C" fn ui_app_peek(px: i32) {
    with_ui(|ui| {
        let face = ui.screens[APP].global::<Face>();
        face.set_peeking(true);
        face.set_peek(px as f32);
    });
}

/// Ends the drag, taking the open app one page back if back; the app resets the peek as it
/// leaves the page, so the page is never drawn back in place.
#[unsafe(no_mangle)]
pub extern "C" fn ui_app_peek_end(back: bool) {
    with_ui(|ui| {
        let face = ui.screens[APP].global::<Face>();
        if back && face.get_app_depth() > 0 {
            face.set_back_count(face.get_back_count() + 1);
        } else {
            face.set_peek(0.0);
            face.set_peeking(false);
        }
    });
}


#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_set_weather(
    city: *const u8,
    city_len: u32,
    days: u32,
    ids: *const i32,
    min_c: *const i32,
    max_c: *const i32,
) {
    let n = if ids.is_null() || min_c.is_null() || max_c.is_null() { 0 } else { days as usize };
    let day = |i: usize| unsafe { Day { id: *ids.add(i), min: *min_c.add(i), max: *max_c.add(i) } };
    let city = unsafe { text(city, city_len) };
    let today = if n > 0 { day(0) } else { Day::default() };
    each(|face| {
        face.set_weather_valid(n > 0);
        face.set_weather_city(city.clone());
        face.set_weather_today(today.clone());
    });
    with_ui(|ui| ui.forecast.set_vec((1..n).map(day).collect::<Vec<_>>()));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_modal() -> bool {
    with_ui(|ui| {
        let face = ui.screens[OVERLAY].global::<Face>();
        face.get_notification_shown() || face.get_passkey() >= 0 || face.get_alarm_ringing()
    }).unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_touch(panel: u32, kind: i32, x: i32, y: i32) {
    with_ui(|ui| {
        let Some(panel) = ui.panels.get(panel as usize) else { return };
        let position = LogicalPosition::new(x as f32, y as f32);
        let button = PointerEventButton::Left;
        let window = panel.window.window();
        let dispatch = |event| {
            let _ = window.dispatch_event_with_result(event);
        };
        match kind {
            0 => dispatch(WindowEvent::PointerPressed { position, button }),
            1 => dispatch(WindowEvent::PointerMoved { position }),
            2 => {
                dispatch(WindowEvent::PointerReleased { position, button });
                dispatch(WindowEvent::PointerExited);
            }
            _ => {}
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_tick() {
    if with_ui(|_| ()).is_none() {
        return;
    }
    while let Some(id) = with_ui(|ui| pop(&ui.opened)).flatten() {
        ui_show_notification(id);
    }
    let hide = with_ui(|ui| {
        let face = ui.screens[OVERLAY].global::<Face>();
        let id = face.get_notification_id();
        if face.get_notification_shown() && !ui.notes.iter().any(|n| n.id == id) {
            face.set_notification_shown(false);
        }
        !face.get_notification_shown()
    });
    if hide == Some(true) {
        ui_hide_notification();
    }
    slint::platform::update_timers_and_animations();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_update(panel: u32, x: *mut u32, y: *mut u32, w: *mut u32, h: *mut u32) -> bool {
    with_ui(|ui| {
        let Some(panel) = ui.panels.get_mut(panel as usize) else { return false };
        let mut region = None;
        let (layer, stride, width, height) = (&mut *panel.layer, ui.stride, ui.width, ui.height);
        panel.window.draw_if_needed(|renderer| {
            region = Some(renderer.render_into_buffer(&mut GpuLayer::new(layer, stride, width, height)))
        });
        let Some(region) = region else { return false };
        let origin = region.bounding_box_origin();
        let size = region.bounding_box_size();
        let x0 = (origin.x.max(0) as usize).min(ui.width);
        let y0 = (origin.y.max(0) as usize).min(ui.height);
        let x1 = (x0 + size.width as usize).min(ui.width);
        let y1 = (y0 + size.height as usize).min(ui.height);
        if x1 == x0 || y1 == y0 {
            return false;
        }
        unsafe {
            for (p, v) in [(x, x0), (y, y0), (w, x1 - x0), (h, y1 - y0)] {
                if !p.is_null() {
                    *p = v as u32;
                }
            }
        }
        true
    }).unwrap_or(false)
}

#[inline(always)]
fn over(src: u32, dst: u32, keep: u32) -> u32 {
    let v = dst * keep + 128;
    src + ((v + (v >> 8)) >> 8)
}

/// Blends the watchface layer, moved by (dx, dy), and the overlay over buf.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_render(buf: *mut u32, stride_px: u32, dx: i32, dy: i32) {
    with_ui(|ui| {
        let stride = stride_px as usize;
        if buf.is_null() || stride < ui.width {
            return;
        }
        let fb = unsafe { core::slice::from_raw_parts_mut(buf, stride * ui.height) };
        let (w, h) = (ui.width as i32, ui.height as i32);
        for (layer, (dx, dy)) in [(&ui.panels[0].layer, (dx.clamp(-w, w), dy.clamp(-h, h))), (&ui.panels[OVERLAY].layer, (0, 0))] {
            let (x0, x1) = (dx.max(0) as usize, (w + dx).min(w) as usize);
            for y in dy.max(0) as usize..(h + dy).min(h) as usize {
                let sy = (y as i32 - dy) as usize;
                let sx = (x0 as i32 - dx) as usize;
                let row = &mut fb[y * stride + x0..y * stride + x1];
                let line = &layer[sy * ui.stride + sx..sy * ui.stride + sx + (x1 - x0)];
                for (d, s) in row.iter_mut().zip(line.iter()) {
                    match s.alpha {
                        0 => {}
                        255 => *d = (s.red as u32) << 16 | (s.green as u32) << 8 | s.blue as u32,
                        a => {
                            let keep = 255 - a as u32;
                            let r = over(s.red as u32, *d >> 16 & 0xff, keep);
                            let g = over(s.green as u32, *d >> 8 & 0xff, keep);
                            let b = over(s.blue as u32, *d & 0xff, keep);
                            *d = r.min(255) << 16 | g.min(255) << 8 | b.min(255);
                        }
                    }
                }
            }
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_repaint(panel: u32, gpu: bool) {
    with_ui(|ui| {
        let Some(panel) = ui.panels.get_mut(panel as usize) else { return };
        let (layer, stride, width, height) = (&mut *panel.layer, ui.stride, ui.width, ui.height);
        panel.window.request_redraw();
        panel.window.draw_if_needed(|renderer| {
            renderer.set_repaint_buffer_type(RepaintBufferType::NewBuffer);
            if gpu {
                renderer.render_into_buffer(&mut GpuLayer::new(layer, stride, width, height));
                paint::finish();
            } else {
                renderer.render(layer, stride);
            }
            renderer.set_repaint_buffer_type(RepaintBufferType::ReusedBuffer);
        });
    });
}
