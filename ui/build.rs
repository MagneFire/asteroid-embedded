// Compiles the .slint files for the software renderer. The UI is not built for a screen size: its
// lengths are fractions of the screen (Theme.unit), set when the firmware starts. So the icons,
// the UI's own and the AsteroidOS Ionicons set for notifications, are stored as outlines for the
// firmware to draw at the size it shows them, and so are the generated shapes: the nightstand
// battery ring, the heart rate arc and the Diamonds tiles, each a small SVG cut to its bounding
// box, with its position and size. The codes the UI and the firmware exchange (the settings, the
// actions, the event kinds) are read from the C headers into a Slint global and Rust constants,
// so neither side keeps its own copy.
//
// The generators work in px of a UI_PX screen, the size the .slint files were first drawn at,
// and write their lengths in Theme units.
use skrifa::instance::{LocationRef, Size};
use skrifa::outline::pen::SvgPen;
use skrifa::outline::DrawSettings;
use skrifa::{FontRef, MetadataProvider};
use std::fmt::Write as _;
use std::path::PathBuf;

const UI_PX: u32 = 400;
const ICON_DIR: &str = "../third_party/asteroid-icons-ion/src";
const ICON_UNIT: i32 = 8192;
const ICON_NONZERO: i16 = -1;
const ICON_EVENODD: i16 = -2;
const ICON_MOVE: i16 = -3;
const ICON_LINE: i16 = -4;
const ICON_QUAD: i16 = -5;
const ICON_CUBIC: i16 = -6;
const ICON_CLOSE: i16 = -7;

/// A length laid out in px of a UI_PX screen, as the UI writes it: in units of Theme.unit, a
/// hundredth of the screen.
fn units(px: f64) -> String {
    format!("{} * Theme.unit", (px * 100.0 / UI_PX as f64 * 1e4).round() / 1e4)
}

/// A white polygon (points in px) as an SVG cut to its bounding box on whole px, written to path,
/// and the box's position and size.
fn polygon_svg(path: &std::path::Path, pts: &[(f64, f64)]) -> (f64, f64, f64, f64) {
    let g = 1.0;
    let x0 = (pts.iter().map(|p| p.0).fold(f64::MAX, f64::min) / g).floor() * g;
    let y0 = (pts.iter().map(|p| p.1).fold(f64::MAX, f64::min) / g).floor() * g;
    let x1 = (pts.iter().map(|p| p.0).fold(f64::MIN, f64::max) / g).ceil() * g;
    let y1 = (pts.iter().map(|p| p.1).fold(f64::MIN, f64::max) / g).ceil() * g;
    let poly: Vec<String> = pts.iter().map(|p| format!("{:.2},{:.2}", p.0 - x0, p.1 - y0)).collect();
    let svg = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{w}\" height=\"{h}\" viewBox=\"0 0 {w} {h}\"><polygon points=\"{}\" fill=\"white\"/></svg>",
        poly.join(" "),
        w = x1 - x0,
        h = y1 - y0
    );
    std::fs::write(path, svg).unwrap();
    (x0, y0, x1 - x0, y1 - y0)
}

