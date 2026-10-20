//! Standalone Immix allocator/collector port based on hxcpp Immix.cpp + GC.h.
//! Target Zig: 0.13.x. No hxcpp types, C++ containers, stack scanning or TLS.
//!
//! Source-derived constants (default, non-HXCPP_GC_BIG_BLOCKS build):
//!   block = 1 << 15 bytes; line = 1 << 7 bytes; 256 lines; 2 header lines.
//!   object header = 32 bits immediately before payload; size shift = 6;
//!   object/container flag = 0x00800000; mark mask = 0x3f000000.
//!
//! This module uses explicit roots and a VM-supplied strong-reference visitor.
//! It ports block allocation, start maps, mark-byte/header semantics, line
//! marking, hole reconstruction, fragmentation scoring and sweep/reclaim.
//! Concurrent workers, Haxe finalizers/weak refs, nursery mode, and relocation
//! are deliberately separate layers; this implementation never moves objects.

const std = @import("std");

pub const BLOCK_BITS: u8 = 15;
pub const BLOCK_SIZE: usize = 1 << BLOCK_BITS;
pub const BLOCK_MASK: usize = BLOCK_SIZE - 1;
pub const LINE_BITS: u8 = 7;
pub const LINE_SIZE: usize = 1 << LINE_BITS;
pub const LINE_COUNT: usize = BLOCK_SIZE / LINE_SIZE;
pub const HEADER_LINES: usize = LINE_COUNT >> LINE_BITS;
pub const USEFUL_LINES: usize = LINE_COUNT - HEADER_LINES;
pub const HEADER_SIZE: usize = 4;
pub const LARGE_OBJECT_SIZE: usize = 4000;
pub const MAX_HOLES: usize = USEFUL_LINES / 2;
pub const FRAG_THRESH: usize = 14;
pub const SIZE_SHIFT: u5 = 6;
pub const MARK_MASK: u32 = 0x3f00_0000;
pub const SIZE_MASK: u32 = 0x000f_ff00;
pub const ROW_COUNT_MASK: u32 = 0x0000_00ff;
pub const CONTAINER_FLAG: u32 = 0x0080_0000;
pub const HEADER_PRESERVE: u32 = 0x00f0_0000;
pub const DEFAULT_MARK_ID: u32 = 0x1000_0000;
pub const DEFAULT_MARK_BYTE: u8 = 0x10;
pub const OBJECT_ALIGNMENT: usize = 4;

comptime {
    if (LINE_COUNT != 256) @compileError("expected 256 lines in a 32 KiB block");
    if (HEADER_LINES != 2) @compileError("hxcpp default reserves two header lines");
}

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
    /// Maximum distance from an object payload pointer accepted as an interior
    /// reference. hxcpp defaults to a small internal offset (platform-defined).
    max_interior_offset: usize = 0,
};

pub const Stats = struct {
    blocks: usize = 0,
    objects_before: usize = 0,
    objects_live: usize = 0,
    objects_reclaimed: usize = 0,
    bytes_before: usize = 0,
    bytes_live: usize = 0,
    rows_in_use: usize = 0,
    bytes_in_use: usize = 0,
    empty_blocks: usize = 0,
    fragmented_blocks: usize = 0,
    fragmentation_score: usize = 0,
};

/// Layout of a single 128-byte Immix line.
pub const Line = struct {
    bytes: [LINE_SIZE]u8,

    pub const BITS: u8 = LINE_BITS;
    pub const SIZE: usize = LINE_SIZE;
};

