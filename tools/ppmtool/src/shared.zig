const std = @import("std");
const input = @import("input.zig");

// std.Thread.Mutex was removed in Zig 0.16; wrap std.atomic.Mutex with yield.
pub const Mutex = struct {
    inner: std.atomic.Mutex = .unlocked,

    pub fn lock(m: *Mutex) void {
        while (!m.inner.tryLock()) {
            std.Thread.yield() catch {};
        }
    }

    pub fn unlock(m: *Mutex) void {
        m.inner.unlock();
    }
};

pub const State = struct {
    mutex: Mutex = .{},
    input: input.State = .{},
    quit: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
};