/// icons.rs: every icon's outline, for the firmware to draw at whatever size the screen shows it.
/// The UI's own icons and the Ionicons set (notification icons) go into one table sorted by name,
/// the UI's copy winning where both have one. usvg resolves transforms, shapes and strokes into
/// filled paths; their points are stored as i16 in 1/ICON_UNIT of the icon's box, each path
/// starting with its fill rule (ICON_NONZERO, ICON_EVENODD) and its segments after it.
fn icon_outlines(out: &std::path::Path) {
    use usvg::tiny_skia_path::PathSegment;
    let mut files = std::collections::BTreeMap::new();
    let generated = |sub: &str| (out.join(sub).display().to_string(), format!("{sub}/"));
    let dirs = [
        (ICON_DIR.to_string(), String::new()),
        ("icons".to_string(), String::new()),
        generated("ring"),
        generated("heart-arc"),
        generated("activity-arc"),
        generated("game"),
    ];
    for (dir, prefix) in &dirs {
        for e in std::fs::read_dir(dir).unwrap() {
            let name = e.unwrap().file_name().into_string().unwrap();
            if let Some(stem) = name.strip_suffix(".svg") {
                files.insert(format!("{prefix}{stem}"), format!("{dir}/{name}"));
            }
        }
    }
    let mut data: Vec<i16> = Vec::new();
    let mut table = String::from("static ICONS: &[(&str, u32, u32)] = &[\n");
    for (name, path) in &files {
        let svg = std::fs::read_to_string(path).unwrap();
        let tree = usvg::Tree::from_str(&svg, &usvg::Options::default()).unwrap_or_else(|e| panic!("{path}: {e}"));
        let (w, h) = (tree.size().width(), tree.size().height());
        let start = data.len();
        let mut paths = Vec::new();
        collect_paths(tree.root(), &mut paths);
        for (p, evenodd) in paths {
            data.push(if evenodd { ICON_EVENODD } else { ICON_NONZERO });
            let pt = |x: f32, y: f32| [(x / w * ICON_UNIT as f32).round() as i16, (y / h * ICON_UNIT as f32).round() as i16];
            for seg in p.segments() {
                match seg {
                    PathSegment::MoveTo(a) => { data.push(ICON_MOVE); data.extend(pt(a.x, a.y)); }
                    PathSegment::LineTo(a) => { data.push(ICON_LINE); data.extend(pt(a.x, a.y)); }
                    PathSegment::QuadTo(a, b) => { data.push(ICON_QUAD); data.extend(pt(a.x, a.y)); data.extend(pt(b.x, b.y)); }
                    PathSegment::CubicTo(a, b, c) => {
                        data.push(ICON_CUBIC);
                        data.extend(pt(a.x, a.y));
                        data.extend(pt(b.x, b.y));
                        data.extend(pt(c.x, c.y));
                    }
                    PathSegment::Close => data.push(ICON_CLOSE),
                }
            }
        }
        writeln!(table, "    (\"{name}\", {start}, {}),", data.len() - start).unwrap();
    }
    table.push_str("];\n");
    let mut rust = format!(
        "pub const ICON_UNIT: i32 = {ICON_UNIT};\npub const ICON_NONZERO: i16 = {ICON_NONZERO};\n\
         pub const ICON_EVENODD: i16 = {ICON_EVENODD};\npub const ICON_MOVE: i16 = {ICON_MOVE};\n\
         pub const ICON_LINE: i16 = {ICON_LINE};\npub const ICON_QUAD: i16 = {ICON_QUAD};\n\
         pub const ICON_CUBIC: i16 = {ICON_CUBIC};\npub const ICON_CLOSE: i16 = {ICON_CLOSE};\n"
    );
    rust.push_str(&table);
    writeln!(rust, "static ICON_DATA: [i16; {}] = {:?};", data.len(), data).unwrap();
    std::fs::write(out.join("icons.rs"), rust).unwrap();
}

/// The filled outlines of a group's visible paths, in the icon's coordinates: strokes become the
/// outline of the stroke.
fn collect_paths(group: &usvg::Group, out: &mut Vec<(usvg::tiny_skia_path::Path, bool)>) {
    for node in group.children() {
        match node {
            usvg::Node::Group(g) => collect_paths(g, out),
            usvg::Node::Path(p) if p.is_visible() => {
                let ts = p.abs_transform();
                if let Some(fill) = p.fill() {
                    if let Some(d) = p.data().clone().transform(ts) {
                        out.push((d, fill.rule() == usvg::FillRule::EvenOdd));
                    }
                }
                if let Some(stroke) = p.stroke() {
                    if let Some(d) = p.data().stroke(&stroke.to_tiny_skia(), 1.0).and_then(|d| d.transform(ts)) {
                        out.push((d, false));
                    }
                }
            }
            _ => {}
        }
    }
}