/// One Immix block. The first 256 bytes of `memory` mirror hxcpp's row-mark
/// overlay; the first two 128-byte lines are not available for allocations.
pub const Block = struct {
    memory: []align(BLOCK_SIZE) u8,
    id: u16,
    alloc_start: [LINE_COUNT]u32 = @import("std").mem.zeroes([LINE_COUNT]u32),
    row_marked: [LINE_COUNT]u8 = @import("std").mem.zeroes([LINE_COUNT]u8),
    holes: [MAX_HOLES]HoleRange = @import("std").mem.zeroes([MAX_HOLES]HoleRange),
    hole_count: usize = 1,
    used_rows: usize = 0,
    max_hole_size: usize = USEFUL_LINES * LINE_SIZE,
    move_score: usize = 0,
    used_bytes: usize = 0,
    fragged_rows: usize = 0,
    pinned: bool = false,
    zeroed: bool = false,
    reclaimed: bool = true,
    owned: bool = false,
    cursor_hole: usize = 0,
    cursor: usize = HEADER_LINES * LINE_SIZE,

    pub fn init(allocator: std.mem.Allocator, id: u16) AllocError!Block {
        const mem = allocator.alignedAlloc(u8, .fromByteUnits(BLOCK_SIZE), BLOCK_SIZE) catch return AllocError.OutOfMemory;
        @memset(mem, 0);
        var result = Block{ .memory = mem, .id = id };
        result.holes[0] = .{ .start = @intCast(HEADER_LINES * LINE_SIZE), .length = @intCast(USEFUL_LINES * LINE_SIZE) };
        std.mem.writeInt(u16, @ptrCast(mem.ptr), id, .little);
        return result;
    }

    pub fn deinit(self: *Block, allocator: std.mem.Allocator) void {
        allocator.free(self.memory);
        self.* = undefined;
    }

    pub fn base(self: *const Block) usize {
        return @intFromPtr(self.memory.ptr);
    }

    pub fn contains(self: *const Block, ptr: usize) bool {
        return ptr >= self.base() + HEADER_LINES * LINE_SIZE and ptr < self.base() + BLOCK_SIZE;
    }

    fn syncRowMarks(self: *Block) void {
        @memcpy(self.memory[0..LINE_COUNT], self.row_marked[0..]);
        std.mem.writeInt(u16, @ptrCast(self.memory.ptr), self.id, .little);
    }

    fn startBit(offset: usize) u32 {
        return @as(u32, 1) << @as(u5, @intCast((offset & (LINE_SIZE - 1)) / OBJECT_ALIGNMENT));
    }

    fn rowOf(offset: usize) usize {
        return offset >> LINE_BITS;
    }

    fn readHeader(self: *const Block, header_offset: usize) u32 {
        return std.mem.readInt(u32, @ptrCast(self.memory.ptr + header_offset), .little);
    }

    fn writeHeader(self: *Block, header_offset: usize, value: u32) void {
        std.mem.writeInt(u32, @ptrCast(self.memory.ptr + header_offset), value, .little);
    }

    fn setStart(self: *Block, offset: usize) void {
        self.alloc_start[rowOf(offset)] |= startBit(offset);
    }

    fn clearStart(self: *Block, offset: usize) void {
        self.alloc_start[rowOf(offset)] &= ~startBit(offset);
    }

    fn hasStart(self: *const Block, offset: usize) bool {
        if (offset >= BLOCK_SIZE) return false;
        return (self.alloc_start[rowOf(offset)] & startBit(offset)) != 0;
    }

    /// Equivalent to hxcpp's GetFreeRows/GetFreeData and range selection.
    pub fn freeRows(self: *const Block) usize {
        return USEFUL_LINES - self.used_rows;
    }

    pub fn freeBytes(self: *const Block) usize {
        return self.freeRows() * LINE_SIZE;
    }

    pub fn isEmpty(self: *const Block) bool {
        return self.used_rows == 0;
    }

    /// Reserve a contiguous allocation from the current hole, then advance
    /// through the block's remaining hole list. The size includes the 4-byte
    /// hxcpp header. Returned offset points to that header.
    fn reserve(self: *Block, total_size: usize) ?usize {
        if (total_size == 0 or total_size >= LARGE_OBJECT_SIZE) return null;
        while (self.cursor_hole < self.hole_count) {
            const hole = self.holes[self.cursor_hole];
            const begin: usize = hole.start;
            const end = begin + hole.length;
            if (self.cursor < begin or self.cursor > end) self.cursor = begin;
            const aligned = std.mem.alignForward(usize, self.cursor, OBJECT_ALIGNMENT);
            if (aligned <= end and total_size <= end - aligned) {
                self.cursor = aligned + total_size;
                return aligned;
            }
            self.cursor_hole += 1;
            if (self.cursor_hole < self.hole_count)
                self.cursor = self.holes[self.cursor_hole].start;
        }
        return null;
    }

    fn noteAllocation(self: *Block, offset: usize, payload_size: usize, flags: ObjectFlags, mark_id: u32, mark_byte: u8) void {
        const total = std.mem.alignForward(usize, HEADER_SIZE + payload_size, OBJECT_ALIGNMENT);
        const first_row = rowOf(offset);
        const end_row = (offset + total + LINE_SIZE - 1) >> LINE_BITS;
        const rows = end_row - first_row;
        var header = (mark_id & MARK_MASK) |
            (@as(u32, @intCast(payload_size)) << SIZE_SHIFT) |
            @as(u32, @intCast(rows & 0xff)) |
            (flags.preserved & HEADER_PRESERVE);
        if (flags.container) header |= CONTAINER_FLAG;
        self.writeHeader(offset, header);
        self.setStart(offset);
        self.memory[offset + 3] = mark_byte;
        var r = first_row;
        while (r < end_row) : (r += 1) self.row_marked[r] = 1;
        self.syncRowMarks();
        self.reclaimed = false;
        self.zeroed = false;
    }

    fn objectOffsetForPayload(self: *const Block, payload: usize, max_interior: usize) ?usize {
        if (!self.contains(payload)) return null;
        const block_offset = payload - self.base();
        var back: usize = 0;
        while (back <= max_interior and back <= block_offset) : (back += OBJECT_ALIGNMENT) {
            const candidate = block_offset - HEADER_SIZE - back;
            if (candidate < HEADER_LINES * LINE_SIZE or candidate >= BLOCK_SIZE - HEADER_SIZE) continue;
            if (!self.hasStart(candidate)) continue;
            const header = self.readHeader(candidate);
            const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
            if (size == 0 or candidate + HEADER_SIZE + size > BLOCK_SIZE) continue;
            if (payload >= self.base() + candidate + HEADER_SIZE and
                payload < self.base() + candidate + HEADER_SIZE + size)
                return candidate;
        }
        return null;
    }

    fn markObject(self: *Block, header_offset: usize, mark_id: u32, mark_byte: u8) bool {
        if (!self.hasStart(header_offset)) return false;
        var header = self.readHeader(header_offset);
        if ((header & MARK_MASK) == (mark_id & MARK_MASK)) return false;
        const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
        const rows = @as(usize, header & ROW_COUNT_MASK);
        if (size == 0 or rows == 0 or header_offset + HEADER_SIZE + size > BLOCK_SIZE) return false;
        header = (header & ~MARK_MASK) | (mark_id & MARK_MASK);
        self.writeHeader(header_offset, header);
        self.memory[header_offset + 3] = mark_byte;
        const first = rowOf(header_offset);
        var r = first;
        while (r < first + rows and r < LINE_COUNT) : (r += 1) self.row_marked[r] = 1;
        self.syncRowMarks();
        return true;
    }

    /// Rebuild free holes from marked row bytes. Mirrors BlockDataInfo::reclaim.
    /// If `full` is true, dead allocation-start entries are also removed and
    /// live payload bytes are counted from their headers.
    pub fn reclaim(self: *Block, full: bool, mark_id: u32) Stats {
        var stats = Stats{};
        self.hole_count = 0;
        self.max_hole_size = 0;
        self.used_bytes = 0;
        var row: usize = HEADER_LINES;
        var free_lines: usize = 0;

        while (row < LINE_COUNT) {
            if (self.row_marked[row] != 0) {
                if (full and self.alloc_start[row] != 0) {
                    var bits = self.alloc_start[row];
                    while (bits != 0) {
                        const bit: u5 = @intCast(@ctz(bits));
                        const offset = row * LINE_SIZE + @as(usize, bit) * OBJECT_ALIGNMENT;
                        const header = self.readHeader(offset);
                        if ((header & MARK_MASK) == (mark_id & MARK_MASK)) {
                            self.used_bytes += HEADER_SIZE + @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
                        } else {
                            self.alloc_start[row] &= ~(@as(u32, 1) << bit);
                        }
                        bits &= bits - 1;
                    }
                }
                row += 1;
                continue;
            }

            const hole_start = row;
            while (row < LINE_COUNT and self.row_marked[row] == 0) : (row += 1) {}
            const len = row - hole_start;
            if (len == 0) continue;
            if (self.hole_count < MAX_HOLES) {
                self.holes[self.hole_count] = .{
                    .start = @intCast(hole_start * LINE_SIZE),
                    .length = @intCast(len * LINE_SIZE),
                };
                self.hole_count += 1;
            }
            free_lines += len;
            self.max_hole_size = @max(self.max_hole_size, len * LINE_SIZE);
            @memset(self.alloc_start[hole_start .. hole_start + len], 0);
        }

        self.used_rows = USEFUL_LINES - free_lines;
        if (!full) self.used_bytes = self.used_rows * LINE_SIZE;
        self.move_score = self.calcFragScore();
        self.reclaimed = true;
        self.zeroed = false;
        self.cursor_hole = 0;
        self.cursor = if (self.hole_count > 0) self.holes[0].start else BLOCK_SIZE;
        if (self.used_rows == 0) {
            self.hole_count = 1;
            self.holes[0] = .{ .start = @intCast(HEADER_LINES * LINE_SIZE), .length = @intCast(USEFUL_LINES * LINE_SIZE) };
            self.max_hole_size = USEFUL_LINES * LINE_SIZE;
            self.move_score = 0;
            self.cursor = HEADER_LINES * LINE_SIZE;
            self.cursor_hole = 0;
        }
        stats.rows_in_use = self.used_rows;
        stats.bytes_in_use = self.used_bytes;
        if (self.isEmpty()) stats.empty_blocks = 1;
        if (self.move_score > FRAG_THRESH) stats.fragmented_blocks = 1;
        stats.fragmentation_score = self.move_score;
        return stats;
    }

    pub fn calcFragScore(self: *const Block) usize {
        if (self.pinned) return 0;
        const hole_penalty = if (self.hole_count > 3) self.hole_count - 3 else 0;
        return hole_penalty + (8 * self.used_rows * LINE_SIZE) / (self.used_bytes + LINE_SIZE);
    }

    pub fn clearMarks(self: *Block) void {
        @memset(self.row_marked[HEADER_LINES..], 0);
        self.syncRowMarks();
        self.pinned = false;
    }

    pub fn pin(self: *Block) void {
        self.pinned = true;
        self.move_score = 0;
    }
};

