// Slint platform on the firmware's framebuffer: the UI is rendered line by line and
// blended over whatever the framebuffer already holds.
use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec;
use alloc::vec::Vec;
use core::cell::{Cell, UnsafeCell};
use core::time::Duration;
use slint::platform::software_renderer::{
    LineBufferProvider, MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType,
};
use slint::platform::{Platform, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, LogicalPosition, PhysicalSize, PlatformError};

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

struct Ui {
    window: Rc<MinimalSoftwareWindow>,
    face: Watchface,
    line: Vec<PremultipliedRgbaColor>,
    width: usize,
    height: usize,
}

struct Global(UnsafeCell<Option<Ui>>);
unsafe impl Sync for Global {}
static UI: Global = Global(UnsafeCell::new(None));

fn ui() -> Option<&'static mut Ui> {
    unsafe { (*UI.0.get()).as_mut() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_init(heap: *mut u8, heap_size: u32, width: u32, height: u32) -> bool {
    if ui().is_some() || heap.is_null() || heap_size == 0 || width == 0 || height == 0 {
        return false;
    }
    unsafe { crate::heap_init(heap, heap_size as usize) };
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
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
    if face.show().is_err() {
        return false;
    }
    unsafe {
        *UI.0.get() = Some(Ui {
            window,
            face,
            line: vec![PremultipliedRgbaColor::default(); width as usize],
            width: width as usize,
            height: height as usize,
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
pub extern "C" fn ui_touch(kind: i32, x: i32, y: i32) {
    let Some(ui) = ui() else { return };
    let position = LogicalPosition::new(x as f32, y as f32);
    let button = PointerEventButton::Left;
    let window = ui.window.window();
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

struct Overlay<'a> {
    fb: &'a mut [u32],
    stride: usize,
    line: &'a mut [PremultipliedRgbaColor],
}

#[inline(always)]
fn over(src: u32, dst: u32, keep: u32) -> u32 {
    let v = dst * keep + 128;
    src + ((v + (v >> 8)) >> 8)
}

impl LineBufferProvider for Overlay<'_> {
    type TargetPixel = PremultipliedRgbaColor;

    fn process_line(
        &mut self,
        y: usize,
        range: core::ops::Range<usize>,
        render_fn: impl FnOnce(&mut [PremultipliedRgbaColor]),
    ) {
        let line = &mut self.line[range.clone()];
        render_fn(line);
        let row = &mut self.fb[y * self.stride + range.start..y * self.stride + range.end];
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_render(buf: *mut u32, stride_px: u32) {
    let Some(ui) = ui() else { return };
    let stride = stride_px as usize;
    if buf.is_null() || stride < ui.width {
        return;
    }
    let fb = unsafe { core::slice::from_raw_parts_mut(buf, stride * ui.height) };
    slint::platform::update_timers_and_animations();
    ui.window.request_redraw();
    let line = &mut ui.line[..];
    ui.window.draw_if_needed(|renderer| {
        renderer.render_by_line(Overlay { fb, stride, line });
    });
}