fn main() {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    ring(&out);
    heart_arc(&out);
    activity_arc(&out);
    game_shapes(&out);
    icon_outlines(&out);
    codes(&out);

    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRendererNoFonts)
        .with_include_paths(vec![out, std::env::current_dir().unwrap().join("ui")]);
    slint_build::compile_with_config("ui/watchface.slint", config).unwrap();
    println!("cargo:rerun-if-changed=fonts");
    println!("cargo:rerun-if-changed=icons");
    println!("cargo:rerun-if-changed={ICON_DIR}");
    println!("cargo:rerun-if-changed=../src/ui/ui.h");
    println!("cargo:rerun-if-changed=../src/asteroid.h");
}

/// The enumerators of `enum name { ... }` in a C header: only identifiers, with or without an
/// integer value, as the firmware's headers write them.
fn c_enum(header: &str, name: &str) -> Vec<(String, i64)> {
    let start = header.find(&format!("enum {name} {{")).unwrap_or_else(|| panic!("no enum {name}"));
    let body = &header[start..];
    let body = &body[body.find('{').unwrap() + 1..body.find('}').unwrap()];
    let mut next = 0;
    let mut out = Vec::new();
    for entry in body.split(',') {
        let entry = entry.trim();
        if entry.is_empty() {
            continue;
        }
        let (ident, value) = match entry.split_once('=') {
            Some((i, v)) => (i.trim(), v.trim().parse::<i64>().unwrap_or_else(|_| panic!("{name}: {entry}"))),
            None => (entry, next),
        };
        out.push((ident.to_string(), value));
        next = value + 1;
    }
    out
}

/// codes.slint: a `Codes` global with one property per enumerator, lower-kebab (UI_LOCK is
/// Codes.ui-lock); codes.rs: the same as Rust constants.
fn codes(out: &std::path::Path) {
    let ui_h = std::fs::read_to_string("../src/ui/ui.h").unwrap();
    let asteroid_h = std::fs::read_to_string("../src/asteroid.h").unwrap();
    let mut slint = String::from("export global Codes {\n");
    let mut rust = String::new();
    for (header, name, ty) in [
        (&asteroid_h, "setting", "i32"),
        (&ui_h, "ui_action", "i32"),
        (&ui_h, "ui_clock_action", "i32"),
        (&ui_h, "ui_media_action", "i32"),
        (&ui_h, "ui_alarm_action", "i32"),
        (&ui_h, "ui_event_kind", "u32"),
        (&ui_h, "ui_calc_key", "i32"),
        (&ui_h, "ui_game_move", "i32"),
    ] {
        for (ident, value) in c_enum(header, name) {
            writeln!(slint, "    out property <int> {}: {value};", ident.to_lowercase().replace('_', "-")).unwrap();
            writeln!(rust, "pub const {ident}: {ty} = {value};").unwrap();
        }
    }
    slint.push_str("}\n");
    std::fs::write(out.join("codes.slint"), slint).unwrap();
    std::fs::write(out.join("codes.rs"), rust).unwrap();
}

