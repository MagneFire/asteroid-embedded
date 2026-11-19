// The Cortex-M build's Heart Rate arc and the arrow by the previous rate, and the Steps page's
// ring on the same circle, painted under the screen's items as each line is drawn. As images, a
// piece per degree with its round ends, they take more flash and heap than the watch has.
use core::cell::UnsafeCell;
use slint::platform::software_renderer::{PremultipliedRgbaColor, Rgb565Pixel, TargetPixel};

/// asteroid-hrm's green, at 40 %.
const COLOR: (u32, u32, u32) = (0x38, 0xff, 0x12);
const OPACITY: f32 = 0.4;
/// Activity's ring: the filled run in its accent over the whole of it in white at 25 %.
const STEPS_COLOR: (u32, u32, u32) = (0xff, 0xc9, 0x4a);
const TRACK_OPACITY: f32 = 0.25;
const TRACK: (i32, i32) = (115, 425);

/// What heart.slint shows, in whole degrees clockwise from 3 o'clock: the arc from start to e1 and,
/// where end is past g2, from g2 to end, each with round ends, and the arrow at arrow, 0 for none.
/// With steps, the Steps page's ring instead.
#[derive(Clone, Copy, Default, PartialEq)]
pub struct Arc {
    pub start: i32,
    pub e1: i32,
    pub g2: i32,
    pub end: i32,
    pub arrow: i32,
    pub steps: bool,
}

/// A run of the arc: the directions of its ends, and whether it is over half a turn.
#[derive(Clone, Copy, Default)]
struct Run {
    from: (f32, f32),
    to: (f32, f32),
    wide: bool,
}

impl Run {
    fn new(from: i32, to: i32) -> Self {
        Run { from: dir(from as f64), to: dir(to as f64), wide: to - from > 180 }
    }

    /// How much of the pixel at (px, py) from the centre the run covers, round ends included, of
    /// the part band that the circle of radius r covers.
    fn cover(&self, px: f32, py: f32, r: f32, band: f32, round: f32) -> f32 {
        // How far past each end's radius, towards the other end.
        let (a, b) = (self.from.0 * py - self.from.1 * px, self.to.1 * px - self.to.0 * py);
        let within = if self.wide { a.max(b) } else { a.min(b) };
        let mut cover = band.min((within + 0.5).clamp(0.0, 1.0));
        for end in [self.from, self.to] {
            let (ex, ey) = (px - r * end.0, py - r * end.1);
            let d2 = ex * ex + ey * ey;
            if d2 < round * round {
                cover = cover.max((round - sqrt(d2)).min(1.0));
            }
        }
        cover
    }
}

#[derive(Clone, Copy, Default)]
struct State {
    on: bool,
    arc: Arc,
    cx: f32,
    cy: f32,
    r: f32,
    half: f32,
    unit: f32,
    runs: [Run; 2],
    count: usize,
    arrow: Option<[(f32, f32); 3]>,
    /// The Steps ring's whole circle, for steps.
    track: Option<Run>,
}

struct Shared(UnsafeCell<Option<State>>);
// The UI runs on one thread.
unsafe impl Sync for Shared {}
static STATE: Shared = Shared(UnsafeCell::new(None));

fn state() -> &'static mut Option<State> {
    unsafe { &mut *STATE.0.get() }
}

pub fn dir(deg: f64) -> (f32, f32) {
    let a = deg.to_radians();
    (libm::cos(a) as f32, libm::sin(a) as f32)
}

#[cfg(all(target_arch = "arm", target_abi = "eabihf"))]
pub fn sqrt(x: f32) -> f32 {
    // Through a core register: Rust takes the M4F's single-precision unit for less than the
    // `vfp2` that the `sreg` class asks for.
    let mut bits = x.to_bits();
    unsafe {
        core::arch::asm!("vmov s14, {0}", "vsqrt.f32 s14, s14", "vmov {0}, s14", inout(reg) bits, out("s14") _,
            options(pure, nomem, nostack))
    };
    f32::from_bits(bits)
}

#[cfg(not(all(target_arch = "arm", target_abi = "eabihf")))]
pub fn sqrt(x: f32) -> f32 {
    libm::sqrtf(x)
}

type Box = (f32, f32, f32, f32);

fn grow(b: &mut Box, p: (f32, f32)) {
    *b = (b.0.min(p.0), b.1.min(p.1), b.2.max(p.0), b.3.max(p.1));
}

