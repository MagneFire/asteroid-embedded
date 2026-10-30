// A layer as Slint's render target with the drawing handed to the GPU (src/gpu/paint.h): plain and
// rounded rectangles, glyphs and images go to the GPU, anything else (gradients, rotations) is left
// to the software renderer, which then draws into the layer itself, as it does everything while the
// GPU is off. Before the CPU touches a row the GPU's work is finished, and rows the CPU wrote are
// flushed before the GPU draws again.
use slint::platform::software_renderer::{
    DrawRectangleArgs, DrawTextureArgs, PhysicalRegion, PremultipliedRgbaColor, RenderingRotation, TargetPixelBuffer,
    TexturePixelFormat,
};
use slint::{Brush, Color};

unsafe extern "C" {
    fn paint_begin(layer: *mut u32, width: u32, height: u32, stride_px: u32) -> bool;
    fn paint_rect(x0: f32, y0: f32, x1: f32, y1: f32, clip: *const i32, color: u32, copy: bool);
    fn paint_texture(
        data: *const u8,
        stride: u32,
        width: u32,
        height: u32,
        format: i32,
        dx: i32,
        dy: i32,
        dw: i32,
        dh: i32,
        clip: *const i32,
        tint: u32,
        colorize: bool,
    ) -> bool;
    fn paint_rounded(
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        radii: *const f32,
        border: f32,
        clip: *const i32,
        fill: u32,
        edge: u32,
    ) -> bool;
    fn paint_cpu(y0: u32, y1: u32);
    fn paint_sync();
    fn paint_end();
    fn paint_finish();
}

const PAINT_RGB: i32 = 0;
const PAINT_RGBA: i32 = 1;
const PAINT_RGBA_PREMULTIPLIED: i32 = 2;
const PAINT_ALPHA: i32 = 3;

pub struct GpuLayer<'a> {
    layer: &'a mut [PremultipliedRgbaColor],
    stride: usize,
    height: usize,
    on: bool,
    gpu_busy: bool,
    cpu_rows: (usize, usize),
}

impl<'a> GpuLayer<'a> {
    pub fn new(layer: &'a mut [PremultipliedRgbaColor], stride: usize, width: usize, height: usize) -> Self {
        let on = unsafe { paint_begin(layer.as_mut_ptr() as *mut u32, width as u32, height as u32, stride as u32) };
        Self { layer, stride, height, on, gpu_busy: false, cpu_rows: (height, 0) }
    }

    fn gpu(&mut self) {
        let (y0, y1) = self.cpu_rows;
        if y0 < y1 {
            unsafe { paint_cpu(y0 as u32, y1 as u32) };
            self.cpu_rows = (self.height, 0);
        }
        self.gpu_busy = true;
    }

    fn clips(region: &PhysicalRegion) -> impl Iterator<Item = [i32; 4]> + '_ {
        region.iter().map(|(p, s)| [p.x, p.y, p.x + s.width as i32, p.y + s.height as i32])
    }
}

impl Drop for GpuLayer<'_> {
    fn drop(&mut self) {
        if !self.on {
            return;
        }
        let (y0, y1) = self.cpu_rows;
        if y0 < y1 {
            unsafe { paint_cpu(y0 as u32, y1 as u32) };
        }
        unsafe { paint_end() };
    }
}

/// Waits for the GPU to finish every layer drawn so far.
pub fn finish() {
    unsafe { paint_finish() };
}

fn premultiplied(c: Color, alpha: u8) -> u32 {
    let a = c.alpha() as u32 * alpha as u32 / 255;
    let (r, g, b) = (c.red() as u32 * a / 255, c.green() as u32 * a / 255, c.blue() as u32 * a / 255);
    r | g << 8 | b << 16 | a << 24
}

fn round(v: f32) -> f32 {
    (if v < 0.0 { v - 0.5 } else { v + 0.5 }) as i32 as f32
}

fn solid(brush: &Brush) -> Option<Color> {
    match brush {
        Brush::SolidColor(c) => Some(*c),
        _ => None,
    }
}

