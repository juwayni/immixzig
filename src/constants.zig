//! Immix GC architecture constants derived from source specifications.

pub const BLOCK_BITS: u8 = 15;
pub const BLOCK_SIZE: usize = 1 << BLOCK_BITS; // 32,768 bytes
pub const BLOCK_MASK: usize = BLOCK_SIZE - 1;

pub const LINE_BITS: u8 = 7;
pub const LINE_SIZE: usize = 1 << LINE_BITS; // 128 bytes
pub const LINE_COUNT: usize = BLOCK_SIZE / LINE_SIZE; // 256 lines

pub const HEADER_LINES: usize = LINE_COUNT >> LINE_BITS; // 2 lines = 256 bytes reserved
pub const USEFUL_LINES: usize = LINE_COUNT - HEADER_LINES; // 254 lines usable for allocations

pub const HEADER_SIZE: usize = 4; // 4-byte u32 header immediately preceding payload
pub const LARGE_OBJECT_SIZE: usize = 4000;
pub const MAX_HOLES: usize = USEFUL_LINES / 2;
pub const FRAG_THRESH: usize = 14;

pub const SIZE_SHIFT: u5 = 6;
pub const MARK_MASK: u32 = 0x3f00_0000;
pub const SIZE_MASK: u32 = 0x000f_ff00;
pub const ROW_COUNT_MASK: u32 = 0x0000_00ff;
pub const CONTAINER_FLAG: u32 = 0x0080_0000;
pub const HEADER_PRESERVE: u32 = 0x00f0_0000;
pub const MOVED_SENTINEL: u32 = 0x0000_00fe;

pub const DEFAULT_MARK_ID: u32 = 0x1000_0000;
pub const DEFAULT_MARK_BYTE: u8 = 0x10;
pub const OBJECT_ALIGNMENT: usize = 4;

comptime {
    if (BLOCK_SIZE != 32768) @compileError("Block size must be exactly 32KB");
    if (LINE_SIZE != 128) @compileError("Line size must be exactly 128 bytes");
    if (LINE_COUNT != 256) @compileError("Expected 256 lines in a 32KB block");
    if (HEADER_LINES != 2) @compileError("Expected 2 header lines");
    if (HEADER_SIZE != 4) @compileError("Header size must be 4 bytes");
}
