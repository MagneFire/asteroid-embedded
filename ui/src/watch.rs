// Slint platform on the firmware: every screen (the watchface, the screens beside it and the
// overlay) is its own Slint window rendered into its own persistent premultiplied RGBA layer, only
// where it changed and on the GPU while it is on; the caller places and composites the layers on
// the GPU. All screens get the same state through their Face global.
use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell, UnsafeCell};
use core::time::Duration;
use crate::paint::GpuLayer;
use slint::platform::software_renderer::{MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType};
use slint::platform::{Platform, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, LogicalPosition, Model, PhysicalSize, PlatformError, VecModel};

slint::include_modules!();

unsafe extern "C" {
    fn time_ms() -> u32;
    fn dcache_clean_invalidate_range(addr: *const u8, size: u32);
}

const SCREENS: usize = 5;
const OVERLAY: usize = 4;

struct Static<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Static<T> {}

static WINDOWS: Static<RefCell<Vec<Rc<MinimalSoftwareWindow>>>> = Static(UnsafeCell::new(RefCell::new(Vec::new())));
static UI: Static<Option<Ui>> = Static(UnsafeCell::new(None));

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

struct Ui {
    screens: Vec<Screen>,
    panels: Vec<Panel>,
    notes: Rc<VecModel<Note>>,
    forecast: Rc<VecModel<Day>>,
    opened: Queue,
    dismissed: Queue,
    actions: Queue,
    stride: usize,
    width: usize,
    height: usize,
}

fn ui() -> Option<&'static mut Ui> {
    unsafe { (*UI.0.get()).as_mut() }
}

fn each(f: impl Fn(Face)) {
    let Some(ui) = ui() else { return };
    for screen in &ui.screens {
        f(screen.global::<Face>());
    }
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
    if ui().is_some()
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
    let (stride, width, height) = (stride_px as usize, width as usize, height as usize);
    let notes = Rc::new(VecModel::default());
    let forecast = Rc::new(VecModel::default());
    let (opened, dismissed, actions): (Queue, Queue, Queue) = Default::default();
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
        let queue = opened.clone();
        face.on_notification_opened(move |id| queue.borrow_mut().push(id));
        let queue = dismissed.clone();
        face.on_notification_dismissed(move |id| queue.borrow_mut().push(id));
        let queue = actions.clone();
        face.on_media_action(move |action| queue.borrow_mut().push(action));
        let layer = unsafe { layers.add(kind * panel_px as usize) };
        let Some(panel) = open(&screen, layer, stride, width, height) else {
            return false;
        };
        screens.push(screen);
        panels.push(panel);
    }
    unsafe {
        *UI.0.get() =
            Some(Ui { screens, panels, notes, forecast, opened, dismissed, actions, stride, width, height });
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_clock(hour: u32, minute: u32, second: u32, weekday: u32, day: u32, month: u32) {
    each(|face| {
        face.set_hour(hour.min(23) as i32);
        face.set_minute(minute.min(59) as i32);
        face.set_second(second.min(59) as i32);
        face.set_weekday(weekday.min(6) as i32);
        face.set_day(day.clamp(1, 31) as i32);
        face.set_month(month.clamp(1, 12) as i32);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_battery(percent: u32, charging: bool) {
    each(|face| {
        face.set_battery_percent(percent.min(100) as i32);
        face.set_charging(charging);
    });
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
    let Some(ui) = ui() else { return };
    ui.notes.set_vec(Vec::new());
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
) {
    let Some(ui) = ui() else { return };
    ui.notes.push(Note {
        id,
        icon: note_icon(unsafe { text(icon, icon_len) }.as_str()),
        app: unsafe { text(app, app_len) },
        summary: unsafe { text(summary, summary_len) },
        body: unsafe { text(body, body_len) },
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_show_notification(id: i32) {
    let Some(ui) = ui() else { return };
    let Some(note) = ui.notes.iter().find(|n| n.id == id) else { return };
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
pub unsafe extern "C" fn ui_notification_dismissed(id: *mut i32) -> bool {
    let Some(ui) = ui() else { return false };
    let Some(first) = pop(&ui.dismissed) else { return false };
    if !id.is_null() {
        unsafe { *id = first };
    }
    true
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
pub extern "C" fn ui_media_action() -> i32 {
    let Some(ui) = ui() else { return -1 };
    pop(&ui.actions).unwrap_or(-1)
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
    let Some(ui) = ui() else { return };
    let n = if ids.is_null() || min_c.is_null() || max_c.is_null() { 0 } else { days as usize };
    let day = |i: usize| unsafe { Day { id: *ids.add(i), min: *min_c.add(i), max: *max_c.add(i) } };
    let city = unsafe { text(city, city_len) };
    let today = if n > 0 { day(0) } else { Day::default() };
    each(|face| {
        face.set_weather_valid(n > 0);
        face.set_weather_city(city.clone());
        face.set_weather_today(today.clone());
    });
    ui.forecast.set_vec((1..n).map(day).collect::<Vec<_>>());
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_modal() -> bool {
    let Some(ui) = ui() else { return false };
    let face = ui.screens[OVERLAY].global::<Face>();
    face.get_notification_shown() || face.get_passkey() >= 0
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_touch(panel: u32, kind: i32, x: i32, y: i32) {
    let Some(ui) = ui() else { return };
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
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_tick() {
    let Some(ui) = ui() else { return };
    while let Some(id) = pop(&ui.opened) {
        ui_show_notification(id);
    }
    let face = ui.screens[OVERLAY].global::<Face>();
    let id = face.get_notification_id();
    if face.get_notification_shown() && !ui.notes.iter().any(|n| n.id == id) {
        face.set_notification_shown(false);
    }
    if !face.get_notification_shown() {
        ui_hide_notification();
    }
    slint::platform::update_timers_and_animations();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_update(panel: u32, x: *mut u32, y: *mut u32, w: *mut u32, h: *mut u32) -> bool {
    let Some(ui) = ui() else { return false };
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
}

#[inline(always)]
fn over(src: u32, dst: u32, keep: u32) -> u32 {
    let v = dst * keep + 128;
    src + ((v + (v >> 8)) >> 8)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_render(buf: *mut u32, stride_px: u32) {
    let Some(ui) = ui() else { return };
    let stride = stride_px as usize;
    if buf.is_null() || stride < ui.width {
        return;
    }
    let fb = unsafe { core::slice::from_raw_parts_mut(buf, stride * ui.height) };
    for layer in [&ui.panels[0].layer, &ui.panels[OVERLAY].layer] {
        for y in 0..ui.height {
            let row = &mut fb[y * stride..y * stride + ui.width];
            let line = &layer[y * ui.stride..y * ui.stride + ui.width];
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
}