const WorkItem = struct { block_index: usize, header_offset: usize };

pub const ImmixAllocator = struct {
    allocator: std.mem.Allocator,
    config: Config,
    blocks: std.ArrayListUnmanaged(Block) = .empty,
    worklist: std.ArrayListUnmanaged(WorkItem) = .empty,
    trace_fn: ?TraceObjectFn = null,
    trace_context: ?*anyopaque = null,
    mark_id: u32 = DEFAULT_MARK_ID,
    mark_byte: u8 = DEFAULT_MARK_BYTE,
    collecting: bool = false,
    pause_requested: bool = false,
    bytes_allocated: usize = 0,
    collection_count: usize = 0,

    pub fn init(allocator: std.mem.Allocator, config: Config) ImmixAllocator {
        return .{ .allocator = allocator, .config = config };
    }

    pub fn deinit(self: *ImmixAllocator) void {
        for (self.blocks.items) |*block| block.deinit(self.allocator);
        self.blocks.deinit(self.allocator);
        self.worklist.deinit(self.allocator);
        self.* = undefined;
    }

    pub fn setTracer(self: *ImmixAllocator, tracer: TraceObjectFn, context: *anyopaque) void {
        self.trace_fn = tracer;
        self.trace_context = context;
    }

    pub fn requestSafepoint(self: *ImmixAllocator) void {
        self.pause_requested = true;
    }

    pub fn releaseSafepoint(self: *ImmixAllocator) void {
        self.pause_requested = false;
    }

    pub fn blockCount(self: *const ImmixAllocator) usize {
        return self.blocks.items.len;
    }

    fn createBlock(self: *ImmixAllocator) AllocError!void {
        if (self.config.max_blocks != 0 and self.blocks.items.len >= self.config.max_blocks)
            return AllocError.HeapLimitReached;
        if (self.blocks.items.len >= std.math.maxInt(u16)) return AllocError.HeapLimitReached;
        var block = try Block.init(self.allocator, @intCast(self.blocks.items.len));
        errdefer block.deinit(self.allocator);
        try self.blocks.append(self.allocator, block);
    }

    /// Fast-path bump allocation across the selected block's ordered hole list.
    /// Returns the payload pointer; the 32-bit hxcpp-style header is stored at
    /// payload[-4]. Large allocations are deliberately rejected here and belong
    /// in the separate large-object pool (hxcpp's GlobalAllocator::AllocLarge).
    pub fn alloc(self: *ImmixAllocator, size_in: usize, flags: ObjectFlags) AllocError![]u8 {
        if (self.collecting) return AllocError.CollectionInProgress;
        if (size_in == 0) return self.allocator.alloc(u8, 0);
        const size = std.mem.alignForward(usize, size_in, OBJECT_ALIGNMENT);
        if (size >= LARGE_OBJECT_SIZE or size > 0x3ffc) return AllocError.ObjectTooLarge;
        const total = HEADER_SIZE + size;

        var i: usize = 0;
        while (i < self.blocks.items.len) : (i += 1) {
            const block = &self.blocks.items[i];
            if (block.owned) continue;
            if (block.reserve(total)) |offset| {
                block.noteAllocation(offset, size, flags, self.mark_id, self.mark_byte);
                const payload = block.memory[offset + HEADER_SIZE .. offset + HEADER_SIZE + size_in];
                if (self.config.zero_allocations) @memset(payload, 0);
                self.bytes_allocated += size;
                return payload;
            }
        }

        try self.createBlock();
        const index = self.blocks.items.len - 1;
        const block = &self.blocks.items[index];
        const offset = block.reserve(total) orelse return AllocError.CorruptHeap;
        block.noteAllocation(offset, size, flags, self.mark_id, self.mark_byte);
        const payload = block.memory[offset + HEADER_SIZE .. offset + HEADER_SIZE + size_in];
        if (self.config.zero_allocations) @memset(payload, 0);
        self.bytes_allocated += size;
        return payload;
    }

    fn locateHeader(self: *ImmixAllocator, address: usize) ?WorkItem {
        for (self.blocks.items, 0..) |*block, bi| {
            if (!block.contains(address)) continue;
            const offset = block.objectOffsetForPayload(address, self.config.max_interior_offset) orelse return null;
            return .{ .block_index = bi, .header_offset = offset };
        }
        return null;
    }

    fn markAddress(self: *ImmixAllocator, address: usize) AllocError!void {
        if (address == 0) return;
        const item = self.locateHeader(address) orelse return;
        const block = &self.blocks.items[item.block_index];
        if (block.markObject(item.header_offset, self.mark_id, self.mark_byte)) {
            try self.worklist.append(self.allocator, item);
        }
    }

    const VisitContext = struct {
        heap: *ImmixAllocator,
        failure: ?AllocError = null,
    };

    fn visitSlot(context_ptr: *anyopaque, slot: *usize) void {
        const context: *VisitContext = @ptrCast(@alignCast(context_ptr));
        if (context.failure != null or slot.* == 0) return;
        context.heap.markAddress(slot.*) catch |err| {
            context.failure = err;
        };
    }

    fn advanceMarkEpoch(self: *ImmixAllocator) void {
        var next = (self.mark_id + 0x0100_0000) & MARK_MASK;
        if (next == 0) next = 0x0100_0000;
        self.mark_id = next;
        self.mark_byte +%= 1;
        if (self.mark_byte == 0) self.mark_byte = 1;
    }

    /// Stop-the-world explicit-root mark + trace + line sweep/reclaim.
    /// roots is a mutable slice of pointer values; this version is non-moving,
    /// so it does not rewrite them. The visitor must enumerate every strong
    /// pointer field in each object, including nested arrays and containers.
    pub fn collect(self: *ImmixAllocator, roots: []usize) AllocError!Stats {
        if (self.collecting) return AllocError.CollectionInProgress;
        self.collecting = true;
        self.pause_requested = true;
        defer {
            self.collecting = false;
            self.pause_requested = false;
        }

        self.advanceMarkEpoch();
        self.worklist.clearRetainingCapacity();
        var stats = Stats{ .blocks = self.blocks.items.len };
        for (self.blocks.items) |*block| {
            block.clearMarks();
            for (block.alloc_start, 0..) |starts, row| {
                var bits = starts;
                while (bits != 0) {
                    const bit: usize = @intCast(@ctz(bits));
                    const offset = row * LINE_SIZE + bit * OBJECT_ALIGNMENT;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
                    if (size != 0 and offset + HEADER_SIZE + size <= BLOCK_SIZE) {
                        stats.objects_before += 1;
                        stats.bytes_before += size;
                    }
                    bits &= bits - 1;
                }
            }
        }

        for (roots) |root| try self.markAddress(root);
        if (self.trace_fn) |tracer| {
            var visit_context = VisitContext{ .heap = self };
            while (self.worklist.items.len != 0) {
                const item = self.worklist.pop().?;
                const block = &self.blocks.items[item.block_index];
                const header = block.readHeader(item.header_offset);
                const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
                const payload = block.memory[item.header_offset + HEADER_SIZE .. item.header_offset + HEADER_SIZE + size];
                tracer(self.trace_context.?, payload, visitSlot, @ptrCast(&visit_context));
                if (visit_context.failure) |err| return err;
            }
        } else if (roots.len != 0) {
            // Roots alone are not enough to trace object graphs. Rejecting this
            // configuration prevents silently collecting children of live roots.
            return AllocError.CorruptHeap;
        }

        var i: usize = 0;
        while (i < self.blocks.items.len) {
            const block = &self.blocks.items[i];
            var live_objects: usize = 0;
            var live_bytes: usize = 0;
            for (block.alloc_start, 0..) |starts, row| {
                var bits = starts;
                while (bits != 0) {
                    const bit: usize = @intCast(@ctz(bits));
                    const offset = row * LINE_SIZE + bit * OBJECT_ALIGNMENT;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
                    if (size != 0 and (header & MARK_MASK) == self.mark_id) {
                        live_objects += 1;
                        live_bytes += size;
                    }
                    bits &= bits - 1;
                }
            }
            stats.objects_live += live_objects;
            stats.bytes_live += live_bytes;
            const block_stats = block.reclaim(true, self.mark_id);
            stats.rows_in_use += block_stats.rows_in_use;
            stats.bytes_in_use += block_stats.bytes_in_use;
            stats.empty_blocks += block_stats.empty_blocks;
            stats.fragmented_blocks += block_stats.fragmented_blocks;
            stats.fragmentation_score += block_stats.fragmentation_score;
            if (block.isEmpty()) {
                block.deinit(self.allocator);
                _ = self.blocks.orderedRemove(i);
            } else {
                i += 1;
            }
        }
        stats.objects_reclaimed = stats.objects_before - stats.objects_live;
        stats.objects_reclaimed = stats.objects_before - stats.objects_live;
        stats.blocks = self.blocks.items.len;
        self.bytes_allocated = stats.bytes_live;
        self.collection_count += 1;
        return stats;
    }

    /// Verify block alignment, reserved rows, hole bounds, and start-map/header
    /// consistency. Intended for tests and debug builds.
    pub fn validate(self: *ImmixAllocator) AllocError!void {
        for (self.blocks.items) |*block| {
            if (block.base() % BLOCK_SIZE != 0) return AllocError.CorruptHeap;
            if (block.hole_count > MAX_HOLES) return AllocError.CorruptHeap;
            for (block.holes[0..block.hole_count]) |hole| {
                const start: usize = hole.start;
                const end = start + hole.length;
                if (start < HEADER_LINES * LINE_SIZE or end > BLOCK_SIZE or start > end)
                    return AllocError.CorruptHeap;
            }
            for (block.alloc_start, 0..) |starts, row| {
                if (row < HEADER_LINES and starts != 0) return AllocError.CorruptHeap;
                var bits = starts;
                while (bits != 0) {
                    const bit: usize = @intCast(@ctz(bits));
                    const offset = row * LINE_SIZE + bit * OBJECT_ALIGNMENT;
                    if (offset + HEADER_SIZE > BLOCK_SIZE) return AllocError.CorruptHeap;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & SIZE_MASK) >> SIZE_SHIFT);
                    const rows = @as(usize, header & ROW_COUNT_MASK);
                    if (size == 0 or rows == 0 or offset + HEADER_SIZE + size > BLOCK_SIZE)
                        return AllocError.CorruptHeap;
                    bits &= bits - 1;
                }
            }
        }
    }
};

