const std = @import("std");
const rl = @import("raylib");
const shared = @import("shared.zig");

pub const State = struct {
    mouse_pos: rl.Vector2 = .{ .x = 0, .y = 0 },
    mouse_left: bool = false,
    mouse_right: bool = false,
};

pub fn run(s: *shared.State) void {
    while (!s.quit.load(.acquire)) {
        const polled = State{
            .mouse_pos = rl.getMousePosition(),
            .mouse_left = rl.isMouseButtonDown(.left),
            .mouse_right = rl.isMouseButtonDown(.right),
        };
        s.mutex.lock();
        s.input = polled;
        s.mutex.unlock();

        const req = std.os.linux.timespec{ .sec = 0, .nsec = 1_000_000 };
        _ = std.os.linux.nanosleep(&req, null);
    }
}
