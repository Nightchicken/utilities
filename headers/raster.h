#ifndef RASTER_H
#define RASTER_H
#include "vec.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @file raster.h
 * @brief Tiny software rasteriser: depth-tested flat triangles into an RGB
 *        framebuffer, saved as a PPM image.
 *
 * A minimal fixed-function pipeline. project() rotates and perspective-divides
 * a world point into screen space, raster_triangle() fills it with an edge
 * function and a z-buffer, and display_screen() dumps the result to disk.
 * There is no clipping, shading, texturing or perspective-correct
 * interpolation.
 *
 * Coordinates are pixels with the origin at the top left and y increasing
 * downwards. Colours are Vec3f in 0..255 per channel, truncated to int on
 * write and never clamped.
 *
 * Typical use:
 * @code
 *     Screen s = init_screen(800, 600, "out.ppm");
 *     clear_screen(&s, (Vec3f){0, 0, 0});
 *
 *     Vec3f tri[3] = { project(a, ax, ay, 5.0f, s.x, s.y),
 *                      project(b, ax, ay, 5.0f, s.x, s.y),
 *                      project(c, ax, ay, 5.0f, s.x, s.y) };
 *     draw_triangle(&s, tri, 3, (Vec3f){255, 40, 40});
 *
 *     display_screen(&s);
 *     deinit_screen(&s);
 * @endcode
 *
 * @warning Nothing here validates arguments or checks allocations, and the
 *          per-pixel accessors do not bounds check. It is debug-grade code:
 *          fine for known-good input, unsafe for untrusted input.
 * @warning Defines a type named `Screen`, which collides with the unrelated
 *          `Screen` in terminal.h. Only one of the two headers can be included
 *          in a given translation unit.
 */
/**
 * @brief An RGB framebuffer with a matching depth buffer, written out as PPM.
 *
 * @var Screen::x
 *   Width in pixels. Also the row stride: every index is (y * x + col).
 * @var Screen::y
 *   Height in pixels.
 * @var Screen::data
 *   Colour plane, malloc'd, x*y*3 ints in R,G,B order. Stored as int rather
 *   than unsigned char, so values are not clamped anywhere -- writing outside
 *   0..255 produces a malformed PPM rather than a saturated colour.
 * @var Screen::depth
 *   Depth plane, malloc'd, x*y floats, one per pixel. Smaller is nearer;
 *   clear_screen() fills it with FLT_MAX to mean "nothing here yet".
 * @var Screen::name
 *   Output path used by display_screen(). Borrowed, not copied, so the string
 *   must outlive the Screen.
 *
 * @warning This name collides with the unrelated `Screen` in terminal.h --
 *          the two headers cannot be included in the same translation unit.
 */
typedef struct {
  int x;
  int y;
  int *data;    // x*y*3 ints, RGB
  float *depth; // x*y floats, one per pixel
  const char *name;
} Screen;

/**
 * @brief Allocate a framebuffer and its depth buffer.
 *
 * @param width  Width in pixels. Must be positive; not validated.
 * @param height Height in pixels. Must be positive; not validated.
 * @param name   Path display_screen() will write the PPM to. Stored by
 *               pointer, not copied, so it must outlive the Screen -- a string
 *               literal or a long-lived buffer, never a local array.
 * @return A Screen by value. The two malloc results are *not* checked, so on
 *         allocation failure `data` and/or `depth` come back NULL and the
 *         first draw call dereferences them. Check them yourself if the
 *         dimensions are large or caller-supplied.
 *
 * @note Neither plane is initialised here. Call clear_screen() before drawing,
 *       or the depth test reads indeterminate memory.
 * @note Pair with deinit_screen().
 */
Screen init_screen(int width, int height, const char *name) {
  Screen window;
  window.x = width;
  window.y = height;
  window.name = name;
  window.data = (int *)malloc(sizeof(int) * width * height * 3);
  window.depth = (float *)malloc(sizeof(float) * width * height);
  return window;
};
/**
 * @brief Free both planes of a Screen.
 *
 * @param window Screen to release. Must not be NULL. The struct itself is not
 *               freed (it is normally a value on the stack) and its `data` and
 *               `depth` pointers are left dangling, so do not reuse or
 *               double-free it.
 */
