// The Cortex-M build's icons: each as a distance field at one size, which the renderer scales to
// the size the screen shows it at, as Slint static textures (icons.rs). Only the icons the
// UI can show are kept.
use crate::sdf::{self, Flatten, Outline};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

/// The size of the longer side of an icon's field, in pixels.
const ICON_PX: f32 = 28.0;

/// The .slint files only a part uses (generated ones in OUT_DIR included), whose icons go when
/// every part using them is left out.
const PART_FILES: &[(&str, &[&str])] = &[
    ("notes", &["notes.slint"]),
    ("today", &["today.slint"]),
    ("quick", &["quick.slint"]),
    ("launcher", &["launcher.slint"]),
    ("music", &["music.slint"]),
    ("settings", &["settings.slint", "profile.slint", "launcher.slint"]),
    ("alarm", &["alarm.slint"]),
    ("timer", &["clock.slint"]),
    ("stopwatch", &["clock.slint"]),
    ("weather", &["weather.slint"]),
    ("heart", &["heart.slint", "heart_arc.slint"]),
    ("flashlight", &["flash.slint"]),
    ("calculator", &["calc.slint"]),
    ("diamonds", &["game.slint", "game_shapes.slint"]),
    ("agenda", &["agenda.slint"]),
    ("activity", &["activity.slint", "activity_arc.slint"]),
];

fn literals(text: &str) -> impl Iterator<Item = String> + '_ {
    text.split('"').skip(1).step_by(2).map(String::from)
}

/// The names the UI can ask for: those both in the .slint files of the parts the build has and in
/// the Rust code generated for them, where what is not shown has folded away.
fn names(out: &std::path::Path, code: &std::path::Path, leave_out: &[String]) -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    for e in std::fs::read_dir("ui").unwrap().chain(std::fs::read_dir(out).unwrap()) {
        let path = e.unwrap().path();
        let file = path.file_name().unwrap().to_str().unwrap().to_string();
        let parts: Vec<&str> = PART_FILES.iter().filter(|p| p.1.contains(&file.as_str())).map(|p| p.0).collect();
        if file.ends_with(".slint") && (parts.is_empty() || parts.iter().any(|p| !leave_out.iter().any(|l| l == p))) {
            names.extend(literals(&std::fs::read_to_string(&path).unwrap()));
        }
    }
    let generated: BTreeSet<String> = literals(&std::fs::read_to_string(code).unwrap()).collect();
    names.retain(|n| generated.contains(n));
    names
}

/// icons.rs: ICONS, the icons by name, sorted, each a static texture.
pub fn write(out: &std::path::Path, code: &std::path::Path, leave_out: &[String], files: &BTreeMap<String, String>) {
    let used = names(out, code, leave_out);
    let mut rust = String::from(
        "use i_slint_core::graphics::{IntSize, StaticTexture, StaticTextures, TexturePixelFormat};\n\
         use i_slint_core::slice::Slice;\n",
    );
    let mut table = String::new();
    for (n, (name, path)) in files.iter().filter(|f| used.contains(f.0)).enumerate() {
        let svg = std::fs::read_to_string(path).unwrap();
        let tree = usvg::Tree::from_str(&svg, &usvg::Options::default()).unwrap_or_else(|e| panic!("{path}: {e}"));
        let (w, h) = (tree.size().width(), tree.size().height());
        // The generated shapes are drawn at the size a 400 px screen shows them, the others at
        // ICON_PX.
        let s = if name.contains('/') { 1.0 } else { ICON_PX / w.max(h) };
        let (tw, th) = (((w * s).round() as usize).max(1), ((h * s).round() as usize).max(1));
        let mut paths = Vec::new();
        crate::collect_paths(tree.root(), &mut paths);
        let mut shapes = Vec::new();
        for (p, evenodd) in paths {
            use usvg::tiny_skia_path::PathSegment;
            let mut f = Flatten::default();
            let pt = |p: usvg::tiny_skia_path::Point| (p.x * s, p.y * s);
            for seg in p.segments() {
                match seg {
                    PathSegment::MoveTo(a) => f.move_to(pt(a)),
                    PathSegment::LineTo(a) => f.line_to(pt(a)),
                    PathSegment::QuadTo(a, b) => f.quad_to(pt(a), pt(b)),
                    PathSegment::CubicTo(a, b, c) => f.cubic_to(pt(a), pt(b), pt(c)),
                    PathSegment::Close => f.close(),
                }
            }
            f.close();
            shapes.push(Outline { segs: f.segs, evenodd });
        }
        let full = sdf::field(&shapes, tw, th, |c, r| (c as f32 + 0.5, r as f32 + 0.5));
        // Keep the rows and columns within 3 px of the shape: the texture's rect in the image.
        let near = |c: usize, r: usize| full[r * tw + c] as i8 > -127;
        let cols: Vec<usize> = (0..tw).filter(|&c| (0..th).any(|r| near(c, r))).collect();
        let rows: Vec<usize> = (0..th).filter(|&r| (0..tw).any(|c| near(c, r))).collect();
        let (x0, x1, y0, y1) = match (cols.first(), cols.last(), rows.first(), rows.last()) {
            (Some(&x0), Some(&x1), Some(&y0), Some(&y1)) => (x0, x1 + 1, y0, y1 + 1),
            _ => (0, 1, 0, 1),
        };
        let mut data: Vec<u8> = (y0..y1).flat_map(|r| full[r * tw + x0..r * tw + x1].to_vec()).collect();
        data.push(0);
        writeln!(
            rust,
            "static ICON_{n}: StaticTextures = StaticTextures {{ size: IntSize::new({tw}, {th}), original_size: IntSize::new({tw}, {th}), \
             data: Slice::from_slice({} as &[u8]), textures: Slice::from_slice(&[StaticTexture {{ \
             rect: i_slint_core::graphics::euclid::rect({x0}, {y0}, {}, {}), format: TexturePixelFormat::SignedDistanceField, \
             color: 0xffff_ffff, index: 0 }}]) }};",
            sdf::bytes(&data),
            x1 - x0,
            y1 - y0
        )
        .unwrap();
        writeln!(table, "    ({name:?}, &ICON_{n}),").unwrap();
    }
    writeln!(rust, "static ICONS: &[(&str, &StaticTextures)] = &[\n{table}];").unwrap();
    std::fs::write(out.join("icons.rs"), rust).unwrap();
}