test "hxcpp geometry and header constants" {
    try std.testing.expectEqual(@as(usize, 32768), BLOCK_SIZE);
    try std.testing.expectEqual(@as(usize, 128), LINE_SIZE);
    try std.testing.expectEqual(@as(usize, 256), LINE_COUNT);
    try std.testing.expectEqual(@as(usize, 2), HEADER_LINES);
    try std.testing.expectEqual(@as(usize, 4), HEADER_SIZE);
}

test "alloc writes hxcpp-compatible header and start bitmap" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();
    const object = try heap.alloc(36, .{ .container = true });
    try std.testing.expectEqual(@as(usize, 36), object.len);
    const payload_address = @intFromPtr(object.ptr);
    const block_base = payload_address & ~BLOCK_MASK;
    const header_address = payload_address - HEADER_SIZE;
    const header: *const u32 = @ptrFromInt(header_address);
    try std.testing.expect((header.* & CONTAINER_FLAG) != 0);
    try std.testing.expect((header.* & SIZE_MASK) >> SIZE_SHIFT == 36);
    try std.testing.expectEqual(@as(usize, 1), heap.blockCount());
    try std.testing.expectEqual(@as(usize, 0), block_base % BLOCK_SIZE);
    try heap.validate();
}

test "explicit roots mark and sweep unreachable allocations" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();
    const live = try heap.alloc(40, .{});
    _ = try heap.alloc(60, .{});
    var roots = [_]usize{@intFromPtr(live.ptr)};
    const tracer = struct {
        fn trace(_: *anyopaque, _: []u8, _: VisitSlotFn, _: *anyopaque) void {}
    }.trace;
    var dummy: u8 = 0;
    heap.setTracer(tracer, &dummy);
    const stats = try heap.collect(roots[0..]);
    try std.testing.expectEqual(@as(usize, 2), stats.objects_before);
    try std.testing.expectEqual(@as(usize, 1), stats.objects_live);
    try std.testing.expectEqual(@as(usize, 1), stats.objects_reclaimed);
    try std.testing.expectEqual(@as(usize, 40), stats.bytes_live);
    try heap.validate();
}

