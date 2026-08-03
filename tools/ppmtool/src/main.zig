const std = @import("std");
const display = @import("display.zig");
const input = @import("input.zig");
const shared = @import("shared.zig");

pub fn main() !void {
    var s: shared.State = .{};

    const input_thread = try std.Thread.spawn(.{}, input.run, .{&s});
    const display_thread = try std.Thread.spawn(.{}, display.run, .{&s});

    display_thread.join();
    s.quit.store(true, .release);
    input_thread.join();
}
