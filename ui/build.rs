// Compiles the .slint files with fonts and icons embedded as pixel data for the software renderer.
// The notification icons are the AsteroidOS Ionicons set, resized to NOTE_ICON_PX and listed in a
// generated Slint global, with their sorted names for the lookup by name.
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

    let config = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer)
        .with_include_paths(vec![out]);
    slint_build::compile_with_config("ui/watchface.slint", config).unwrap();
    println!("cargo:rerun-if-changed=fonts");
    println!("cargo:rerun-if-changed=icons");
    println!("cargo:rerun-if-changed={ICON_DIR}");
}
