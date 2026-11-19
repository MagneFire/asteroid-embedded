// Slint user interface for the firmware: heap, panic path and the C API (see src/ui/ui.h).
#![no_std]

extern crate alloc;

#[cfg(all(feature = "mcu", feature = "outlines"))]
compile_error!("the mcu build has its glyphs and icons made at build time: build it with --no-default-features");

#[cfg(feature = "mcu")]
mod arc;
#[cfg(feature = "mcu")]
mod fonts;
#[cfg(feature = "mcu")]
mod ring;
#[cfg_attr(feature = "mcu", path = "icon_fields.rs")]
mod icons;
#[cfg(not(feature = "mcu"))]
mod paint;
mod watch;

#[cfg(target_os = "none")]
use core::fmt::Write;
#[cfg(target_os = "none")]
use core::panic::PanicInfo;
use embedded_alloc::TlsfHeap;

#[cfg(not(feature = "mcu"))]
#[global_allocator]
static HEAP: TlsfHeap = TlsfHeap::empty();

#[cfg(not(feature = "mcu"))]
pub(crate) unsafe fn heap_init(start: *mut u8, size: usize) {
    unsafe { HEAP.init(start as usize, size) }
}

/// The heap, counting the bytes allocated from it and their peak.
#[cfg(feature = "mcu")]
struct Counted {
    heap: TlsfHeap,
    used: core::cell::Cell<usize>,
    peak: core::cell::Cell<usize>,
}

// The UI runs on one thread.
#[cfg(feature = "mcu")]
unsafe impl Sync for Counted {}

#[cfg(feature = "mcu")]
impl Counted {
    fn add(&self, n: usize) {
        self.used.set(self.used.get() + n);
        self.peak.set(self.peak.get().max(self.used.get()));
    }
}

#[cfg(feature = "mcu")]
unsafe impl core::alloc::GlobalAlloc for Counted {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        let p = unsafe { self.heap.alloc(layout) };
        if !p.is_null() {
            self.add(layout.size());
        }
        p
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: core::alloc::Layout) {
        unsafe { self.heap.dealloc(ptr, layout) };
        self.used.set(self.used.get() - layout.size());
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: core::alloc::Layout, size: usize) -> *mut u8 {
        let p = unsafe { self.heap.realloc(ptr, layout, size) };
        if !p.is_null() {
            self.used.set(self.used.get() - layout.size());
            self.add(size);
        }
        p
    }
}

#[cfg(feature = "mcu")]
#[global_allocator]
static HEAP: Counted = Counted { heap: TlsfHeap::empty(), used: core::cell::Cell::new(0), peak: core::cell::Cell::new(0) };

#[cfg(feature = "mcu")]
pub(crate) unsafe fn heap_init(start: *mut u8, size: usize) {
    unsafe { HEAP.heap.init(start as usize, size) }
}

/// The most bytes the UI has had allocated from its heap at once, not counting the allocator's
/// own overhead.
#[cfg(feature = "mcu")]
#[unsafe(no_mangle)]
pub extern "C" fn ui_heap_peak() -> u32 {
    HEAP.peak.get() as u32
}

struct IrqMask;
critical_section::set_impl!(IrqMask);

#[cfg(all(target_arch = "arm", not(target_feature = "mclass")))]
unsafe impl critical_section::Impl for IrqMask {
    unsafe fn acquire() -> u32 {
        let cpsr: u32;
        unsafe { core::arch::asm!("mrs {0}, cpsr", "cpsid i", out(reg) cpsr) };
        cpsr
    }

    unsafe fn release(cpsr: u32) {
        if cpsr & 0x80 == 0 {
            unsafe { core::arch::asm!("cpsie i") };
        }
    }
}

#[cfg(all(target_arch = "arm", target_feature = "mclass"))]
unsafe impl critical_section::Impl for IrqMask {
    unsafe fn acquire() -> u32 {
        let primask: u32;
        unsafe { core::arch::asm!("mrs {0}, primask", "cpsid i", out(reg) primask) };
        primask
    }

    unsafe fn release(primask: u32) {
        if primask & 1 == 0 {
            unsafe { core::arch::asm!("cpsie i") };
        }
    }
}

// A host build, for measuring the UI off the watch: one thread, no interrupts.
#[cfg(not(target_arch = "arm"))]
unsafe impl critical_section::Impl for IrqMask {
    unsafe fn acquire() -> u32 {
        0
    }

    unsafe fn release(_: u32) {}
}

#[cfg(target_os = "none")]
unsafe extern "C" {
    fn ui_panic(msg: *const u8, len: usize) -> !;
}

// The host build has std and its panic handler.
#[cfg(target_os = "none")]
struct Message {
    buf: [u8; 512],
    len: usize,
}

#[cfg(target_os = "none")]
impl Write for Message {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

#[cfg(target_os = "none")]
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let mut msg = Message { buf: [0; 512], len: 0 };
    match info.location() {
        Some(l) => {
            let _ = write!(msg, "{}:{}:{}: {}", l.file(), l.line(), l.column(), info.message());
        }
        None => {
            let _ = write!(msg, "{}", info.message());
        }
    }
    unsafe { ui_panic(msg.buf.as_ptr(), msg.len) }
}