impl TargetPixelBuffer for GpuLayer<'_> {
    type TargetPixel = PremultipliedRgbaColor;

    fn line_slice(&mut self, line: usize) -> &mut [PremultipliedRgbaColor] {
        if self.gpu_busy {
            unsafe { paint_sync() };
            self.gpu_busy = false;
        }
        self.cpu_rows = (self.cpu_rows.0.min(line), self.cpu_rows.1.max(line + 1));
        &mut self.layer[line * self.stride..(line + 1) * self.stride]
    }

    fn num_lines(&self) -> usize {
        self.height
    }

    fn fill_background(&mut self, brush: &Brush, region: &PhysicalRegion) -> bool {
        if !self.on {
            return false;
        }
        let Some(color) = solid(brush) else { return false };
        self.gpu();
        let color = premultiplied(color, 255);
        for clip in Self::clips(region) {
            let r = clip.map(|v| v as f32);
            unsafe { paint_rect(r[0], r[1], r[2], r[3], clip.as_ptr(), color, true) };
        }
        true
    }

    fn draw_rectangle(&mut self, args: &DrawRectangleArgs, clip: &PhysicalRegion) -> bool {
        if !self.on {
            return false;
        }
        let radii = [args.top_left_radius, args.top_right_radius, args.bottom_right_radius, args.bottom_left_radius];
        if args.rotation != RenderingRotation::NoRotation {
            return false;
        }
        let (Some(background), Some(border)) = (solid(&args.background), solid(&args.border)) else { return false };
        let color = premultiplied(background, args.alpha);
        let mut border_color = premultiplied(border, args.alpha);
        let b = if border_color >> 24 == 0 { 0.0 } else { args.border_width as i32 as f32 };
        if border_color >> 24 != 0 && border_color >> 24 < 255 {
            let ba = border_color >> 24;
            let mix = |shift: u32| ((color >> shift & 0xff) * (255 - ba) / 255 + (border_color >> shift & 0xff)) << shift;
            let alpha = (color >> 24) + ba - (color >> 24) * ba / 255;
            border_color = mix(0) | mix(8) | mix(16) | alpha << 24;
        }
        self.gpu();
        let (x0, y0) = (round(args.x), round(args.y));
        let (x1, y1) = (round(args.x + args.width), round(args.y + args.height));
        let rounded = radii.iter().any(|&r| r > 0.0);
        for clip in Self::clips(clip) {
            let c = clip.as_ptr();
            if rounded {
                let edge = if b > 0.0 { border_color } else { color };
                if !unsafe { paint_rounded(x0, y0, x1, y1, radii.as_ptr(), b, c, color, edge) } {
                    return false;
                }
                continue;
            }
            unsafe {
                if color >> 24 != 0 && x1 - x0 > 2.0 * b && y1 - y0 > 2.0 * b {
                    paint_rect(x0 + b, y0 + b, x1 - b, y1 - b, c, color, false);
                }
                if b > 0.0 {
                    paint_rect(x0, y0, x1, y0 + b, c, border_color, false);
                    paint_rect(x0, y1 - b, x1, y1, c, border_color, false);
                    paint_rect(x0, y0 + b, x0 + b, y1 - b, c, border_color, false);
                    paint_rect(x1 - b, y0 + b, x1, y1 - b, c, border_color, false);
                }
            }
        }
        true
    }

    fn draw_texture(&mut self, args: &DrawTextureArgs, clip: &PhysicalRegion) -> bool {
        if !self.on {
            return false;
        }
        if args.rotation != RenderingRotation::NoRotation || args.tiling.is_some() {
            return false;
        }
        let source = args.source();
        let format = match source.pixel_format {
            TexturePixelFormat::Rgb => PAINT_RGB,
            TexturePixelFormat::Rgba => PAINT_RGBA,
            TexturePixelFormat::RgbaPremultiplied => PAINT_RGBA_PREMULTIPLIED,
            TexturePixelFormat::AlphaMap => PAINT_ALPHA,
            _ => return false,
        };
        let tint = match args.colorize {
            Some(c) => premultiplied(Color::from_rgb_u8(c.red(), c.green(), c.blue()), args.alpha),
            None if format == PAINT_ALPHA => return false,
            None => (args.alpha as u32) * 0x01010101,
        };
        self.gpu();
        let mut ok = true;
        for clip in Self::clips(clip) {
            ok &= unsafe {
                paint_texture(
                    source.data.as_ptr(),
                    source.byte_stride as u32,
                    source.width,
                    source.height,
                    format,
                    args.dst_x as i32,
                    args.dst_y as i32,
                    args.dst_width as i32,
                    args.dst_height as i32,
                    clip.as_ptr(),
                    tint,
                    args.colorize.is_some() && format != PAINT_ALPHA,
                )
            };
        }
        ok
    }
}