void deinit_screen(Screen *window) {
  free(window->data);
  free(window->depth);
}
/**
 * @brief Reset the colour plane to a flat colour and the depth plane to far.
 *
 * @param window Screen to clear.
 * @param color  Background colour, one channel per component in 0..255 (x=R,
 *               y=G, z=B). Truncated to int, not rounded and not clamped.
 *
 * @note The depth plane is filled with FLT_MAX, i.e. "infinitely far", so the
 *       first fragment at any pixel always wins the depth test.
 * @note Call this at the start of every frame. Skipping it leaves the previous
 *       frame's depth values in place, which silently rejects new geometry.
 */
void clear_screen(Screen *window, Vec3f color) {
  for (int i = 0; i < window->x * window->y; i++) {
    window->data[i * 3 + 0] = (int)color.x;
    window->data[i * 3 + 1] = (int)color.y;
    window->data[i * 3 + 2] = (int)color.z;
    window->depth[i] = FLT_MAX;
  }
}
/**
 * @brief Read one pixel's colour.
 *
 * @param window Screen to read from.
 * @param x      Column, 0 to window->x - 1.
 * @param y      Row, 0 to window->y - 1.
 * @return The pixel as {R, G, B, 255} floats; alpha is always 255, since the
 *         buffer stores none.
 *
 * @warning No bounds checking. Out-of-range coordinates read out of bounds.
 */
Vec4f get_color(Screen *window, int x, int y) {
  int i = (y * window->x + x) * 3;
  return (Vec4f){(float)window->data[i + 0], (float)window->data[i + 1],
                (float)window->data[i + 2], 255};
}
/**
 * @brief Write one pixel's colour, ignoring the depth buffer.
 *
 * @param window Screen to write to.
 * @param x      Column, 0 to window->x - 1.
 * @param y      Row, 0 to window->y - 1.
 * @param color  Colour, one channel per component in 0..255 (x=R, y=G, z=B).
 *               Truncated to int, not clamped.
 *
 * @warning No bounds checking. Out-of-range coordinates corrupt the heap.
 * @note This is an unconditional write: it neither tests nor updates
 *       @ref Screen::depth. raster_triangle() does that part itself.
 */
void set_color(Screen *window, int x, int y, Vec3f color) {
  int i = (y * window->x + x) * 3;
  window->data[i + 0] = (int)color.x;
  window->data[i + 1] = (int)color.y;
  window->data[i + 2] = (int)color.z;
}
/**
 * @brief Write the colour plane to @ref Screen::name as an ASCII PPM (P3).
 *
 * @param window Screen to save. Its `name` field selects the output path; an
 *               existing file there is truncated.
 *
 * @warning The fopen() result is not checked, so an unwritable path
 *          dereferences NULL. The depth plane is not written.
 * @note P3 is the plain-text PPM variant: one decimal triple per line, roughly
 *       12 bytes per pixel. Fine for debug output, large for real images.
 */
void display_screen(Screen *window) {
  FILE *file = fopen(window->name, "w");
  fprintf(file, "P3\n%d %d\n%d\n", window->x, window->y, 255);
  for (int y = 0; y < window->y; y++) {
    for (int x = 0; x < window->x; x++) {
      Vec4f color = get_color(window, x, y);
      fprintf(file, "%d %d %d\n", (int)color.x, (int)color.y, (int)color.z);
    }
  }
  fflush(file);
  fclose(file);
}
/**
 * @brief Signed area of the triangle (a, b, p), doubled -- the edge function.
 *
 * @param a Start of the edge.
 * @param b End of the edge.
 * @param p Point being classified. Only x and y are read; z is ignored.
 * @return Positive when @p p lies to one side of the directed edge a->b, zero
 *         when exactly on it, negative on the other side. The magnitude is
 *         twice the triangle's area, which is what makes the three edge values
 *         usable directly as unnormalised barycentric weights.
 *
 * @note Which side counts as positive depends on the winding convention and
 *       on y pointing down; here all three positive means "inside".
 */
