// The Cortex-M build's text: each face's glyphs as distance fields at one size, which the
// renderer scales to whatever size the text asks for, written as Slint bitmap fonts (fonts.rs).
use crate::sdf::{self, Flatten, Outline};
use skrifa::instance::{LocationRef, Size};
use skrifa::outline::{DrawSettings, OutlinePen};
use skrifa::{FontRef, MetadataProvider};
use std::fmt::Write as _;

pub enum Chars {
    /// Printable ASCII, the Latin-1 letters and the common typographic punctuation: text from
    /// anywhere in western Europe.
    Text,
    /// Only these.
    Only(&'static str),
}

/// A face: its file in fonts/, the family and weight the UI asks for, the size in pixels its
/// fields are made at, its characters, and the other families the UI asks for that are drawn with
/// its glyphs.
pub struct Face {
    pub file: &'static str,
    pub family: &'static str,
    pub weight: u16,
    pub px: f32,
    pub chars: Chars,
    pub also: &'static [&'static str],
}

impl Chars {
    fn list(&self) -> Vec<char> {
        let mut v: Vec<char> = match self {
            Chars::Text => (' '..='~').chain('À'..='ÿ').filter(|&c| c != '×' && c != '÷').chain("°‘’“”–—…•€●".chars()).collect(),
            Chars::Only(s) => s.chars().chain([' ']).collect(),
        };
        v.sort();
        v.dedup();
        v
    }
}

impl OutlinePen for Flatten {
    fn move_to(&mut self, x: f32, y: f32) {
        Flatten::move_to(self, (x, y));
    }
    fn line_to(&mut self, x: f32, y: f32) {
        Flatten::line_to(self, (x, y));
    }
    fn quad_to(&mut self, cx0: f32, cy0: f32, x: f32, y: f32) {
        Flatten::quad_to(self, (cx0, cy0), (x, y));
    }
    fn curve_to(&mut self, cx0: f32, cy0: f32, cx1: f32, cy1: f32, x: f32, y: f32) {
        Flatten::cubic_to(self, (cx0, cy0), (cx1, cy1), (x, y));
    }
    fn close(&mut self) {
        Flatten::close(self);
    }
}

/// fonts.rs: FONTS, every face and its other families.
pub fn write(out: &std::path::Path, faces: &[Face]) {
    let mut rust = String::from(
        "use i_slint_core::graphics::{BitmapFont, BitmapGlyph, BitmapGlyphs};\n\
         use i_slint_core::slice::Slice;\n",
    );
    let mut fonts = Vec::new();
    for (n, face) in faces.iter().enumerate() {
        let data = std::fs::read(format!("fonts/{}", face.file)).unwrap();
        let font = FontRef::new(&data).unwrap();
        let size = Size::new(face.px);
        let charmap = font.charmap();
        let advances = font.glyph_metrics(size, LocationRef::default());
        let outlines = font.outline_glyphs();
        let chars: Vec<char> = face.chars.list().into_iter().filter(|&c| charmap.map(c).is_some()).collect();
        writeln!(rust, "static GLYPHS_{n}: [BitmapGlyph; {}] = [", chars.len()).unwrap();
        for &c in &chars {
            let gid = charmap.map(c).unwrap();
            let advance = (advances.advance_width(gid).unwrap_or(0.0) * 64.0).round() as i16;
            let mut pen = Flatten::default();
            outlines.get(gid).unwrap().draw(DrawSettings::unhinted(size, LocationRef::default()), &mut pen).unwrap();
            pen.close();
            if pen.segs.is_empty() {
                writeln!(rust, "    BitmapGlyph {{ x: 0, y: 0, width: 0, height: 0, x_advance: {advance}, data: Slice::from_slice(&[]) }},").unwrap();
                continue;
            }
            let pts = pen.segs.iter().flat_map(|s| [s.0, s.1]);
            let (x0, y0, x1, y1) = pts.fold((f32::MAX, f32::MAX, f32::MIN, f32::MIN), |b, p| {
                (b.0.min(p.0), b.1.min(p.1), b.2.max(p.0), b.3.max(p.1))
            });
            // A pixel of margin all round, as the renderer expects; y is up from the baseline.
            let (w, h) = ((x1 - x0 + 2.0).ceil() as usize, (y1 - y0 + 2.0).ceil() as usize);
            let shape = [Outline { segs: pen.segs, evenodd: false }];
            let field = sdf::field(&shape, w, h, |c, r| (x0 - 1.0 + c as f32 + 0.5, y0 - 1.0 + (h - 1 - r) as f32 + 0.5));
            writeln!(
                rust,
                "    BitmapGlyph {{ x: {}, y: {}, width: {w}, height: {h}, x_advance: {advance}, data: Slice::from_slice({} as &[u8]) }},",
                ((x0 - 1.0) * 64.0).ceil() as i16,
                ((y0 - 1.0) * 64.0).ceil() as i16,
                sdf::bytes(&field)
            )
            .unwrap();
        }
        rust.push_str("];\n");
        writeln!(rust, "static SIZES_{n}: [BitmapGlyphs; 1] = [BitmapGlyphs {{ pixel_size: {}, glyph_data: Slice::from_slice(&GLYPHS_{n}) }}];", face.px as i16).unwrap();
        // Sorted, as the list is: a code point's position is its glyph's.
        writeln!(rust, "static MAP_{n}: [char; {}] = {chars:?};", chars.len()).unwrap();
        let m = font.metrics(Size::unscaled(), LocationRef::default());
        for family in std::iter::once(&face.family).chain(face.also) {
            fonts.push(format!(
                "    BitmapFont {{ family_name: Slice::from_slice({family:?}.as_bytes()), character_map: Slice::from_slice(&MAP_{n}), \
                 units_per_em: {:?}, ascent: {:?}, descent: {:?}, x_height: {:?}, cap_height: {:?}, glyphs: Slice::from_slice(&SIZES_{n}), \
                 weight: {}, italic: false, sdf: true }},",
                m.units_per_em as f32,
                m.ascent,
                m.descent,
                m.x_height.unwrap_or_default(),
                m.cap_height.unwrap_or_default(),
                face.weight
            ));
        }
    }
    writeln!(rust, "pub static FONTS: [BitmapFont; {}] = [\n{}\n];", fonts.len(), fonts.join("\n")).unwrap();
    std::fs::write(out.join("fonts.rs"), rust).unwrap();
}
