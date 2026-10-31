// Compiles the .slint files with fonts and icons embedded as pixel data for the software renderer.
// The notification icons are the AsteroidOS Ionicons set, resized to NOTE_ICON_PX and listed in a
// generated Slint global, with their sorted names for the lookup by name. The nightstand battery
// ring is generated too: each of its segments a small SVG cut to its bounding box on a 400 px
// screen, with its position, since paths cannot be drawn at run time here. The codes the UI and
// the firmware exchange (the settings, the actions, the event kinds) are read from the C headers
// into a Slint global and Rust constants, so neither side keeps its own copy.
use std::fmt::Write as _;
use std::path::PathBuf;

const NOTE_ICON_PX: u32 = 40;
const ICON_DIR: &str = "../third_party/asteroid-icons-ion/src";

fn resize(svg: &str, px: u32) -> String {
    let Some(start) = svg.find("<svg") else { return svg.to_string() };
    let end = start + svg[start..].find('>').unwrap_or(0);
    let mut tag = svg[start..end].to_string();
    for attr in ["width", "height"] {
        if let Some(i) = tag.find(&format!(" {attr}=\"")) {
            let v = i + attr.len() + 3;
            let close = v + tag[v..].find('"').unwrap_or(0);
            tag.replace_range(i..close + 1, "");
        }
    }
    if !tag.contains("viewBox") {
        tag.push_str(" viewBox=\"0 0 512 512\"");
    }
    format!("{}{tag} width=\"{px}\" height=\"{px}\"{}", &svg[..start], &svg[end..])
}

fn main() {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let dir = out.join("note-icons");
    std::fs::create_dir_all(&dir).unwrap();
    let mut names: Vec<String> = std::fs::read_dir(ICON_DIR)
        .unwrap()
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter_map(|n| n.strip_suffix(".svg").map(str::to_string))
        .collect();
    names.sort();
    let mut slint = String::from("export global NoteIcons {\n    out property <[image]> list: [\n");
    let mut rust = String::from("static NOTE_ICON_NAMES: &[&str] = &[\n");
    for name in &names {
        let svg = std::fs::read_to_string(format!("{ICON_DIR}/{name}.svg")).unwrap();
        let path = dir.join(format!("{name}.svg"));
        std::fs::write(&path, resize(&svg, NOTE_ICON_PX)).unwrap();
        writeln!(slint, "        @image-url(\"{}\"),", path.display()).unwrap();
        writeln!(rust, "    \"{name}\",").unwrap();
    }
    slint.push_str("    ];\n}\n");
    rust.push_str("];\n");
    std::fs::write(out.join("note_icons.slint"), slint).unwrap();
    std::fs::write(out.join("note_icons.rs"), rust).unwrap();

    ring(&out);
    codes(&out);

    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRendererNoFonts)
        .with_include_paths(vec![out]);
    slint_build::compile_with_config("ui/watchface.slint", config).unwrap();
    println!("cargo:rerun-if-changed=fonts");
    println!("cargo:rerun-if-changed=icons");
    println!("cargo:rerun-if-changed={ICON_DIR}");
    println!("cargo:rerun-if-changed=../src/ui/ui.h");
    println!("cargo:rerun-if-changed=../src/asteroid.h");
    println!("cargo:rerun-if-env-changed=ASTEROID_BOARD");
    println!("cargo:rerun-if-env-changed=ASTEROID_UI_SIZE");
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
    let board = std::env::var("ASTEROID_BOARD").unwrap_or_else(|_| "sturgeon".into());
    let mut slint = format!("export global Codes {{\n    out property <string> board: \"{board}\";\n");
    let mut rust = String::new();
    for (header, name, ty) in [
        (&asteroid_h, "setting", "i32"),
        (&ui_h, "ui_action", "i32"),
        (&ui_h, "ui_clock_action", "i32"),
        (&ui_h, "ui_media_action", "i32"),
        (&ui_h, "ui_alarm_action", "i32"),
        (&ui_h, "ui_event_kind", "u32"),
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

/// 50 segments of 1.2 degrees, 7.2 degrees apart from 12 o'clock clockwise, between radius
/// 0.395 and 0.45 of the screen, whose size is ASTEROID_UI_SIZE (the board's).
fn ring(out: &std::path::Path) {
    let size: f64 = std::env::var("ASTEROID_UI_SIZE").ok().and_then(|s| s.parse().ok()).unwrap_or(400.0);
    let dir = out.join("ring");
    std::fs::create_dir_all(&dir).unwrap();
    let mut slint = String::from("export global Ring {\n    out property <[{img: image, x: length, y: length}]> segments: [\n");
    for i in 0..50 {
        let a0 = (-90.0 + i as f64 * 7.2_f64).to_radians();
        let a1 = a0 + 1.2_f64.to_radians();
        let pts: Vec<(f64, f64)> = [(0.395, a0), (0.45, a0), (0.45, a1), (0.395, a1)]
            .iter()
            .map(|&(r, a)| (size / 2.0 + r * size * a.cos(), size / 2.0 + r * size * a.sin()))
            .collect();
        let x0 = pts.iter().map(|p| p.0).fold(f64::MAX, f64::min).floor();
        let y0 = pts.iter().map(|p| p.1).fold(f64::MAX, f64::min).floor();
        let x1 = pts.iter().map(|p| p.0).fold(f64::MIN, f64::max).ceil();
        let y1 = pts.iter().map(|p| p.1).fold(f64::MIN, f64::max).ceil();
        let poly: Vec<String> = pts.iter().map(|p| format!("{:.2},{:.2}", p.0 - x0, p.1 - y0)).collect();
        let svg = format!(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{w}\" height=\"{h}\" viewBox=\"0 0 {w} {h}\"><polygon points=\"{}\" fill=\"white\"/></svg>",
            poly.join(" "),
            w = x1 - x0,
            h = y1 - y0
        );
        let path = dir.join(format!("{i}.svg"));
        std::fs::write(&path, svg).unwrap();
        writeln!(slint, "        {{ img: @image-url(\"{}\"), x: {x0}px, y: {y0}px }},", path.display()).unwrap();
    }
    slint.push_str("    ];\n}\n");
    std::fs::write(out.join("ring.slint"), slint).unwrap();
}