test "tracer preserves transitive child object" {
    const Pair = struct { child: usize };
    const tracer = struct {
        fn trace(_: *anyopaque, payload: []u8, visit: VisitSlotFn, visit_context: *anyopaque) void {
            if (payload.len < @sizeOf(Pair)) return;
            var child_ptr: usize = std.mem.readInt(usize, payload[0..@sizeOf(usize)], .little);
            visit(visit_context, &child_ptr);
        }
    }.trace;
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();
    const parent_bytes = try heap.alloc(@sizeOf(Pair), .{ .container = true });
    const child_bytes = try heap.alloc(24, .{});
    std.mem.writeInt(usize, parent_bytes[0..@sizeOf(usize)], @intFromPtr(child_bytes.ptr), .little);
    var dummy: u8 = 0;
    heap.setTracer(tracer, &dummy);
    var roots = [_]usize{@intFromPtr(parent_bytes.ptr)};
    const stats = try heap.collect(roots[0..]);
    try std.testing.expectEqual(@as(usize, 2), stats.objects_live);
}

test "hole recycling and multi-block allocation" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    // Allocate 100 objects of 200 bytes each across blocks
    var ptrs: std.ArrayListUnmanaged(usize) = .empty;
    defer ptrs.deinit(std.testing.allocator);

    var i: usize = 0;
    while (i < 100) : (i += 1) {
        const obj = try heap.alloc(200, .{});
        try ptrs.append(std.testing.allocator, @intFromPtr(obj.ptr));
    }

    try std.testing.expect(heap.blockCount() > 0);

    // Keep only even objects alive
    var live_roots: std.ArrayListUnmanaged(usize) = .empty;
    defer live_roots.deinit(std.testing.allocator);

    for (ptrs.items, 0..) |ptr, index| {
        if (index % 2 == 0) {
            try live_roots.append(std.testing.allocator, ptr);
        }
    }

    const tracer = struct {
        fn trace(_: *anyopaque, _: []u8, _: VisitSlotFn, _: *anyopaque) void {}
    }.trace;
    var dummy: u8 = 0;
    heap.setTracer(tracer, &dummy);

    const stats1 = try heap.collect(live_roots.items);
    try std.testing.expectEqual(@as(usize, 50), stats1.objects_live);
    try std.testing.expectEqual(@as(usize, 50), stats1.objects_reclaimed);

    // Re-allocate into freed holes
    var new_ptrs: std.ArrayListUnmanaged(usize) = .empty;
    defer new_ptrs.deinit(std.testing.allocator);

    i = 0;
    while (i < 50) : (i += 1) {
        const obj = try heap.alloc(200, .{});
        try new_ptrs.append(std.testing.allocator, @intFromPtr(obj.ptr));
    }

    var all_roots: std.ArrayListUnmanaged(usize) = .empty;
    defer all_roots.deinit(std.testing.allocator);
    try all_roots.appendSlice(std.testing.allocator, live_roots.items);
    try all_roots.appendSlice(std.testing.allocator, new_ptrs.items);

    const stats2 = try heap.collect(all_roots.items);
    try std.testing.expectEqual(@as(usize, 100), stats2.objects_live);
    try heap.validate();
}

test "safepoint flags and configuration limits" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{ .max_blocks = 2 });
    defer heap.deinit();

    try std.testing.expect(!heap.pause_requested);
    heap.requestSafepoint();
    try std.testing.expect(heap.pause_requested);
    heap.releaseSafepoint();
    try std.testing.expect(!heap.pause_requested);

    // Rejection of oversize objects in standard pool
    try std.testing.expectError(AllocError.ObjectTooLarge, heap.alloc(5000, .{}));
}