const NONE: Box = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);

impl State {
    fn runs(arc: &Arc) -> ([(i32, i32); 2], usize) {
        let mut runs = [(0, 0); 2];
        let mut n = 0;
        for (from, to) in [(arc.start, arc.e1), (arc.g2, arc.end)] {
            if to > from {
                runs[n] = (from, to);
                n += 1;
            }
        }
        (runs, n)
    }

    fn at(&self, deg: i32, r: f32) -> (f32, f32) {
        let d = dir(deg as f64);
        (self.cx + r * d.0, self.cy + r * d.1)
    }

    /// The arrow's corners: 20 px long and 22 wide on a 400 px screen, its tip 58 px inside the arc.
    fn arrow(&self, deg: i32) -> [(f32, f32); 3] {
        let d = dir(deg as f64);
        let base = self.at(deg, self.r - 9.5 * self.unit);
        let w = 2.75 * self.unit;
        [self.at(deg, self.r - 14.5 * self.unit), (base.0 - w * d.1, base.1 + w * d.0), (base.0 + w * d.1, base.1 - w * d.0)]
    }

    /// A box around the band from lo to hi degrees with its round ends: around its ends, or
    /// around all of the arc if it goes round a quarter's end.
    fn band_box(&self, lo: i32, hi: i32) -> Box {
        let pad = self.half + 2.0;
        if lo.div_euclid(90) != hi.div_euclid(90) {
            let r = self.r + pad;
            return (self.cx - r, self.cy - r, self.cx + r, self.cy + r);
        }
        let (a, b) = (self.at(lo, self.r), self.at(hi, self.r));
        (a.0.min(b.0) - pad, a.1.min(b.1) - pad, a.0.max(b.0) + pad, a.1.max(b.1) + pad)
    }

    fn set(&mut self, arc: Arc) {
        self.arc = arc;
        let (runs, n) = Self::runs(&arc);
        self.count = n;
        for i in 0..n {
            self.runs[i] = Run::new(runs[i].0, runs[i].1);
        }
        self.arrow = (arc.arrow != 0).then(|| self.arrow(arc.arrow));
        self.track = arc.steps.then(|| Run::new(TRACK.0, TRACK.1));
    }
}

fn covers(arc: &Arc, deg: i32) -> bool {
    let (runs, n) = State::runs(arc);
    runs[..n].iter().any(|r| deg >= r.0 && deg < r.1)
}

fn mark(b: Box, dirty: &mut impl FnMut(u32, u32, u32, u32)) {
    let (x0, y0) = (b.0.max(0.0) as u32, b.1.max(0.0) as u32);
    let (x1, y1) = (b.2.max(0.0) as u32 + 1, b.3.max(0.0) as u32 + 1);
    dirty(x0, y0, x1 - x0, y1 - y0);
}

fn triangle_box(t: &[(f32, f32); 3]) -> Box {
    let mut b = NONE;
    t.iter().for_each(|&p| grow(&mut b, p));
    (b.0 - 1.0, b.1 - 1.0, b.2 + 1.0, b.3 + 1.0)
}

/// Shows arc on a width x height screen from the next draw, telling dirty the boxes that change.
pub fn set(arc: Arc, width: u32, height: u32, mut dirty: impl FnMut(u32, u32, u32, u32)) {
    let unit = width.min(height) as f32 / 100.0;
    let old = *state();
    let mut new = State {
        on: old.is_some_and(|s| s.on),
        cx: 50.0 * unit,
        cy: 50.0 * unit,
        r: 100.0 * unit / 2.9,
        half: 1.7 * unit,
        unit,
        ..Default::default()
    };
    new.set(arc);
    *state() = Some(new);
    let was = old.map(|s| s.arc).unwrap_or_default();
    if !new.on || was == arc {
        return;
    }
    if was.steps != arc.steps {
        mark(new.band_box(0, 360), &mut dirty);
        for deg in [was.arrow, arc.arrow] {
            if deg != 0 {
                mark(triangle_box(&new.arrow(deg)), &mut dirty);
            }
        }
        return;
    }
    // The degrees one of the two covers and the other does not, in runs between their ends.
    let mut ends = [was.start, was.e1, was.g2, was.end, arc.start, arc.e1, arc.g2, arc.end];
    ends.sort_unstable();
    for pair in ends.windows(2) {
        if pair[1] > pair[0] && covers(&was, pair[0]) != covers(&arc, pair[0]) {
            mark(new.band_box(pair[0], pair[1]), &mut dirty);
        }
    }
    if was.arrow != arc.arrow {
        for deg in [was.arrow, arc.arrow] {
            if deg != 0 {
                mark(triangle_box(&new.arrow(deg)), &mut dirty);
            }
        }
    }
}

