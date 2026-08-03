const std = @import("std");
const rl = @import("raylib");
const shared = @import("shared.zig");

pub const FrameBuffer = struct {
    target: rl.RenderTexture2D,
    width: i32,
    height: i32,

    pub fn init(width: i32, height: i32) !FrameBuffer {
        return .{
            .target = try rl.loadRenderTexture(width, height),
            .width = width,
            .height = height,
        };
    }

    pub fn deinit(self: FrameBuffer) void {
        rl.unloadRenderTexture(self.target);
    }

    pub fn begin(self: FrameBuffer) void {
        rl.beginTextureMode(self.target);
        rl.clearBackground(rl.Color.ray_white);
    }

    pub fn end(_: FrameBuffer) void {
        rl.endTextureMode();
    }

    // Blit framebuffer to screen, flipping Y to correct raylib's texture orientation.
    pub fn present(self: FrameBuffer) void {
        const src = rl.Rectangle{
            .x = 0,
            .y = 0,
            .width = @floatFromInt(self.width),
            .height = -@as(f32, @floatFromInt(self.height)),
        };
        rl.beginDrawing();
        defer rl.endDrawing();
        rl.clearBackground(rl.Color.black);
        rl.drawTextureRec(self.target.texture, src, .{ .x = 0, .y = 0 }, rl.Color.white);
    }
};

pub const Canvas = struct {
    x: u8,
    y: u8,
    w: u8,
    h: u8,
    size: *u8,
    pub fn init(x: u8, y: u8, w: u8, h: u8, allocator: std.heap.Allocator) Canvas {
        return Canvas{ .x = x, .y = y, .w = w, .h = h, .size = allocator.alloc(u8, w * h * 3) };
    }
    pub fn deinit(self: *Canvas, allocator: std.heap.Allocator) void {
        allocator.free(self.size);
    }
};

pub fn run(s: *shared.State, allocator: std.heap.Allocator) void {
    rl.initWindow(800, 600, "ppmtool");
    defer rl.closeWindow();
    rl.setTargetFPS(60);
    var canvas = Canvas.init(0, 0, 100, 100, allocator);
    defer canvas.deinit(allocator);

    const fb = FrameBuffer.init(800, 600) catch {
        s.quit.store(true, .release);
        return;
    };
    defer fb.deinit();

    while (!rl.windowShouldClose() and !s.quit.load(.acquire)) {
        s.mutex.lock();
        const inp = s.input;
        s.mutex.unlock();

        var buf: [64:0]u8 = undefined;
        const text = std.fmt.bufPrintZ(&buf, "x: {d:.1} y: {d:.1}", .{ inp.mouse_pos.x, inp.mouse_pos.y }) catch "?";

        fb.begin();
        rl.drawText(text, 10, 10, 20, rl.Color.dark_gray);
        fb.end();
        fb.present();
    }

    s.quit.store(true, .release);
}
