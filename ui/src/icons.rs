// Icons drawn from their outlines (build.rs's icons.rs) at the size the screen shows them, once:
// drawing a path every frame costs about a millisecond per icon here, a cached image almost
// nothing. Each is an alpha map in memory that is never freed, as the icons the Slint compiler
// embeds are, so it is drawn the same way: one byte a pixel, coloured by colorize.
use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use core::cell::RefCell;
use i_slint_core::graphics::euclid::{point2, size2};
use i_slint_core::graphics::{Image, ImageInner, IntRect, StaticTexture, StaticTextures, TexturePixelFormat};
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
    let data: &'static [u8] = Box::leak(alpha.into_boxed_slice());
    let texture: &'static [StaticTexture] = Box::leak(Box::new([StaticTexture {
        rect: IntRect::new(point2(0, 0), size2(w as i32, h as i32)),
        format: TexturePixelFormat::AlphaMap,
        color: i_slint_core::Color::from_rgb_u8(255, 255, 255),
        index: 0,
    }]));
    let textures = Box::leak(Box::new(StaticTextures {
        size: size2(w, h),
        original_size: size2(w, h),
        data: data.into(),
        textures: texture[..].into(),
    }));
    Image::from(ImageInner::StaticTextures(textures))
}

struct Cache(RefCell<BTreeMap<(&'static str, u32, u32), Image>>);
// The UI runs on one thread.
unsafe impl Sync for Cache {}
static CACHE: Cache = Cache(RefCell::new(BTreeMap::new()));

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
    if let Some(img) = CACHE.0.borrow().get(&key) {
        return img.clone();
    }
    let img = draw(name, w, h);
    CACHE.0.borrow_mut().insert(key, img.clone());
    img
}