/// Paints the arc or not, as the Heart Rate app or the Steps page is on screen or not; the box to
/// draw again if that changed.
pub fn show(on: bool) -> Option<(u32, u32, u32, u32)> {
    let s = state().as_mut()?;
    if s.on == on {
        return None;
    }
    s.on = on;
    let mut b = s.band_box(0, 360);
    if let Some(t) = &s.arrow {
        let t = triangle_box(t);
        b = (b.0.min(t.0), b.1.min(t.1), b.2.max(t.2), b.3.max(t.3));
    }
    let mut out = None;
    mark(b, &mut |x, y, w, h| out = Some((x, y, w, h)));
    out
}

/// Blends color over px at opacity, 0 to 1.
pub fn blend(px: &mut Rgb565Pixel, color: (u32, u32, u32), opacity: f32) {
    let a = (opacity * 255.0 + 0.5) as u32;
    px.blend(PremultipliedRgbaColor {
        red: (color.0 * a / 255) as u8,
        green: (color.1 * a / 255) as u8,
        blue: (color.2 * a / 255) as u8,
        alpha: a as u8,
    });
}

/// Paints the arc's part of a line over buf, the pixels from x = start on.
pub fn paint(line: usize, start: usize, buf: &mut [Rgb565Pixel]) {
    let Some(s) = state().as_ref().filter(|s| s.on) else { return };
    let py = line as f32 + 0.5 - s.cy;
    let end = start + buf.len();
    let clip = |from: f32, to: f32| ((from.max(0.0) as usize).max(start), (to.max(0.0) as usize + 1).min(end));
    if let Some(t) = &s.arrow {
        let b = triangle_box(t);
        let y = line as f32 + 0.5;
        if y > b.1 && y < b.3 {
            let (from, to) = clip(b.0, b.2);
            for x in from..to {
                let p = (x as f32 + 0.5, y);
                let mut inside = f32::MAX;
                for i in 0..3 {
                    let (a, b, c) = (t[i], t[(i + 1) % 3], t[(i + 2) % 3]);
                    let (ex, ey) = (b.0 - a.0, b.1 - a.1);
                    let side = |q: (f32, f32)| (q.0 - a.0) * ey - (q.1 - a.1) * ex;
                    inside = inside.min(side(p) * side(c).signum() / sqrt(ex * ex + ey * ey));
                }
                let cover = (inside + 0.5).clamp(0.0, 1.0);
                if cover > 0.0 {
                    blend(&mut buf[x - start], COLOR, cover * OPACITY);
                }
            }
        }
    }
    let outer = s.r + s.half + 1.0;
    if (s.count == 0 && !s.arc.steps) || py.abs() >= outer {
        return;
    }
    let (color, opacity) = if s.arc.steps { (STEPS_COLOR, 1.0) } else { (COLOR, OPACITY) };
    let reach = sqrt(outer * outer - py * py);
    let inner = s.r - s.half - 1.0;
    let hole = if py.abs() < inner { sqrt(inner * inner - py * py) } else { 0.0 };
    let (left, right) = (clip(s.cx - reach, s.cx - hole), clip(s.cx + hole, s.cx + reach));
    let round = s.half + 0.5;
    for (from, to) in [left, (right.0.max(left.1), right.1)] {
        for x in from..to {
            let px = x as f32 + 0.5 - s.cx;
            let band = (round - (sqrt(px * px + py * py) - s.r).abs()).clamp(0.0, 1.0);
            if band <= 0.0 {
                continue;
            }
            if let Some(track) = &s.track {
                let cover = track.cover(px, py, s.r, band, round);
                if cover > 0.0 {
                    blend(&mut buf[x - start], (255, 255, 255), cover * TRACK_OPACITY);
                }
            }
            let mut cover = 0.0f32;
            for run in &s.runs[..s.count] {
                cover = cover.max(run.cover(px, py, s.r, band, round));
            }
            if cover > 0.0 {
                blend(&mut buf[x - start], color, cover * opacity);
            }
        }
    }
}
