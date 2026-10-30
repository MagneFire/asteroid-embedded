// Slint platform on the firmware: the UI is rendered into a persistent premultiplied RGBA
// layer, only where it changed, and composited over the background by the caller. Touch
// moves past SWIPE_PX become a swipe for the screen stack (the press is cancelled so no
// click fires), except vertical drags on a screen that scrolls.
use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell, UnsafeCell};
use core::time::Duration;
use slint::platform::software_renderer::{MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType};
use slint::platform::{Platform, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, LogicalPosition, Model, PhysicalSize, PlatformError, VecModel};

slint::include_modules!();

unsafe extern "C" {
    fn time_ms() -> u32;
}

struct Firmware {
    window: Rc<MinimalSoftwareWindow>,
    last_ms: Cell<u32>,
    elapsed_ms: Cell<u64>,
}

impl Platform for Firmware {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        let now = unsafe { time_ms() };
        let elapsed = self.elapsed_ms.get() + now.wrapping_sub(self.last_ms.get()) as u64;
        self.last_ms.set(now);
        self.elapsed_ms.set(elapsed);
        Duration::from_millis(elapsed)
    }
}

const SWIPE_PX: i32 = 40;
const FLICK_MS: u32 = 300;

#[derive(Clone, Copy, PartialEq)]
enum Gesture {
    Idle,
    Pending,
    Scroll,
    Swipe,
}

struct Ui {
    window: Rc<MinimalSoftwareWindow>,
    face: Watchface,
    layer: &'static mut [PremultipliedRgbaColor],
    stride: usize,
    width: usize,
    height: usize,
    notes: Rc<VecModel<Note>>,
    forecast: Rc<VecModel<Day>>,
    dismissed: Rc<RefCell<Vec<i32>>>,
    actions: Rc<RefCell<Vec<i32>>>,
    gesture: Gesture,
    down: (i32, i32),
    down_ms: u32,
    horizontal: bool,
    screen: i32,
    full_until: u32,
}

struct Global(UnsafeCell<Option<Ui>>);
unsafe impl Sync for Global {}
static UI: Global = Global(UnsafeCell::new(None));

