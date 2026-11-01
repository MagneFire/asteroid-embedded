// Icons drawn from their outlines (build.rs's icons.rs) at the size the screen shows them, once:
// drawing a path every frame costs about a millisecond per icon here, a cached image almost
// nothing. The images are white with the coverage as alpha, for Slint's colorize. The cache keeps
// the most recently used up to CACHE_BYTES; an image still shown stays alive until it is not.
use alloc::collections::BTreeMap;
use core::cell::RefCell;
use slint::{Image, Rgba8Pixel, SharedPixelBuffer};
use zeno::{Command, Fill, Mask, Vector};

include!(concat!(env!("OUT_DIR"), "/icons.rs"));

/// The outline's paths, scaled to w x h pixels, each with whether it fills even-odd.
fn paths(data: &[i16], w: f32, h: f32) -> alloc::vec::Vec<(alloc::vec::Vec<Command>, bool)> {
    let mut out: alloc::vec::Vec<(alloc::vec::Vec<Command>, bool)> = alloc::vec::Vec::new();
    let unit = ICON_UNIT as f32;
    let pt = |i: usize| Vector::new(data[i] as f32 * w / unit, data[i + 1] as f32 * h / unit);
    let mut i = 0;
    while i < data.len() {
        let op = data[i];
        i += 1;
        match op {
            ICON_NONZERO | ICON_EVENODD => out.push((alloc::vec::Vec::new(), op == ICON_EVENODD)),
            ICON_MOVE | ICON_LINE => {
                let p = pt(i);
                i += 2;
                if let Some(path) = out.last_mut() {
                    path.0.push(if op == ICON_MOVE { Command::MoveTo(p) } else { Command::LineTo(p) });
                }
            }
            ICON_QUAD => {
                let (a, b) = (pt(i), pt(i + 2));
                i += 4;
                if let Some(path) = out.last_mut() {
                    path.0.push(Command::QuadTo(a, b));
                }
            }
            ICON_CUBIC => {
                let (a, b, c) = (pt(i), pt(i + 2), pt(i + 4));
                i += 6;
                if let Some(path) = out.last_mut() {
                    path.0.push(Command::CurveTo(a, b, c));
                }
            }
            ICON_CLOSE => {
                if let Some(path) = out.last_mut() {
                    path.0.push(Command::Close);
                }
            }
            _ => break,
        }
    }
    out
}

/// The icon called name drawn at w x h pixels; an empty image if there is no such icon.
fn draw(name: &str, w: u32, h: u32) -> Image {
    let Ok(i) = ICONS.binary_search_by(|e| e.0.cmp(name)) else { return Image::default() };
    let (_, start, len) = ICONS[i];
    let data = &ICON_DATA[start as usize..(start + len) as usize];
    let mut alpha = alloc::vec![0u8; (w * h) as usize];
    for (cmds, evenodd) in paths(data, w as f32, h as f32) {
        let mut mask = alloc::vec![0u8; (w * h) as usize];
        Mask::new(&cmds[..]).size(w, h).style(if evenodd { Fill::EvenOdd } else { Fill::NonZero }).render_into(&mut mask, None);
        for (a, m) in alpha.iter_mut().zip(mask) {
            *a = (*a).max(m);
        }
    }
    let mut buf = SharedPixelBuffer::<Rgba8Pixel>::new(w, h);
    for (px, a) in buf.make_mut_slice().iter_mut().zip(alpha) {
        *px = Rgba8Pixel { r: a, g: a, b: a, a };
    }
    Image::from_rgba8_premultiplied(buf)
}

const CACHE_BYTES: u32 = 4 << 20;

/// The images by (name, width, height), each with when it was last used.
struct Cache {
    images: BTreeMap<(&'static str, u32, u32), (Image, u32)>,
    bytes: u32,
    uses: u32,
}

struct Shared(RefCell<Cache>);
// The UI runs on one thread.
unsafe impl Sync for Shared {}
static CACHE: Shared = Shared(RefCell::new(Cache { images: BTreeMap::new(), bytes: 0, uses: 0 }));

/// Whether there is an icon called name.
pub fn exists(name: &str) -> bool {
    ICONS.binary_search_by(|e| e.0.cmp(name)).is_ok()
}

/// The icon at w x h logical pixels, which are the screen's (the UI is not scaled), cached.
pub fn icon(name: &str, w: f32, h: f32) -> Image {
    let (w, h) = ((w.max(0.0) + 0.5) as u32, (h.max(0.0) + 0.5) as u32);
    let Ok(i) = ICONS.binary_search_by(|e| e.0.cmp(name)) else { return Image::default() };
    if w == 0 || h == 0 {
        return Image::default();
    }
    let key = (ICONS[i].0, w, h);
    let mut cache = CACHE.0.borrow_mut();
    cache.uses += 1;
    let now = cache.uses;
    if let Some((img, used)) = cache.images.get_mut(&key) {
        *used = now;
        return img.clone();
    }
    while cache.bytes + w * h * 4 > CACHE_BYTES {
        let Some(old) = cache.images.iter().min_by_key(|e| e.1.1).map(|e| *e.0) else { break };
        cache.images.remove(&old);
        cache.bytes -= old.1 * old.2 * 4;
    }
    let img = draw(name, w, h);
    cache.images.insert(key, (img.clone(), now));
    cache.bytes += w * h * 4;
    img
}
