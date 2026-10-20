//! Compaction and Evacuation module for Immix GC.

const std = @import("std");
const constants = @import("constants.zig");
const types = @import("types.zig");
const Block = @import("block.zig").Block;

pub const Compactor = struct {
    pub fn compact(
        blocks: *std.ArrayList(Block),
        allocator: std.mem.Allocator,
        mark_id: u32,
        mark_byte: u8,
        roots: []usize,
        trace_fn: ?types.TraceObjectFn,
        trace_context: ?*anyopaque,
    ) types.AllocError!usize {
        var moved_count: usize = 0;
        if (blocks.items.len == 0) return 0;

        for (0..blocks.items.len) |src_idx| {
            var src_block = &blocks.items[src_idx];
            if (src_block.pinned or src_block.move_score <= constants.FRAG_THRESH) continue;

            var row: usize = constants.HEADER_LINES;
            while (row < constants.LINE_COUNT) : (row += 1) {
                var starts = src_block.alloc_start[row];
                while (starts != 0) {
                    const bit: u5 = @intCast(@ctz(starts));
                    const src_offset = row * constants.LINE_SIZE + @as(usize, bit) * 4;
                    const header = src_block.readHeader(src_offset);

                    if ((header & constants.MARK_MASK) == (mark_id & constants.MARK_MASK) and
                        (header & constants.MOVED_SENTINEL) != constants.MOVED_SENTINEL)
                    {
                        const payload_size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                        const total_size = constants.HEADER_SIZE + std.mem.alignForward(usize, payload_size, constants.OBJECT_ALIGNMENT);

                        var dest_index: usize = 0;
                        while (dest_index < blocks.items.len) : (dest_index += 1) {
                            var dest_block = &blocks.items[dest_index];
                            if (dest_index == src_idx or dest_block.move_score > constants.FRAG_THRESH) continue;

                            if (dest_block.reserve(total_size)) |dest_offset| {
                                const is_container = (header & constants.CONTAINER_FLAG) != 0;
                                const preserved = header & constants.HEADER_PRESERVE;
                                dest_block.noteAllocation(
                                    dest_offset,
                                    payload_size,
                                    .{ .container = is_container, .preserved = preserved },
                                    mark_id,
                                    mark_byte,
                                );

                                const src_payload = src_block.memory[src_offset + constants.HEADER_SIZE .. src_offset + constants.HEADER_SIZE + payload_size];
                                const dest_payload = dest_block.memory[dest_offset + constants.HEADER_SIZE .. dest_offset + constants.HEADER_SIZE + payload_size];
                                @memcpy(dest_payload, src_payload);

                                const new_payload_ptr = @intFromPtr(dest_payload.ptr);

                                src_block.writeHeader(src_offset, constants.MOVED_SENTINEL);
                                std.mem.writeInt(usize, @ptrCast(src_block.memory.ptr + src_offset + constants.HEADER_SIZE), new_payload_ptr, .little);

                                // Clear start bit on old source location so line sweep reclaims evacuated line
                                src_block.clearStart(src_offset);

                                moved_count += 1;
                                break;
                            }
                        }
                    }

                    starts &= starts - 1;
                }
            }

            if (moved_count > 0) {
                // Recompute row_marked on source block based on remaining non-evacuated objects
                @memset(src_block.row_marked[constants.HEADER_LINES..], 0);
                for (constants.HEADER_LINES..constants.LINE_COUNT) |r| {
                    var bits = src_block.alloc_start[r];
                    while (bits != 0) {
                        const bit: u5 = @intCast(@ctz(bits));
                        const offset = r * constants.LINE_SIZE + @as(usize, bit) * 4;
                        const h = src_block.readHeader(offset);
                        if ((h & constants.MARK_MASK) == (mark_id & constants.MARK_MASK) and
                            (h & constants.MOVED_SENTINEL) != constants.MOVED_SENTINEL)
                        {
                            const size = @as(usize, (h & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                            const end_r = (offset + constants.HEADER_SIZE + size + constants.LINE_SIZE - 1) >> constants.LINE_BITS;
                            for (r..end_r) |mr| {
                                if (mr < constants.LINE_COUNT) src_block.row_marked[mr] = 1;
                            }
                        }
                        bits &= bits - 1;
                    }
                }
                src_block.syncRowMarks();
            }
        }

        if (moved_count == 0) return 0;

        const UpdateContext = struct {
            blocks: []Block,
        };

        const updateSlot = struct {
            fn visit(ctx_ptr: *anyopaque, slot: *usize) void {
                const ctx: *UpdateContext = @ptrCast(@alignCast(ctx_ptr));
                const ptr = slot.*;
                if (ptr == 0) return;

                for (ctx.blocks) |*b| {
                    if (b.contains(ptr)) {
                        const block_offset = ptr - b.base();
                        var back: usize = 0;
                        while (back <= block_offset) : (back += 4) {
                            if (block_offset < constants.HEADER_SIZE + back) break;
                            const candidate = block_offset - constants.HEADER_SIZE - back;
                            if (candidate < constants.HEADER_LINES * constants.LINE_SIZE) continue;
                            if (!b.hasStart(candidate)) continue;
                            const h = b.readHeader(candidate);
                            if (h == constants.MOVED_SENTINEL) {
                                const fwd_ptr = std.mem.readInt(usize, @ptrCast(b.memory.ptr + candidate + constants.HEADER_SIZE), .little);
                                const offset_inside_obj = ptr - (b.base() + candidate + constants.HEADER_SIZE);
                                slot.* = fwd_ptr + offset_inside_obj;
                                return;
                            }
                        }
                    }
                }
            }
        };

        var update_ctx = UpdateContext{ .blocks = blocks.items };

        for (roots) |*root_ptr| {
            updateSlot.visit(&update_ctx, root_ptr);
        }

        if (trace_fn) |tracer| {
            for (blocks.items) |*b| {
                for (0..constants.LINE_COUNT) |row| {
                    var starts = b.alloc_start[row];
                    while (starts != 0) {
                        const bit: u5 = @intCast(@ctz(starts));
                        const offset = row * constants.LINE_SIZE + @as(usize, bit) * 4;
                        const header = b.readHeader(offset);
                        if ((header & constants.MARK_MASK) == (mark_id & constants.MARK_MASK) and
                            (header & constants.MOVED_SENTINEL) != constants.MOVED_SENTINEL)
                        {
                            const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
                            const payload = b.memory[offset + constants.HEADER_SIZE .. offset + constants.HEADER_SIZE + size];
                            tracer(trace_context.?, payload, updateSlot.visit, &update_ctx);
                        }
                        starts &= starts - 1;
                    }
                }
            }
        }

        _ = allocator;
        return moved_count;
    }
};

test "Compactor compiles cleanly" {
    try std.testing.expectEqual(@as(usize, 0), @sizeOf(Compactor));
}