fn ui() -> Option<&'static mut Ui> {
    unsafe { (*UI.0.get()).as_mut() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_init(
    heap: *mut u8,
    heap_size: u32,
    layer: *mut u32,
    stride_px: u32,
    width: u32,
    height: u32,
) -> bool {
    if ui().is_some()
        || heap.is_null()
        || heap_size == 0
        || layer.is_null()
        || width == 0
        || height == 0
        || stride_px < width
    {
        return false;
    }
    unsafe { crate::heap_init(heap, heap_size as usize) };
    let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
    let platform = Firmware {
        window: window.clone(),
        last_ms: Cell::new(unsafe { time_ms() }),
        elapsed_ms: Cell::new(0),
    };
    if slint::platform::set_platform(Box::new(platform)).is_err() {
        return false;
    }
    window.set_size(PhysicalSize::new(width, height));
    let Ok(face) = Watchface::new() else {
        return false;
    };
    let notes = Rc::new(VecModel::default());
    let forecast = Rc::new(VecModel::default());
    face.set_notes(notes.clone().into());
    face.set_weather_forecast(forecast.clone().into());
    let dismissed = Rc::new(RefCell::new(Vec::new()));
    let actions = Rc::new(RefCell::new(Vec::new()));
    let queue = dismissed.clone();
    face.on_notification_dismissed(move |id| queue.borrow_mut().push(id));
    let queue = actions.clone();
    face.on_media_action(move |action| queue.borrow_mut().push(action));
    if face.show().is_err() {
        return false;
    }
    let layer = unsafe {
        core::slice::from_raw_parts_mut(
            layer as *mut PremultipliedRgbaColor,
            stride_px as usize * height as usize,
        )
    };
    layer.fill(PremultipliedRgbaColor::default());
    window.request_redraw();
    unsafe {
        *UI.0.get() = Some(Ui {
            window,
            face,
            layer,
            stride: stride_px as usize,
            width: width as usize,
            height: height as usize,
            notes,
            forecast,
            dismissed,
            actions,
            gesture: Gesture::Idle,
            down: (0, 0),
            down_ms: 0,
            horizontal: false,
            screen: 0,
            full_until: 0,
        });
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_clock(hour: u32, minute: u32, second: u32, weekday: u32, day: u32, month: u32) {
    let Some(ui) = ui() else { return };
    ui.face.set_hour(hour.min(23) as i32);
    ui.face.set_minute(minute.min(59) as i32);
    ui.face.set_second(second.min(59) as i32);
    ui.face.set_weekday(weekday.min(6) as i32);
    ui.face.set_day(day.clamp(1, 31) as i32);
    ui.face.set_month(month.clamp(1, 12) as i32);
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_battery(percent: u32, charging: bool) {
    let Some(ui) = ui() else { return };
    ui.face.set_battery_percent(percent.min(100) as i32);
    ui.face.set_charging(charging);
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_set_passkey(passkey: i32) {
    let Some(ui) = ui() else { return };
    ui.face.set_passkey(passkey);
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
    ui.face.set_notification_id(note.id);
    ui.face.set_notification_icon(note.icon);
    ui.face.set_notification_app(note.app);
    ui.face.set_notification_summary(note.summary);
    ui.face.set_notification_body(note.body);
    ui.face.set_notification_shown(true);
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_hide_notification() {
    let Some(ui) = ui() else { return };
    ui.face.set_notification_shown(false);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_notification_dismissed(id: *mut i32) -> bool {
    let Some(ui) = ui() else { return false };
    let mut queue = ui.dismissed.borrow_mut();
    if queue.is_empty() {
        return false;
    }
    let first = queue.remove(0);
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
    let Some(ui) = ui() else { return };
    ui.face.set_media_title(unsafe { text(title, title_len) });
    ui.face.set_media_artist(unsafe { text(artist, artist_len) });
    ui.face.set_media_album(unsafe { text(album, album_len) });
    ui.face.set_media_playing(playing);
    ui.face.set_media_volume(volume.clamp(-1, 100));
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_media_action() -> i32 {
    let Some(ui) = ui() else { return -1 };
    let mut queue = ui.actions.borrow_mut();
    if queue.is_empty() { -1 } else { queue.remove(0) }
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
    ui.face.set_weather_valid(n > 0);
    ui.face.set_weather_city(unsafe { text(city, city_len) });
    ui.face.set_weather_today(if n > 0 { day(0) } else { Day::default() });
    ui.forecast.set_vec((1..n).map(day).collect::<Vec<_>>());
}

#[unsafe(no_mangle)]
pub extern "C" fn ui_home() {
    let Some(ui) = ui() else { return };
    ui.face.invoke_home();
    ui.gesture = Gesture::Idle;
}

fn drag(ui: &Ui, dx: i32, dy: i32) {
    let (dx, dy) = if ui.horizontal { (dx, 0) } else { (0, dy) };
    ui.face.invoke_drag(dx as f32, dy as f32);
}

fn flick(ui: &Ui) -> bool {
    unsafe { time_ms() }.wrapping_sub(ui.down_ms) < FLICK_MS
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_scroll(x: *mut f32, y: *mut f32) {
    let Some(ui) = ui() else { return };
    unsafe {
        *x = ui.face.get_scroll_x();
        *y = ui.face.get_scroll_y();
    }
}


#[unsafe(no_mangle)]
pub extern "C" fn ui_touch(kind: i32, x: i32, y: i32) {
    let Some(ui) = ui() else { return };
    let position = LogicalPosition::new(x as f32, y as f32);
    let button = PointerEventButton::Left;
    let window = ui.window.window();
    let dispatch = |event| {
        let _ = window.dispatch_event_with_result(event);
    };
    let cancel = || {
        let away = LogicalPosition::new(-10000.0, -10000.0);
        dispatch(WindowEvent::PointerReleased { position: away, button });
        dispatch(WindowEvent::PointerExited);
    };
    let (dx, dy) = (x - ui.down.0, y - ui.down.1);
    let far = dx * dx + dy * dy > SWIPE_PX * SWIPE_PX;
    let scrolls = dy.abs() >= dx.abs() && ui.face.invoke_scrolls_vertically();
    match kind {
        0 => {
            ui.gesture = Gesture::Pending;
            ui.down = (x, y);
            ui.down_ms = unsafe { time_ms() };
            dispatch(WindowEvent::PointerPressed { position, button });
        }
        1 => match ui.gesture {
            Gesture::Pending if far && !scrolls => {
                ui.gesture = Gesture::Swipe;
                ui.horizontal = dx.abs() >= dy.abs();
                cancel();
                drag(ui, dx, dy);
            }
            Gesture::Swipe => drag(ui, dx, dy),
            Gesture::Pending if far => {
                ui.gesture = Gesture::Scroll;
                dispatch(WindowEvent::PointerMoved { position });
            }
            Gesture::Pending | Gesture::Scroll => dispatch(WindowEvent::PointerMoved { position }),
            _ => {}
        },
        2 => {
            match ui.gesture {
                Gesture::Swipe => {
                    drag(ui, dx, dy);
                    ui.face.invoke_release(flick(ui));
                }
                Gesture::Pending if far && !scrolls => {
                    cancel();
                    ui.horizontal = dx.abs() >= dy.abs();
                    drag(ui, dx, dy);
                    ui.face.invoke_release(true);
                }
                Gesture::Pending | Gesture::Scroll => {
                    dispatch(WindowEvent::PointerReleased { position, button });
                    dispatch(WindowEvent::PointerExited);
                }
                Gesture::Idle => {}
            }
            ui.gesture = Gesture::Idle;
        }
        _ => {}
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_update(x: *mut u32, y: *mut u32, w: *mut u32, h: *mut u32) -> bool {
    let Some(ui) = ui() else { return false };
    if ui.face.get_notification_shown() {
        let id = ui.face.get_notification_id();
        if !ui.notes.iter().any(|n| n.id == id) {
            ui.face.set_notification_shown(false);
        }
    }
    slint::platform::update_timers_and_animations();
    let now = unsafe { time_ms() };
    if ui.face.get_screen() != ui.screen {
        ui.screen = ui.face.get_screen();
        ui.full_until = now.wrapping_add(400);
    }
    let full = (ui.full_until.wrapping_sub(now) as i32) > 0;
    let mut region = None;
    let (layer, stride) = (&mut *ui.layer, ui.stride);
    ui.window.draw_if_needed(|renderer| {
        renderer.set_repaint_buffer_type(if full { RepaintBufferType::NewBuffer } else { RepaintBufferType::ReusedBuffer });
        region = Some(renderer.render(layer, stride))
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
    for y in 0..ui.height {
        let row = &mut fb[y * stride..y * stride + ui.width];
        let line = &ui.layer[y * ui.stride..y * ui.stride + ui.width];
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
