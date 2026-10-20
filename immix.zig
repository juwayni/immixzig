const std = @import("std");

pub const constants = @import("src/constants.zig");
pub const types = @import("src/types.zig");
pub const line = @import("src/line.zig");
pub const block = @import("src/block.zig");
pub const compact = @import("src/compact.zig");
pub const allocator = @import("src/allocator.zig");

pub const BLOCK_SIZE = constants.BLOCK_SIZE;
pub const BLOCK_BITS = constants.BLOCK_BITS;
pub const LINE_SIZE = constants.LINE_SIZE;
pub const LINE_BITS = constants.LINE_BITS;
pub const LINE_COUNT = constants.LINE_COUNT;
pub const HEADER_LINES = constants.HEADER_LINES;
pub const USEFUL_LINES = constants.USEFUL_LINES;
pub const HEADER_SIZE = constants.HEADER_SIZE;
pub const LARGE_OBJECT_SIZE = constants.LARGE_OBJECT_SIZE;
pub const FRAG_THRESH = constants.FRAG_THRESH;

pub const AllocError = types.AllocError;
pub const HoleRange = types.HoleRange;
pub const ObjectFlags = types.ObjectFlags;
pub const VisitSlotFn = types.VisitSlotFn;
pub const TraceObjectFn = types.TraceObjectFn;
pub const Config = types.Config;
pub const Stats = types.Stats;

pub const Line = line.Line;
pub const Block = block.Block;
pub const Compactor = compact.Compactor;
pub const ImmixAllocator = allocator.ImmixAllocator;

test {
    std.testing.refAllDecls(@This());
    _ = @import("src/line.zig");
    _ = @import("src/block.zig");
    _ = @import("src/compact.zig");
    _ = @import("src/allocator.zig");
}

test "Topology and layout constants" {
    try std.testing.expectEqual(@as(usize, 32768), BLOCK_SIZE);
    try std.testing.expectEqual(@as(usize, 128), LINE_SIZE);
    try std.testing.expectEqual(@as(usize, 256), LINE_COUNT);
    try std.testing.expectEqual(@as(usize, 2), HEADER_LINES);
    try std.testing.expectEqual(@as(usize, 4), HEADER_SIZE);
}

test "Fast path allocation and header writing" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    const obj = try heap.alloc(36, .{ .container = true });
    try std.testing.expectEqual(@as(usize, 36), obj.len);

    const payload_addr = @intFromPtr(obj.ptr);
    const header_addr = payload_addr - HEADER_SIZE;
    const header_ptr: *const u32 = @ptrFromInt(header_addr);

    try std.testing.expect((header_ptr.* & constants.CONTAINER_FLAG) != 0);
    try std.testing.expectEqual(@as(u32, 36), (header_ptr.* & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
    try std.testing.expectEqual(@as(usize, 1), heap.blockCount());

    try heap.validate();
}

test "Explicit root collection reclaims dead objects" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    const live = try heap.alloc(40, .{});
    _ = try heap.alloc(80, .{});

    var dummy: u8 = 0;
    const dummy_tracer = struct {
        fn trace(_: *anyopaque, _: []u8, _: VisitSlotFn, _: *anyopaque) void {}
    }.trace;

    heap.setTracer(dummy_tracer, &dummy);

    var roots = [_]usize{@intFromPtr(live.ptr)};
    const stats = try heap.collect(roots[0..]);

    try std.testing.expectEqual(@as(usize, 2), stats.objects_before);
    try std.testing.expectEqual(@as(usize, 1), stats.objects_live);
    try std.testing.expectEqual(@as(usize, 1), stats.objects_reclaimed);
    try std.testing.expectEqual(@as(usize, 40), stats.bytes_live);

    try heap.validate();
}

test "Trace visitor preserves reachable object graph" {
    const node_tracer = struct {
        fn trace(_: *anyopaque, payload: []u8, visit: VisitSlotFn, visit_ctx: *anyopaque) void {
            if (payload.len >= @sizeOf(usize)) {
                var ptr: usize = std.mem.readInt(usize, @ptrCast(payload[0..@sizeOf(usize)]), .little);
                if (ptr != 0) {
                    visit(visit_ctx, &ptr);
                    std.mem.writeInt(usize, @ptrCast(payload[0..@sizeOf(usize)]), ptr, .little);
                }
            }
        }
    }.trace;

    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    const parent_bytes = try heap.alloc(8, .{ .container = true });
    const child_bytes = try heap.alloc(32, .{});

    std.mem.writeInt(usize, @ptrCast(parent_bytes[0..@sizeOf(usize)]), @intFromPtr(child_bytes.ptr), .little);

    var dummy: u8 = 0;
    heap.setTracer(node_tracer, &dummy);

    var roots = [_]usize{@intFromPtr(parent_bytes.ptr)};
    const stats = try heap.collect(roots[0..]);

    try std.testing.expectEqual(@as(usize, 2), stats.objects_live);
    try std.testing.expectEqual(@as(usize, 0), stats.objects_reclaimed);
}

test "Safepoint request and release" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    heap.requestSafepoint();
    try std.testing.expect(heap.pause_requested);

    heap.releaseSafepoint();
    try std.testing.expect(!heap.pause_requested);
}

test "Compaction evacuates objects and updates slot references" {
    var heap = ImmixAllocator.init(std.testing.allocator, .{});
    defer heap.deinit();

    const ref_tracer = struct {
        fn trace(_: *anyopaque, payload: []u8, visit: VisitSlotFn, visit_ctx: *anyopaque) void {
            if (payload.len >= @sizeOf(usize)) {
                var ptr: usize = std.mem.readInt(usize, @ptrCast(payload[0..@sizeOf(usize)]), .little);
                if (ptr != 0) {
                    visit(visit_ctx, &ptr);
                    std.mem.writeInt(usize, @ptrCast(payload[0..@sizeOf(usize)]), ptr, .little);
                }
            }
        }
    }.trace;

    var dummy: u8 = 0;
    heap.setTracer(ref_tracer, &dummy);

    const target_bytes = try heap.alloc(64, .{});
    const holder_bytes = try heap.alloc(8, .{ .container = true });

    std.mem.writeInt(usize, @ptrCast(holder_bytes[0..@sizeOf(usize)]), @intFromPtr(target_bytes.ptr), .little);

    heap.blocks.items[0].move_score = 20;

    var roots = [_]usize{@intFromPtr(holder_bytes.ptr)};
    const stats = try heap.collectExt(roots[0..], true);

    try std.testing.expect(stats.objects_live >= 2);
    try heap.validate();
}
