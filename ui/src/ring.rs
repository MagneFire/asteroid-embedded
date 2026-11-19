// The Cortex-M build's nightstand battery ring, painted under the watchface's items as each line
// is drawn: 50 segments of 1.2 degrees, 7.2 degrees apart clockwise from 12 o'clock, between
// 0.395 and 0.45 of the screen from its centre. As images they take 20 KB of flash.
use crate::arc::{blend, dir, sqrt};
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use slint::platform::software_renderer::Rgb565Pixel;

const SEGMENTS: usize = 50;

/// A segment: the directions of its two sides and its middle, from the ring's centre.
struct Segment {
    from: (f32, f32),
    to: (f32, f32),
    x: f32,
    y: f32,
}

struct State {
    on: bool,
    /// How many segments show, their colour, and where the face is on the screen.
    shown: usize,
    color: slint::Color,
    dx: f32,
    dy: f32,
    cx: f32,
    cy: f32,
    inner: f32,
    outer: f32,
    segments: Vec<Segment>,
}

struct Shared(UnsafeCell<Option<State>>);
// The UI runs on one thread.
unsafe impl Sync for Shared {}
static STATE: Shared = Shared(UnsafeCell::new(None));

fn state() -> &'static mut Option<State> {
    unsafe { &mut *STATE.0.get() }
}

impl State {
    fn new(width: u32, height: u32) -> Self {
        let unit = width.min(height) as f32 / 100.0;
        let (c, inner, outer) = (50.0 * unit, 39.5 * unit, 45.0 * unit);
        let segments = (0..SEGMENTS)
            .map(|i| {
                let middle = dir(-90.0 + i as f64 * 7.2 + 0.6);
                Segment {
                    from: dir(-90.0 + i as f64 * 7.2),
                    to: dir(-90.0 + i as f64 * 7.2 + 1.2),
                    x: middle.0 * (inner + outer) / 2.0,
                    y: middle.1 * (inner + outer) / 2.0,
                }
            })
            .collect();
        State { on: false, shown: 0, color: slint::Color::default(), dx: 0.0, dy: 0.0, cx: c, cy: c, inner, outer, segments }
    }

    /// Half the side of the box around a segment's middle that holds all of it.
    fn reach(&self) -> f32 {
        (self.outer - self.inner) / 2.0 + 2.0
    }

    /// The box around the ring where it is.
    fn mark(&self, dirty: &mut impl FnMut(u32, u32, u32, u32)) {
        if self.on {
            let r = self.outer + 2.0;
            let (x, y) = ((self.cx + self.dx - r).max(0.0) as u32, (self.cy + self.dy - r).max(0.0) as u32);
            dirty(x, y, (2.0 * r) as u32 + 2, (2.0 * r) as u32 + 2);
        }
    }
}

/// Shows the first shown segments in color, for a face at (dx, dy) of a width x height screen,
/// from the next draw, telling dirty the box to draw again.
pub fn set(shown: i32, color: slint::Color, dx: f32, dy: f32, width: u32, height: u32, mut dirty: impl FnMut(u32, u32, u32, u32)) {
    let shown = shown.clamp(0, SEGMENTS as i32) as usize;
    if shown == 0 {
        if let Some(s) = state().take() {
            s.mark(&mut dirty);
        }
        return;
    }
    let s = state().get_or_insert_with(|| State::new(width, height));
    if s.shown != shown || s.color != color || s.dx != dx || s.dy != dy {
        s.mark(&mut dirty);
        (s.shown, s.color, s.dx, s.dy) = (shown, color, dx, dy);
        s.mark(&mut dirty);
    }
}

/// Paints the ring or not, as the watchface is on screen or not, telling dirty the box to draw
/// again if that changed.
pub fn show(on: bool, mut dirty: impl FnMut(u32, u32, u32, u32)) {
    let Some(s) = state().as_mut() else { return };
    if s.on != on {
        s.on = true;
        s.mark(&mut dirty);
        s.on = on;
    }
}

/// Paints the ring's part of a line over buf, the pixels from x = start on.
pub fn paint(line: usize, start: usize, buf: &mut [Rgb565Pixel]) {
    let Some(s) = state().as_ref().filter(|s| s.on) else { return };
    let y = line as f32 - s.dy;
    let py = y + 0.5 - s.cy;
    if py.abs() > s.outer + 1.0 {
        return;
    }
    let color = (s.color.red() as u32, s.color.green() as u32, s.color.blue() as u32);
    let opacity = s.color.alpha() as f32 / 255.0;
    let (end, reach) = (start + buf.len(), s.reach());
    for segment in &s.segments[..s.shown] {
        if (py - segment.y).abs() > reach {
            continue;
        }
        let middle = s.cx + s.dx + segment.x;
        let from = ((middle - reach).max(0.0) as usize).max(start);
        let to = ((middle + reach).max(0.0) as usize + 1).min(end);
        for x in from..to {
            let px = x as f32 + 0.5 - s.dx - s.cx;
            let r = sqrt(px * px + py * py);
            let band = (r - s.inner).min(s.outer - r);
            // How far past each side, towards the other.
            let within = (segment.from.0 * py - segment.from.1 * px).min(segment.to.1 * px - segment.to.0 * py);
            let cover = (band.min(within) + 0.5).clamp(0.0, 1.0);
            if cover > 0.0 {
                blend(&mut buf[x - start], color, cover * opacity);
            }
        }
    }
}
