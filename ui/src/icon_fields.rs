// The Cortex-M build's icons: distance fields made at build time (build/icons.rs), which the
// renderer scales to the size each is shown at, so nothing is drawn or kept at run time.
use i_slint_core::graphics::{Image, ImageInner};

include!(concat!(env!("OUT_DIR"), "/icons.rs"));

fn find(name: &str) -> Option<&'static StaticTextures> {
    ICONS.binary_search_by(|e| e.0.cmp(name)).ok().map(|i| ICONS[i].1)
}

/// Whether there is an icon called name.
pub fn exists(name: &str) -> bool {
    find(name).is_some()
}

/// The icon called name; an empty image if there is no such icon.
pub fn icon(name: &str, _w: f32, _h: f32) -> Image {
    find(name).map_or_else(Image::default, |t| Image::from(ImageInner::StaticTextures(t)))
}
