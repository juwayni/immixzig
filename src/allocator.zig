//! Main ImmixAllocator implementing allocation, tracing, GC cycles, and safepoint management.

const std = @import("std");
const constants = @import("constants.zig");
const types = @import("types.zig");
const Block = @import("block.zig").Block;
const Compactor = @import("compact.zig").Compactor;

const WorkItem = struct {
    block_index: usize,
    header_offset: usize,
};

pub const Mutex = struct {
    state: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    pub fn lock(self: *Mutex) void {
        while (self.state.cmpxchgWeak(0, 1, .acquire, .monotonic) != null) {
            std.Thread.yield() catch {};
        }
    }

    pub fn unlock(self: *Mutex) void {
        self.state.store(0, .release);
    }
};

pub const ImmixAllocator = struct {
    allocator: std.mem.Allocator,
    config: types.Config,
    blocks: std.ArrayList(Block) = .empty,
    worklist: std.ArrayList(WorkItem) = .empty,
    trace_fn: ?types.TraceObjectFn = null,
    trace_context: ?*anyopaque = null,
    mark_id: u32 = constants.DEFAULT_MARK_ID,
    mark_byte: u8 = constants.DEFAULT_MARK_BYTE,
    collecting: bool = false,
    pause_requested: bool = false,
    bytes_allocated: usize = 0,
    collection_count: usize = 0,

    mutex: Mutex = .{},

    pub fn init(allocator: std.mem.Allocator, config: types.Config) ImmixAllocator {
        return .{
            .allocator = allocator,
            .config = config,
        };
    }

    pub fn deinit(self: *ImmixAllocator) void {
        for (self.blocks.items) |*block| block.deinit(self.allocator);
        self.blocks.deinit(self.allocator);
        self.worklist.deinit(self.allocator);
        self.* = undefined;
    }

    pub fn setTracer(self: *ImmixAllocator, tracer: types.TraceObjectFn, context: *anyopaque) void {
        self.trace_fn = tracer;
        self.trace_context = context;
    }

    pub fn requestSafepoint(self: *ImmixAllocator) void {
        self.mutex.lock();
        defer self.mutex.unlock();
        self.pause_requested = true;
    }

    pub fn releaseSafepoint(self: *ImmixAllocator) void {
        self.mutex.lock();
        defer self.mutex.unlock();
        self.pause_requested = false;
    }

    pub fn checkSafepoint(self: *ImmixAllocator) void {
        while (self.pause_requested) {
            std.Thread.yield() catch {};
        }
    }

    pub fn blockCount(self: *const ImmixAllocator) usize {
        return self.blocks.items.len;
    }

    fn createBlock(self: *ImmixAllocator) types.AllocError!void {
        if (self.config.max_blocks != 0 and self.blocks.items.len >= self.config.max_blocks)
            return types.AllocError.HeapLimitReached;
        if (self.blocks.items.len >= std.math.maxInt(u16)) return types.AllocError.HeapLimitReached;
        var block = try Block.init(self.allocator, @intCast(self.blocks.items.len));
        errdefer block.deinit(self.allocator);
        try self.blocks.append(self.allocator, block);
    }

    pub fn alloc(self: *ImmixAllocator, size_in: usize, flags: types.ObjectFlags) types.AllocError![]u8 {
        if (self.collecting) return types.AllocError.CollectionInProgress;
        if (size_in == 0) return self.allocator.alloc(u8, 0);
        const size = std.mem.alignForward(usize, size_in, constants.OBJECT_ALIGNMENT);
        if (size >= constants.LARGE_OBJECT_SIZE or size > 0x3ffc) return types.AllocError.ObjectTooLarge;
        const total = constants.HEADER_SIZE + size;

        var i: usize = 0;
        while (i < self.blocks.items.len) : (i += 1) {
            const block = &self.blocks.items[i];
            if (block.owned) continue;
            if (block.reserve(total)) |offset| {
                block.noteAllocation(offset, size_in, flags, self.mark_id, self.mark_byte);
                const payload = block.memory[offset + constants.HEADER_SIZE .. offset + constants.HEADER_SIZE + size_in];
                if (self.config.zero_allocations) @memset(payload, 0);
                self.bytes_allocated += size;
                return payload;
            }
        }

        try self.createBlock();
        const index = self.blocks.items.len - 1;
        const block = &self.blocks.items[index];
        const offset = block.reserve(total) orelse return types.AllocError.CorruptHeap;
        block.noteAllocation(offset, size_in, flags, self.mark_id, self.mark_byte);
        const payload = block.memory[offset + constants.HEADER_SIZE .. offset + constants.HEADER_SIZE + size_in];
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

    fn markAddress(self: *ImmixAllocator, address: usize) types.AllocError!void {
        if (address == 0) return;
        const item = self.locateHeader(address) orelse return;
        const block = &self.blocks.items[item.block_index];
        if (block.markObject(item.header_offset, self.mark_id, self.mark_byte)) {
            try self.worklist.append(self.allocator, item);
        }
    }

    const VisitContext = struct {
        heap: *ImmixAllocator,
        failure: ?types.AllocError = null,
    };

    fn visitSlot(context_ptr: *anyopaque, slot: *usize) void {
        const context: *VisitContext = @ptrCast(@alignCast(context_ptr));
        if (context.failure != null or slot.* == 0) return;
        if (context.heap.markAddress(slot.*)) |_| {} else |err| {
            context.failure = err;
        }
    }

    fn advanceMarkEpoch(self: *ImmixAllocator) void {
        var next = (self.mark_id + 0x0100_0000) & constants.MARK_MASK;
        if (next == 0) next = 0x0100_0000;
        self.mark_id = next;
        self.mark_byte +%= 1;
        if (self.mark_byte == 0) self.mark_byte = 1;
    }

    pub fn collect(self: *ImmixAllocator, roots: []usize) types.AllocError!types.Stats {
        return self.collectExt(roots, false);
    }

    pub fn collectExt(self: *ImmixAllocator, roots: []usize, force_compact: bool) types.AllocError!types.Stats {
        if (self.collecting) return types.AllocError.CollectionInProgress;
        self.collecting = true;
        self.requestSafepoint();
        defer {
            self.collecting = false;
            self.releaseSafepoint();
        }

        self.advanceMarkEpoch();
        self.worklist.clearRetainingCapacity();
        var stats = types.Stats{ .blocks = self.blocks.items.len };
        for (self.blocks.items) |*block| {
            block.clearMarks();
            for (block.alloc_start, 0..) |starts, row| {
                var bits = starts;
                while (bits != 0) {
                    const bit: usize = @intCast(@ctz(bits));
                    const offset = row * constants.LINE_SIZE + bit * 4;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                    if (size != 0 and offset + constants.HEADER_SIZE + size <= constants.BLOCK_SIZE) {
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
                const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                const payload = block.memory[item.header_offset + constants.HEADER_SIZE .. item.header_offset + constants.HEADER_SIZE + size];
                tracer(self.trace_context.?, payload, visitSlot, @ptrCast(&visit_context));
                if (visit_context.failure) |err| return err;
            }
        } else if (roots.len != 0) {
            return types.AllocError.CorruptHeap;
        }

        if (force_compact) {
            const moved = try Compactor.compact(
                &self.blocks,
                self.allocator,
                self.mark_id,
                self.mark_byte,
                roots,
                self.trace_fn,
                self.trace_context,
            );
            stats.objects_moved = moved;
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
                    const offset = row * constants.LINE_SIZE + bit * 4;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                    if (size != 0 and (header & constants.MARK_MASK) == self.mark_id) {
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
        stats.blocks = self.blocks.items.len;
        self.bytes_allocated = stats.bytes_live;
        self.collection_count += 1;
        return stats;
    }

    pub fn validate(self: *ImmixAllocator) types.AllocError!void {
        for (self.blocks.items) |*block| {
            if (block.base() % constants.BLOCK_SIZE != 0) return types.AllocError.CorruptHeap;
            if (block.hole_count > constants.MAX_HOLES) return types.AllocError.CorruptHeap;
            for (block.holes[0..block.hole_count]) |hole| {
                const start: usize = hole.start;
                const end = start + hole.length;
                if (start < constants.HEADER_LINES * constants.LINE_SIZE or end > constants.BLOCK_SIZE or start > end)
                    return types.AllocError.CorruptHeap;
            }
            for (block.alloc_start, 0..) |starts, row| {
                if (row < constants.HEADER_LINES and starts != 0) return types.AllocError.CorruptHeap;
                var bits = starts;
                while (bits != 0) {
                    const bit: usize = @intCast(@ctz(bits));
                    const offset = row * constants.LINE_SIZE + bit * 4;
                    if (offset + constants.HEADER_SIZE > constants.BLOCK_SIZE) return types.AllocError.CorruptHeap;
                    const header = block.readHeader(offset);
                    const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                    const rows = @as(usize, header & constants.ROW_COUNT_MASK);
                    if (size == 0 or rows == 0 or offset + constants.HEADER_SIZE + size > constants.BLOCK_SIZE)
                        return types.AllocError.CorruptHeap;
                    bits &= bits - 1;
                }
            }
        }
    }
};

test "ImmixAllocator basic allocation" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    const mem = try heap.alloc(64, .{});
    try std.testing.expectEqual(@as(usize, 64), mem.len);
    try std.testing.expectEqual(@as(usize, 1), heap.blockCount());
    try heap.validate();
}
