// The Cortex-M build's text, glyphs made at build time (build/glyphs.rs).
use i_slint_core::window::WindowAdapter;
use slint::platform::software_renderer::MinimalSoftwareWindow;

include!(concat!(env!("OUT_DIR"), "/fonts.rs"));

pub fn register(window: &MinimalSoftwareWindow) {
    for font in &FONTS {
        window.renderer().register_bitmap_font(font);
    }
}