/// asteroid-hrm's arc: radius 1/2.9 of the screen, 3.4 % wide, clockwise from 115 degrees (from 3
/// o'clock, clockwise) round to 65, leaving the bottom for the heart. It is drawn at 40 % opacity
/// and the renderer has no group opacity, so its pieces must not overlap: 310 segments of one
/// degree, and for each degree a round start cap (the half disc before it) and end cap (after it),
/// so any run of segments gets round ends. The arrow by the previous rate, pointing at the
/// centre, comes for each degree from 140 to 220.
fn heart_arc(out: &std::path::Path) {
    let size = UI_PX as f64;
    let (r, half, c) = (size / 2.9, size * 0.034 / 2.0, size / 2.0);
    let dir = out.join("heart-arc");
    std::fs::create_dir_all(&dir).unwrap();
    let mut slint = String::from("import { Theme } from \"theme.slint\";\nexport global HeartArc {\n");
    let mut list = |name: &str, polys: Vec<Vec<(f64, f64)>>| {
        writeln!(slint, "    out property <[{{img: string, x: length, y: length, w: length, h: length}}]> {name}: [").unwrap();
        for (i, pts) in polys.iter().enumerate() {
            let path = dir.join(format!("{name}-{i}.svg"));
            let (x0, y0, w, h) = polygon_svg(&path, pts);
            writeln!(slint, "        {{ img: \"heart-arc/{name}-{i}\", x: {}, y: {}, w: {}, h: {} }},", units(x0), units(y0), units(w), units(h))
                .unwrap();
        }
        slint.push_str("    ];\n");
    };
    let at = |rr: f64, a: f64| (c + rr * a.cos(), c + rr * a.sin());
    let segments = (0..310)
        .map(|i| {
            let a0 = (115.0 + i as f64).to_radians();
            let a1 = a0 + 1.0_f64.to_radians();
            vec![at(r + half, a0), at(r + half, a1), at(r - half, a1), at(r - half, a0)]
        })
        .collect();
    list("segments", segments);
    // A half disc on the radial line at the angle, towards smaller (start) or larger (end) angles.
    let cap = |deg: f64, dir: f64| {
        let a = deg.to_radians();
        let (cx, cy) = at(r, a);
        (0..=16)
            .map(|k| {
                let phi = (-90.0 + k as f64 * 11.25_f64).to_radians();
                let radial = phi.sin() * half;
                let along = phi.cos() * half * dir;
                (cx + radial * a.cos() - along * a.sin(), cy + radial * a.sin() + along * a.cos())
            })
            .collect::<Vec<_>>()
    };
    list("starts", (0..=310).map(|k| cap(115.0 + k as f64, -1.0)).collect());
    list("ends", (0..=310).map(|k| cap(115.0 + k as f64, 1.0)).collect());
    let arrows = (140..=220)
        .map(|deg| {
            let a = (deg as f64).to_radians();
            let (tip, base, w) = (r - 58.0, r - 38.0, 11.0);
            let (bx, by) = at(base, a);
            vec![at(tip, a), (bx - w * a.sin(), by + w * a.cos()), (bx + w * a.sin(), by - w * a.cos())]
        })
        .collect();
    list("arrows", arrows);
    slint.push_str("}\n");
    std::fs::write(out.join("heart_arc.slint"), slint).unwrap();
}

/// The Activity app's ring, on the heart arc's circle: the whole of it, and for each degree k up
/// to 310 the part from 115 to 115 + k filled, each a single band with round ends, as pieces of
/// the band would show their seams. All share the circle's box, so they lie on top of each other.
fn activity_arc(out: &std::path::Path) {
    let size = UI_PX as f64;
    let (r, half, c) = (size / 2.9, size * 0.034 / 2.0, size / 2.0);
    let dir = out.join("activity-arc");
    std::fs::create_dir_all(&dir).unwrap();
    let x0 = (c - r - half).floor();
    let w = (c + r + half).ceil() - x0;
    let at = |rr: f64, deg: f64| {
        let a = deg.to_radians();
        (c + rr * a.cos() - x0, c + rr * a.sin() - x0)
    };
    let band = |from: f64, to: f64| {
        let large = if to - from > 180.0 { 1 } else { 0 };
        let (o0, o1, i1, i0) = (at(r + half, from), at(r + half, to), at(r - half, to), at(r - half, from));
        format!(
            "M{:.2},{:.2} A{ro:.2},{ro:.2} 0 {large} 1 {:.2},{:.2} A{half:.2},{half:.2} 0 0 1 {:.2},{:.2} \
             A{ri:.2},{ri:.2} 0 {large} 0 {:.2},{:.2} A{half:.2},{half:.2} 0 0 1 {:.2},{:.2} Z",
            o0.0, o0.1, o1.0, o1.1, i1.0, i1.1, i0.0, i0.1, o0.0, o0.1,
            ro = r + half,
            ri = r - half,
        )
    };
    let write = |name: &str, d: String| {
        let svg = format!(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{w}\" height=\"{w}\" viewBox=\"0 0 {w} {w}\"><path d=\"{d}\" fill=\"white\"/></svg>"
        );
        std::fs::write(dir.join(format!("{name}.svg")), svg).unwrap();
    };
    write("track", band(115.0, 425.0));
    for k in 1..=310 {
        write(&format!("fill-{k}"), band(115.0, 115.0 + k as f64));
    }
    let slint = format!(
        "import {{ Theme }} from \"theme.slint\";\nexport global ActivityArc {{\n    out property <length> x: {};\n    out property <length> size: {};\n}}\n",
        units(x0),
        units(w)
    );
    std::fs::write(out.join("activity_arc.slint"), slint).unwrap();
}

