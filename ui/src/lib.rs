// Slint user interface for the firmware: heap, panic path and the C API (see src/ui/ui.h).
#![no_std]

extern crate alloc;

mod watch;

use core::fmt::Write;
use core::panic::PanicInfo;
use embedded_alloc::TlsfHeap;

#[global_allocator]
static HEAP: TlsfHeap = TlsfHeap::empty();

pub(crate) unsafe fn heap_init(start: *mut u8, size: usize) {
    unsafe { HEAP.init(start as usize, size) }
}

struct IrqMask;
critical_section::set_impl!(IrqMask);

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

unsafe extern "C" {
    fn ui_panic(msg: *const u8, len: usize) -> !;
}

struct Message {
    buf: [u8; 512],
    len: usize,
}

impl Write for Message {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

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
