#ifndef RASTER_H
#define RASTER_H
#include "vec.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {
  int x;
  int y;
  int *data;    // x*y*3 ints, RGB
  float *depth; // x*y floats, one per pixel
  const char *name;
} Screen;

Screen init_screen(int width, int height, const char *name) {
  Screen window;
  window.x = width;
  window.y = height;
  window.name = name;
  window.data = (int *)malloc(sizeof(int) * width * height * 3);
  window.depth = (float *)malloc(sizeof(float) * width * height);
  return window;
};
void deinit_screen(Screen *window) {
  free(window->data);
  free(window->depth);
}
// Clear color buffer to `color` and depth buffer to "far" (+inf).
void clear_screen(Screen *window, Vec3f color) {
  for (int i = 0; i < window->x * window->y; i++) {
    window->data[i * 3 + 0] = (int)color.x;
    window->data[i * 3 + 1] = (int)color.y;
    window->data[i * 3 + 2] = (int)color.z;
    window->depth[i] = FLT_MAX;
  }
}
Vec4f get_color(Screen *window, int x, int y) {
  int i = (y * window->x + x) * 3;
  return (Vec4f){(float)window->data[i + 0], (float)window->data[i + 1],
                (float)window->data[i + 2], 255};
}
void set_color(Screen *window, int x, int y, Vec3f color) {
  int i = (y * window->x + x) * 3;
  window->data[i + 0] = (int)color.x;
  window->data[i + 1] = (int)color.y;
  window->data[i + 2] = (int)color.z;
}
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
float edge(Vec3f a, Vec3f b, Vec3f p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
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

void draw_triangle(Screen *window, Vec3f *verticies, int count, Vec3f color) {
  for (int i = 0; i + 2 < count; i += 3) {
    raster_triangle(window, verticies[i], verticies[i + 1], verticies[i + 2],
                    color);
  }
}
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