/// The glyphs of text in the font at px as SVG paths from the baseline's start, filled with
/// fill, and the text's width.
fn glyph_paths(font: &FontRef, text: &str, px: f32, fill: &str) -> (String, f32) {
    let size = Size::new(px);
    let charmap = font.charmap();
    let metrics = font.glyph_metrics(size, LocationRef::default());
    let outlines = font.outline_glyphs();
    let mut out = String::new();
    let mut x = 0.0f32;
    for ch in text.chars() {
        let gid = charmap.map(ch).unwrap_or_else(|| panic!("no glyph for {ch}"));
        let mut pen = SvgPen::with_precision(2);
        outlines.get(gid).unwrap().draw(DrawSettings::unhinted(size, LocationRef::default()), &mut pen).unwrap();
        write!(out, "<path d=\"{}\" fill=\"{fill}\" transform=\"translate({x:.2} 0) scale(1 -1)\"/>", &*pen).unwrap();
        x += metrics.advance_width(gid).unwrap();
    }
    (out, x)
}

/// asteroid-diamonds' board is turned 45 degrees, which the renderer cannot do, so its shapes
/// come as images: a tile (a rounded square 5 px smaller than a cell of the board, whose
/// diagonal is the screen) and the score boxes (0.3 by 0.12 of the screen), each turned 45
/// degrees one way or the other, cut to their bounding boxes. Text cannot be turned either, so
/// the boxes' captions and the digits of their numbers are glyph outlines turned the same way.
fn game_shapes(out: &std::path::Path) {
    let size = UI_PX as f64;
    let dir = out.join("game");
    std::fs::create_dir_all(&dir).unwrap();
    let field = (size * size / 2.0).sqrt().floor();
    let tile = field / 4.0 - 5.0;
    let font_data = std::fs::read("fonts/NotoSans-Bold.ttf").unwrap();
    let font = FontRef::new(&font_data).unwrap();
    let cap = |px: f32| font.metrics(Size::new(px), LocationRef::default()).cap_height.unwrap_or(px * 0.7);
    let mut slint = String::from("import { Theme } from \"theme.slint\";\nexport global GameShapes {\n");
    let mut shape = |name: &str, w: f64, h: f64, angle: f64| {
        let bbox = ((w + h) / 2.0_f64.sqrt()).ceil();
        let (x, y, c) = ((bbox - w) / 2.0, (bbox - h) / 2.0, bbox / 2.0);
        let svg = format!(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{bbox}\" height=\"{bbox}\" viewBox=\"0 0 {bbox} {bbox}\"><rect x=\"{x:.2}\" y=\"{y:.2}\" width=\"{w:.2}\" height=\"{h:.2}\" rx=\"3\" fill=\"white\" transform=\"rotate({angle} {c} {c})\"/></svg>"
        );
        let path = dir.join(format!("{name}.svg"));
        std::fs::write(&path, svg).unwrap();
        writeln!(slint, "    out property <string> {name}: \"game/{name}\";").unwrap();
        writeln!(slint, "    out property <length> {name}-size: {};", units(bbox)).unwrap();
    };
    shape("tile", tile, tile, 45.0);
    shape("box-left", size * 0.3, size * 0.12, 45.0);
    shape("box-right", size * 0.3, size * 0.12, -45.0);
    // The caption sits in the lower part of its box (0.6 of the height down, 12 px), the number
    // in the upper (0.1 down, 20 px), both centred along it.
    let (w, h) = (size * 0.3, size * 0.12);
    let bbox = ((w + h) / 2.0_f64.sqrt()).ceil();
    for (name, text, angle) in [("caption-left", "BEST", 45.0), ("caption-right", "SCORE", -45.0)] {
        let (paths, width) = glyph_paths(&font, text, 12.0, "#eee4da");
        let svg = format!(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{bbox}\" height=\"{bbox}\" viewBox=\"0 0 {bbox} {bbox}\"><g transform=\"translate({c} {c}) rotate({angle}) translate({x:.2} {y:.2})\">{paths}</g></svg>",
            c = bbox / 2.0,
            x = -width as f64 / 2.0,
            y = h * 0.6 + 12.0 * 1.36 / 2.0 + cap(12.0) as f64 / 2.0 - h / 2.0,
        );
        std::fs::write(dir.join(format!("{name}.svg")), svg).unwrap();
        writeln!(slint, "    out property <string> {name}: \"game/{name}\";").unwrap();
    }
    let digit_px = 28.0;
    let advance = font.glyph_metrics(Size::new(20.0), LocationRef::default()).advance_width(font.charmap().map('0').unwrap()).unwrap();
    for (name, angle) in [("digits-left", 45.0), ("digits-right", -45.0)] {
        writeln!(slint, "    out property <[string]> {name}: [").unwrap();
        for d in 0..10 {
            let (paths, width) = glyph_paths(&font, &d.to_string(), 20.0, "white");
            let svg = format!(
                "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{digit_px}\" height=\"{digit_px}\" viewBox=\"0 0 {digit_px} {digit_px}\"><g transform=\"translate({c} {c}) rotate({angle}) translate({x:.2} {y:.2})\">{paths}</g></svg>",
                c = digit_px / 2.0,
                x = -width / 2.0,
                y = cap(20.0) / 2.0,
            );
            std::fs::write(dir.join(format!("{name}-{d}.svg")), svg).unwrap();
            writeln!(slint, "        \"game/{name}-{d}\",").unwrap();
        }
        slint.push_str("    ];\n");
    }
    writeln!(slint, "    out property <length> digit-size: {};", units(digit_px)).unwrap();
    writeln!(slint, "    out property <length> digit-advance: {};", units(advance as f64)).unwrap();
    // The number's centre line, above the box's centre.
    writeln!(slint, "    out property <length> number-offset: {};", units(h * 0.1 + 20.0 * 1.36 / 2.0 - h / 2.0)).unwrap();
    writeln!(slint, "    out property <length> pitch: {};", units(field / 4.0)).unwrap();
    slint.push_str("}\n");
    std::fs::write(out.join("game_shapes.slint"), slint).unwrap();
}

