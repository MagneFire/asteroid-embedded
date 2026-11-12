// Signed distance fields of filled outlines, as Slint's software renderer draws them scaled to any
// size: a byte per pixel, the distance from the pixel's centre to the nearest edge of the shape,
// from 3 px outside (-128) to 3 px inside (127), with one byte after the last pixel that the
// renderer reads but does not use.

pub type Pt = (f32, f32);

/// A filled outline: its edges as segments in closed loops, and its fill rule.
pub struct Outline {
    pub segs: Vec<(Pt, Pt)>,
    pub evenodd: bool,
}

/// Collects an outline's edges, curves flattened to segments of at most about a quarter pixel.
#[derive(Default)]
pub struct Flatten {
    pub segs: Vec<(Pt, Pt)>,
    start: Pt,
    cur: Pt,
}

fn steps(len: f32) -> usize {
    ((len / 0.25).ceil() as usize).clamp(1, 256)
}

fn dist(a: Pt, b: Pt) -> f32 {
    ((a.0 - b.0).powi(2) + (a.1 - b.1).powi(2)).sqrt()
}

impl Flatten {
    pub fn move_to(&mut self, p: Pt) {
        self.close();
        self.start = p;
        self.cur = p;
    }

    pub fn line_to(&mut self, p: Pt) {
        if p != self.cur {
            self.segs.push((self.cur, p));
        }
        self.cur = p;
    }

    pub fn quad_to(&mut self, c: Pt, p: Pt) {
        let a = self.cur;
        let n = steps(dist(a, c) + dist(c, p));
        for k in 1..=n {
            let t = k as f32 / n as f32;
            let u = 1.0 - t;
            self.line_to((u * u * a.0 + 2.0 * u * t * c.0 + t * t * p.0, u * u * a.1 + 2.0 * u * t * c.1 + t * t * p.1));
        }
    }

    pub fn cubic_to(&mut self, c1: Pt, c2: Pt, p: Pt) {
        let a = self.cur;
        let n = steps(dist(a, c1) + dist(c1, c2) + dist(c2, p));
        for k in 1..=n {
            let t = k as f32 / n as f32;
            let u = 1.0 - t;
            let (b0, b1, b2, b3) = (u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t);
            self.line_to((
                b0 * a.0 + b1 * c1.0 + b2 * c2.0 + b3 * p.0,
                b0 * a.1 + b1 * c1.1 + b2 * c2.1 + b3 * p.1,
            ));
        }
    }

    pub fn close(&mut self) {
        let start = self.start;
        self.line_to(start);
    }
}

fn winding(segs: &[(Pt, Pt)], p: Pt) -> i32 {
    let mut w = 0;
    for &(a, b) in segs {
        if (a.1 <= p.1) != (b.1 <= p.1) {
            let x = a.0 + (p.1 - a.1) / (b.1 - a.1) * (b.0 - a.0);
            if x > p.0 {
                w += if b.1 > a.1 { 1 } else { -1 };
            }
        }
    }
    w
}

fn inside(shapes: &[Outline], p: Pt) -> bool {
    shapes.iter().any(|o| {
        let w = winding(&o.segs, p);
        if o.evenodd { w % 2 != 0 } else { w != 0 }
    })
}

/// The edges of the union of the shapes: those with the inside on one side only, so that where
/// shapes or loops overlap the edges within the union leave no seam.
fn edges(shapes: &[Outline]) -> Vec<(Pt, Pt)> {
    let mut out = Vec::new();
    for o in shapes {
        for &(a, b) in &o.segs {
            let len = dist(a, b);
            if len == 0.0 {
                continue;
            }
            let m = ((a.0 + b.0) / 2.0, (a.1 + b.1) / 2.0);
            let (nx, ny) = (-(b.1 - a.1) / len * 0.02, (b.0 - a.0) / len * 0.02);
            if inside(shapes, (m.0 + nx, m.1 + ny)) != inside(shapes, (m.0 - nx, m.1 - ny)) {
                out.push((a, b));
            }
        }
    }
    out
}

fn seg_dist(p: Pt, (a, b): (Pt, Pt)) -> f32 {
    let (dx, dy) = (b.0 - a.0, b.1 - a.1);
    let t = (((p.0 - a.0) * dx + (p.1 - a.1) * dy) / (dx * dx + dy * dy)).clamp(0.0, 1.0);
    dist(p, (a.0 + t * dx, a.1 + t * dy))
}

/// The field of the shapes on a w x h grid, rows from the top, at(column, row) being where the
/// centre of that pixel is in the shapes' coordinates (in pixels).
pub fn field(shapes: &[Outline], w: usize, h: usize, at: impl Fn(usize, usize) -> Pt) -> Vec<u8> {
    let edges = edges(shapes);
    let mut out = Vec::with_capacity(w * h + 1);
    for r in 0..h {
        for c in 0..w {
            let p = at(c, r);
            let mut d = 3.0f32;
            for &e in &edges {
                let (a, b) = e;
                if a.0.min(b.0) - d > p.0 || a.0.max(b.0) + d < p.0 || a.1.min(b.1) - d > p.1 || a.1.max(b.1) + d < p.1 {
                    continue;
                }
                d = d.min(seg_dist(p, e));
            }
            let s = if inside(shapes, p) { d } else { -d };
            out.push((s / 3.0 * 127.0).round().clamp(-128.0, 127.0) as i8 as u8);
        }
    }
    out.push(0);
    out
}

/// bytes as a Rust byte string literal.
pub fn bytes(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 4 + 3);
    s.push_str("b\"");
    for &b in data {
        match b {
            b'"' => s.push_str("\\\""),
            b'\\' => s.push_str("\\\\"),
            0x20..=0x7e => s.push(b as char),
            _ => s.push_str(&format!("\\x{b:02x}")),
        }
    }
    s.push('"');
    s
}
