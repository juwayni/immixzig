//! 32KB Aligned Block topology for Immix GC.

const std = @import("std");
const constants = @import("constants.zig");
const types = @import("types.zig");

pub const Block = struct {
    memory: []align(constants.BLOCK_SIZE) u8,
    id: u16,
    alloc_start: [constants.LINE_COUNT]u32 = std.mem.zeroes([constants.LINE_COUNT]u32),
    row_marked: [constants.LINE_COUNT]u8 = std.mem.zeroes([constants.LINE_COUNT]u8),
    holes: [constants.MAX_HOLES]types.HoleRange = std.mem.zeroes([constants.MAX_HOLES]types.HoleRange),
    hole_count: usize = 1,
    used_rows: usize = 0,
    max_hole_size: usize = constants.USEFUL_LINES * constants.LINE_SIZE,
    move_score: usize = 0,
    used_bytes: usize = 0,
    fragged_rows: usize = 0,
    pinned: bool = false,
    zeroed: bool = false,
    reclaimed: bool = true,
    owned: bool = false,
    cursor_hole: usize = 0,
    cursor: usize = constants.HEADER_LINES * constants.LINE_SIZE,

    pub fn init(allocator: std.mem.Allocator, id: u16) types.AllocError!Block {
        const alignment = comptime std.mem.Alignment.fromByteUnits(constants.BLOCK_SIZE);
        const mem = allocator.alignedAlloc(u8, alignment, constants.BLOCK_SIZE) catch return types.AllocError.OutOfMemory;
        @memset(mem, 0);
        var result = Block{ .memory = mem, .id = id };
        result.holes[0] = .{
            .start = @intCast(constants.HEADER_LINES * constants.LINE_SIZE),
            .length = @intCast(constants.USEFUL_LINES * constants.LINE_SIZE),
        };
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
        return ptr >= self.base() + constants.HEADER_LINES * constants.LINE_SIZE and ptr < self.base() + constants.BLOCK_SIZE;
    }

    pub fn syncRowMarks(self: *Block) void {
        @memcpy(self.memory[0..constants.LINE_COUNT], self.row_marked[0..]);
        std.mem.writeInt(u16, @ptrCast(self.memory.ptr), self.id, .little);
    }

    fn startBit(offset: usize) u32 {
        return @as(u32, 1) << @as(u5, @intCast((offset & (constants.LINE_SIZE - 1)) / 4));
    }

    pub fn rowOf(offset: usize) usize {
        return offset >> constants.LINE_BITS;
    }

    pub fn readHeader(self: *const Block, header_offset: usize) u32 {
        return std.mem.readInt(u32, @ptrCast(self.memory.ptr + header_offset), .little);
    }

    pub fn writeHeader(self: *Block, header_offset: usize, value: u32) void {
        std.mem.writeInt(u32, @ptrCast(self.memory.ptr + header_offset), value, .little);
    }

    pub fn setStart(self: *Block, offset: usize) void {
        self.alloc_start[rowOf(offset)] |= startBit(offset);
    }

    pub fn clearStart(self: *Block, offset: usize) void {
        self.alloc_start[rowOf(offset)] &= ~startBit(offset);
    }

    pub fn hasStart(self: *const Block, offset: usize) bool {
        if (offset >= constants.BLOCK_SIZE) return false;
        return (self.alloc_start[rowOf(offset)] & startBit(offset)) != 0;
    }

    pub fn freeRows(self: *const Block) usize {
        return constants.USEFUL_LINES - self.used_rows;
    }

    pub fn freeBytes(self: *const Block) usize {
        return self.freeRows() * constants.LINE_SIZE;
    }

    pub fn isEmpty(self: *const Block) bool {
        return self.used_rows == 0;
    }

    pub fn reserve(self: *Block, total_size: usize) ?usize {
        if (total_size == 0 or total_size >= constants.LARGE_OBJECT_SIZE) return null;
        while (self.cursor_hole < self.hole_count) {
            const hole = self.holes[self.cursor_hole];
            const begin: usize = hole.start;
            const end = begin + hole.length;
            if (self.cursor < begin or self.cursor > end) self.cursor = begin;

            const payload_aligned = std.mem.alignForward(usize, self.cursor + constants.HEADER_SIZE, constants.OBJECT_ALIGNMENT);
            const header_offset = payload_aligned - constants.HEADER_SIZE;

            if (header_offset >= begin and payload_aligned + (total_size - constants.HEADER_SIZE) <= end) {
                self.cursor = payload_aligned + (total_size - constants.HEADER_SIZE);
                return header_offset;
            }
            self.cursor_hole += 1;
            if (self.cursor_hole < self.hole_count)
                self.cursor = self.holes[self.cursor_hole].start;
        }
        return null;
    }

    pub fn noteAllocation(self: *Block, offset: usize, payload_size: usize, flags: types.ObjectFlags, mark_id: u32, mark_byte: u8) void {
        const alloc_bytes = std.mem.alignForward(usize, payload_size, constants.OBJECT_ALIGNMENT);
        const total = constants.HEADER_SIZE + alloc_bytes;
        const first_row = rowOf(offset);
        const end_row = (offset + total + constants.LINE_SIZE - 1) >> constants.LINE_BITS;
        const rows = end_row - first_row;
        var header = (mark_id & constants.MARK_MASK) |
            (@as(u32, @intCast(payload_size)) << constants.SIZE_SHIFT) |
            @as(u32, @intCast(rows & 0xff)) |
            (flags.preserved & constants.HEADER_PRESERVE);
        if (flags.container) header |= constants.CONTAINER_FLAG;
        self.writeHeader(offset, header);
        self.setStart(offset);
        self.memory[offset + 3] = mark_byte;
        var r = first_row;
        while (r < end_row) : (r += 1) self.row_marked[r] = 1;
        self.syncRowMarks();
        self.reclaimed = false;
        self.zeroed = false;
    }

    pub fn objectOffsetForPayload(self: *const Block, payload: usize, max_interior: usize) ?usize {
        if (!self.contains(payload)) return null;
        const block_offset = payload - self.base();
        var back: usize = 0;
        while (back <= max_interior and back <= block_offset) : (back += 4) {
            const candidate = block_offset - constants.HEADER_SIZE - back;
            if (candidate < constants.HEADER_LINES * constants.LINE_SIZE or candidate >= constants.BLOCK_SIZE - constants.HEADER_SIZE) continue;
            if (!self.hasStart(candidate)) continue;
            const header = self.readHeader(candidate);
            const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
            if (size == 0 or candidate + constants.HEADER_SIZE + size > constants.BLOCK_SIZE) continue;
            if (payload >= self.base() + candidate + constants.HEADER_SIZE and
                payload < self.base() + candidate + constants.HEADER_SIZE + size)
                return candidate;
        }
        return null;
    }

    pub fn markObject(self: *Block, header_offset: usize, mark_id: u32, mark_byte: u8) bool {
        if (!self.hasStart(header_offset)) return false;
        var header = self.readHeader(header_offset);
        if ((header & constants.MARK_MASK) == (mark_id & constants.MARK_MASK)) return false;
        const size = @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
        const rows = @as(usize, header & constants.ROW_COUNT_MASK);
        if (size == 0 or rows == 0 or header_offset + constants.HEADER_SIZE + size > constants.BLOCK_SIZE) return false;
        header = (header & ~constants.MARK_MASK) | (mark_id & constants.MARK_MASK);
        self.writeHeader(header_offset, header);
        self.memory[header_offset + 3] = mark_byte;
        const first = rowOf(header_offset);
        var r = first;
        while (r < first + rows and r < constants.LINE_COUNT) : (r += 1) self.row_marked[r] = 1;
        self.syncRowMarks();
        return true;
    }

    pub fn reclaim(self: *Block, full: bool, mark_id: u32) types.Stats {
        var stats = types.Stats{};
        self.hole_count = 0;
        self.max_hole_size = 0;
        self.used_bytes = 0;
        var row: usize = constants.HEADER_LINES;
        var free_lines: usize = 0;

        while (row < constants.LINE_COUNT) {
            if (self.row_marked[row] != 0) {
                if (full and self.alloc_start[row] != 0) {
                    var bits = self.alloc_start[row];
                    while (bits != 0) {
                        const bit: u5 = @intCast(@ctz(bits));
                        const offset = row * constants.LINE_SIZE + @as(usize, bit) * 4;
                        const header = self.readHeader(offset);
                        if ((header & constants.MARK_MASK) == (mark_id & constants.MARK_MASK)) {
                            self.used_bytes += constants.HEADER_SIZE + @as(usize, (header & constants.SIZE_MASK) >> constants.SIZE_SHIFT);
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
            while (row < constants.LINE_COUNT and self.row_marked[row] == 0) : (row += 1) {}
            const len = row - hole_start;
            if (len == 0) continue;
            if (self.hole_count < constants.MAX_HOLES) {
                self.holes[self.hole_count] = .{
                    .start = @intCast(hole_start * constants.LINE_SIZE),
                    .length = @intCast(len * constants.LINE_SIZE),
                };
                self.hole_count += 1;
            }
            free_lines += len;
            self.max_hole_size = @max(self.max_hole_size, len * constants.LINE_SIZE);
            @memset(self.alloc_start[hole_start .. hole_start + len], 0);
        }

        self.used_rows = constants.USEFUL_LINES - free_lines;
        if (!full) self.used_bytes = self.used_rows * constants.LINE_SIZE;
        self.move_score = self.calcFragScore();
        self.reclaimed = true;
        self.zeroed = false;
        self.cursor_hole = 0;
        self.cursor = if (self.hole_count > 0) self.holes[0].start else constants.BLOCK_SIZE;
        if (self.used_rows == 0) {
            self.hole_count = 1;
            self.holes[0] = .{
                .start = @intCast(constants.HEADER_LINES * constants.LINE_SIZE),
                .length = @intCast(constants.USEFUL_LINES * constants.LINE_SIZE),
            };
            self.max_hole_size = constants.USEFUL_LINES * constants.LINE_SIZE;
            self.move_score = 0;
            self.cursor = constants.HEADER_LINES * constants.LINE_SIZE;
            self.cursor_hole = 0;
        }
        stats.rows_in_use = self.used_rows;
        stats.bytes_in_use = self.used_bytes;
        if (self.isEmpty()) stats.empty_blocks = 1;
        if (self.move_score > constants.FRAG_THRESH) stats.fragmented_blocks = 1;
        stats.fragmentation_score = self.move_score;
        return stats;
    }

    pub fn calcFragScore(self: *const Block) usize {
        if (self.pinned) return 0;
        const hole_penalty = if (self.hole_count > 3) self.hole_count - 3 else 0;
        return hole_penalty + (8 * self.used_rows * constants.LINE_SIZE) / (self.used_bytes + constants.LINE_SIZE);
    }

    pub fn clearMarks(self: *Block) void {
        @memset(self.row_marked[constants.HEADER_LINES..], 0);
        self.syncRowMarks();
        self.pinned = false;
    }

    pub fn pin(self: *Block) void {
        self.pinned = true;
        self.move_score = 0;
    }
};

test "Block initialization and reservation" {
    var block = try Block.init(std.testing.allocator, 1);
    defer block.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 0), block.base() % constants.BLOCK_SIZE);
    try std.testing.expectEqual(@as(u16, 1), block.id);
    try std.testing.expect(block.isEmpty());

    const reserved = block.reserve(64);
    try std.testing.expect(reserved != null);
    try std.testing.expectEqual(@as(usize, constants.HEADER_LINES * constants.LINE_SIZE), reserved.?);
}
