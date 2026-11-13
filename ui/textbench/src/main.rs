//! Times full repaints of Agenda-like text with the firmware's Slint configuration.
//!
//! `make textbench`, or `textbench [case] [frames]` to run one case for longer under a profiler,
//! or `textbench pixels` for a hash of every frame painted, to compare two builds by.
//! Four cases: nothing changed, the strip scrolling (its bindings go dirty each frame, a few
//! values really change), the event list scrolling, and the event titles changing. Each is
//! timed painting the pixels, and as a walk: the scene built, text laid out and glyphs looked
//! up, but no line painted, which is what the watch's `ui:` repaint time comes to with its
//! painting stubbed out.

use std::rc::Rc;
use std::time::{Duration, Instant};

use slint::platform::software_renderer::{
    LineBufferProvider, MinimalSoftwareWindow, RepaintBufferType, Rgb565Pixel,
};
use slint::platform::{Platform, WindowAdapter};
use slint::{ComponentHandle, PlatformError};

slint::include_modules!();

static FONTS: [&[u8]; 4] = [
    include_bytes!("../../fonts/NotoSans-SemiCondensedLight.ttf"),
    include_bytes!("../../fonts/NotoSans-SemiCondensedSemiBold.ttf"),
    include_bytes!("../../fonts/NotoSans-SemiCondensedBold.ttf"),
    include_bytes!("../../fonts/NotoSans-CondensedMedium.ttf"),
];

const SIZE: usize = 320;

struct Host {
    window: Rc<MinimalSoftwareWindow>,
    start: Instant,
}

impl Platform for Host {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        self.start.elapsed()
    }
}

/// Takes the lines without painting them.
struct Walk;

impl LineBufferProvider for Walk {
    type TargetPixel = Rgb565Pixel;

    fn process_line(
        &mut self,
        _line: usize,
        _range: core::ops::Range<usize>,
        _render_fn: impl FnOnce(&mut [Rgb565Pixel]),
    ) {
    }
}

fn repaint(window: &MinimalSoftwareWindow, buffer: &mut [Rgb565Pixel], paint: bool) {
    window.request_redraw();
    window.draw_if_needed(|renderer| {
        if paint {
            renderer.render(buffer, SIZE);
        } else {
            renderer.render_by_line(Walk);
        }
    });
}

fn time(
    frames: usize,
    bench: &Bench,
    window: &MinimalSoftwareWindow,
    step: &impl Fn(&Bench, usize),
    paint: bool,
) -> f64 {
    let mut buffer = vec![Rgb565Pixel::default(); SIZE * SIZE];
    // Warm up the glyph cache and the first shaping, as a watch that has drawn the app once.
    for frame in 0..600 {
        step(bench, frame);
        repaint(window, &mut buffer, paint);
    }
    let start = Instant::now();
    for frame in 0..frames {
        step(bench, frame);
        repaint(window, &mut buffer, paint);
    }
    start.elapsed().as_secs_f64() * 1e6 / frames as f64
}

/// FNV-1a over the frame painted after each step.
fn hashes(bench: &Bench, window: &MinimalSoftwareWindow, step: &impl Fn(&Bench, usize)) -> u64 {
    let mut buffer = vec![Rgb565Pixel::default(); SIZE * SIZE];
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for frame in 0..240 {
        step(bench, frame);
        repaint(window, &mut buffer, true);
        hash = buffer.iter().fold(hash, |hash, pixel| (hash ^ pixel.0 as u64).wrapping_mul(0x0100_0000_01b3));
    }
    hash
}

fn run(
    name: &str,
    frames: usize,
    bench: &Bench,
    window: &MinimalSoftwareWindow,
    step: impl Fn(&Bench, usize),
) {
    if std::env::args().nth(1).as_deref() == Some("pixels") {
        println!("{name:>12}: {:016x}", hashes(bench, window, &step));
        return;
    }
    let paint = time(frames, bench, window, &step, true);
    let walk = time(frames, bench, window, &step, false);
    println!("{name:>12}: {paint:7.1} us per repaint, {walk:6.1} us without painting");
}

fn main() {
    let mut args = std::env::args().skip(1);
    let only = args.next();
    let frames = args.next().and_then(|frames| frames.parse().ok()).unwrap_or(600);
    let pixels = only.as_deref() == Some("pixels");
    let wanted = |name: &str| pixels || only.as_deref().is_none_or(|only| only == name);

    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    window.set_size(slint::PhysicalSize::new(SIZE as u32, SIZE as u32));
    slint::platform::set_platform(Box::new(Host { window: window.clone(), start: Instant::now() })).unwrap();
    i_slint_core::context::with_global_context(
        || Err(PlatformError::NoPlatform),
        |ctx| FONTS.iter().for_each(|font| ctx.font_context().borrow_mut().register_static_font(font)),
    )
    .unwrap();

    let bench = Bench::new().unwrap();
    bench.show().unwrap();
    let item = 84.0 * SIZE as f32 / 100.0 / 2.5;

    if wanted("static") {
        run("static", frames, &bench, &window, |_, _| ());
    }
    // A swipe across two days and back, the way the Pager animates scroll.
    if wanted("strip") {
        run("strip scroll", frames, &bench, &window, |bench, frame| {
            let t = (frame % 120) as f32 / 120.0;
            let swing = if t < 0.5 { t * 2.0 } else { 2.0 - t * 2.0 };
            bench.set_scroll(10.0 * item + swing * 2.0 * item);
        });
    }
    if wanted("list") {
        run("list scroll", frames, &bench, &window, |bench, frame| {
            bench.set_list((frame % 64) as f32 * 0.5);
        });
    }
    if wanted("rename") {
        run("rename", frames, &bench, &window, |bench, frame| {
            bench.set_rename((frame / 4 % 50) as i32);
        });
    }
}