float edge(Vec3f a, Vec3f b, Vec3f p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
/**
 * @brief Fill one depth-tested triangle in screen space.
 *
 * @param window Screen to draw into.
 * @param v0     First vertex: x and y in pixels, z the depth to interpolate.
 * @param v1     Second vertex. Winding matters -- see the note below.
 * @param v2     Third vertex.
 * @param color  Flat fill colour in 0..255 per component. No shading or
 *               per-vertex colour; the whole triangle is one colour.
 *
 * @note Back-face culled by sign: a triangle whose doubled signed area is <= 0
 *       returns immediately and draws nothing. A triangle that vanishes is
 *       usually wound the wrong way, not off-screen. Degenerate (zero-area)
 *       triangles are dropped by the same test.
 * @note The bounding box is clamped to the surface, so off-screen geometry is
 *       clipped rather than written out of bounds. Pixels are sampled at their
 *       centre (+0.5, +0.5).
 * @note Depth test is "smaller z wins", and a passing fragment updates
 *       @ref Screen::depth. Depth is interpolated linearly in screen space --
 *       no perspective correction.
 */
void raster_triangle(Screen *window, Vec3f v0, Vec3f v1, Vec3f v2,
                            Vec3f color) {
  float area = edge(v0, v1, v2);
  if (area <= 0.0f)
    return;

  int min_x = (int)floorf(fminf(v0.x, fminf(v1.x, v2.x)));
  int max_x = (int)ceilf(fmaxf(v0.x, fmaxf(v1.x, v2.x)));
  int min_y = (int)floorf(fminf(v0.y, fminf(v1.y, v2.y)));
  int max_y = (int)ceilf(fmaxf(v0.y, fmaxf(v1.y, v2.y)));
  if (min_x < 0)
    min_x = 0;
  if (min_y < 0)
    min_y = 0;
  if (max_x > window->x - 1)
    max_x = window->x - 1;
  if (max_y > window->y - 1)
    max_y = window->y - 1;

  float inv_area = 1.0f / area;
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      Vec3f p = {(float)x + 0.5f, (float)y + 0.5f, 0.0f};
      float w0 = edge(v1, v2, p);
      float w1 = edge(v2, v0, p);
      float w2 = edge(v0, v1, p);

      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
        continue;

      float b0 = w0 * inv_area;
      float b1 = w1 * inv_area;
      float b2 = w2 * inv_area;
      float z = b0 * v0.z + b1 * v1.z + b2 * v2.z;

      int di = y * window->x + x;
      if (z < window->depth[di]) {
        window->depth[di] = z;
        set_color(window, x, y, color);
      }
    }
  }
}

/**
 * @brief Draw a flat array of vertices as a list of independent triangles.
 *
 * @param window    Screen to draw into.
 * @param verticies Vertex array, consumed three at a time. Not a strip or a
 *                  fan: vertices 0,1,2 form one triangle, 3,4,5 the next.
 * @param count     Number of *vertices*, not triangles. A trailing partial
 *                  triangle (count not a multiple of 3) is silently ignored.
 * @param color     Flat colour applied to every triangle, in 0..255 per
 *                  component.
 *
 * @note Each triangle goes through raster_triangle(), so the same winding rule
 *       and depth test apply per triangle.
 */
void draw_triangle(Screen *window, Vec3f *verticies, int count, Vec3f color) {
  for (int i = 0; i + 2 < count; i += 3) {
    raster_triangle(window, verticies[i], verticies[i + 1], verticies[i + 2],
                    color);
  }
}
/**
 * @brief Rotate a world point and perspective-project it to screen space.
 *
 * @param p    Point in world space.
 * @param ax   Pitch in radians, applied second, about the x axis.
 * @param ay   Yaw in radians, applied first, about the y axis.
 * @param dist Camera distance, added to z after rotation to push the scene in
 *             front of the viewer. Must exceed the scene's z extent, or points
 *             land at or behind z = 0 and the divide blows up -- there is no
 *             near-plane clipping here.
 * @param w    Viewport width in pixels, used to centre the result.
 * @param h    Viewport height in pixels; also sets the field of view, since
 *             the projection scale is 0.9 * h.
 * @return Screen-space position: x and y in pixels with the origin moved to the
 *         viewport centre, and z the post-translation depth for the depth
 *         buffer (so it is a distance, not a normalised 0..1 value).
 *
 * @note y is negated, mapping +y up in world space to rows increasing
 *       downwards on screen.
 */
Vec3f project(Vec3f p, float ax, float ay, float dist, int w, int h) {
  float x1 = p.x * cosf(ay) + p.z * sinf(ay);
  float z1 = -p.x * sinf(ay) + p.z * cosf(ay);
  float y1 = p.y;
  float y2 = y1 * cosf(ax) - z1 * sinf(ax);
  float z2 = y1 * sinf(ax) + z1 * cosf(ax);
  float x2 = x1;
  z2 += dist;
  float scale = 0.9f * (float)h;
  return (Vec3f){x2 / z2 * scale + w * 0.5f,
                -y2 / z2 * scale + h * 0.5f,
                z2};
}
#endif
