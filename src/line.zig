//! Line abstraction representing 128-byte Immix line geometry and status metadata.

const std = @import("std");
const constants = @import("constants.zig");

pub const Line = struct {
    index: u8,
    block_offset: u16,

    pub fn init(index: u8) Line {
        return .{
            .index = index,
            .block_offset = @intCast(@as(usize, index) * constants.LINE_SIZE),
        };
    }

    pub fn isHeader(self: Line) bool {
        return self.index < constants.HEADER_LINES;
    }

    pub fn isUseful(self: Line) bool {
        return !self.isHeader();
    }
};

test "Line initialization and properties" {
    const l0 = Line.init(0);
    try std.testing.expect(l0.isHeader());
    try std.testing.expect(!l0.isUseful());

    const l2 = Line.init(2);
    try std.testing.expect(!l2.isHeader());
    try std.testing.expect(l2.isUseful());
    try std.testing.expectEqual(@as(u16, 256), l2.block_offset);
}
