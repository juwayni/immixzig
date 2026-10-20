//! Data types and function pointers for the Immix allocator module.

const std = @import("std");

pub const AllocError = error{
    OutOfMemory,
    HeapLimitReached,
    ObjectTooLarge,
    InvalidPointer,
    CorruptHeap,
    CollectionInProgress,
};

pub const HoleRange = struct {
    start: u16 = 0,
    length: u16 = 0,
};

pub const ObjectFlags = struct {
    container: bool = false,
    preserved: u32 = 0,
};

pub const VisitSlotFn = *const fn (context: *anyopaque, slot: *usize) void;

pub const TraceObjectFn = *const fn (
    context: *anyopaque,
    payload: []u8,
    visit_slot: VisitSlotFn,
    visit_context: *anyopaque,
) void;

pub const Config = struct {
    max_blocks: usize = 0,
    zero_allocations: bool = true,
    max_interior_offset: usize = 0,
};

pub const Stats = struct {
    blocks: usize = 0,
    objects_before: usize = 0,
    objects_live: usize = 0,
    objects_reclaimed: usize = 0,
    objects_moved: usize = 0,
    bytes_before: usize = 0,
    bytes_live: usize = 0,
    rows_in_use: usize = 0,
    bytes_in_use: usize = 0,
    empty_blocks: usize = 0,
    fragmented_blocks: usize = 0,
    fragmentation_score: usize = 0,
};