/// 50 segments of 1.2 degrees, 7.2 degrees apart from 12 o'clock clockwise, between radius
/// 0.395 and 0.45 of the screen.
fn ring(out: &std::path::Path) {
    let size = UI_PX as f64;
    let dir = out.join("ring");
    std::fs::create_dir_all(&dir).unwrap();
    let mut slint = String::from(
        "import { Theme } from \"theme.slint\";\nexport global Ring {\n    out property <[{img: string, x: length, y: length, w: length, h: length}]> segments: [\n",
    );
    for i in 0..50 {
        let a0 = (-90.0 + i as f64 * 7.2_f64).to_radians();
        let a1 = a0 + 1.2_f64.to_radians();
        let pts: Vec<(f64, f64)> = [(0.395, a0), (0.45, a0), (0.45, a1), (0.395, a1)]
            .iter()
            .map(|&(r, a)| (size / 2.0 + r * size * a.cos(), size / 2.0 + r * size * a.sin()))
            .collect();
        let path = dir.join(format!("{i}.svg"));
        let (x0, y0, w, h) = polygon_svg(&path, &pts);
        writeln!(slint, "        {{ img: \"ring/{i}\", x: {}, y: {}, w: {}, h: {} }},", units(x0), units(y0), units(w), units(h)).unwrap();
    }
    slint.push_str("    ];\n}\n");
    std::fs::write(out.join("ring.slint"), slint).unwrap();
}
