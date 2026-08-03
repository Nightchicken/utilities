#ifndef VEC_H
#define VEC_H
// Plain fixed-size vector types and the arithmetic on them. Everything is
// passed and returned by value: these are two to four floats, so copying beats
// pointer indirection and lets the results compose as expressions.
//
// The suffix names the element type -- `f` for float, `d` for double -- and the
// digit the dimension. The game only uses Vec2f (screen and path positions);
// the rest are here as a general-purpose set.
//
// Note this is pure algebra with no notion of the terminal's non-square cells.
// Distances that need to look right on screen go through world_dist() in
// game.h, which weights the y axis; vec*_ functions never do.

// 2D vector. Used throughout the game for character-cell positions, held as
// floats so an enemy can sit between cells and only be rounded when drawn.
typedef struct Vec2f {
  float x; // column, increasing rightwards
  float y; // row, increasing *downwards* -- screen convention, not maths
} Vec2f;
// 3D vector. Unused by the game; here to complete the set.
typedef struct Vec3f {
  float x;
  float y;
  float z; // depth
} Vec3f;
// 4D vector. Unused by the game; the usual homogeneous / RGBA shape.
typedef struct Vec4f {
  float x;
  float y;
  float z;
  float w; // homogeneous coordinate, or the fourth channel
} Vec4f;
// Componentwise sum, a + b.
Vec2f vec2f_add(Vec2f a, Vec2f b) { return (Vec2f){a.x + b.x, a.y + b.y}; }
Vec3f vec3f_add(Vec3f a, Vec3f b) {
  return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec4f vec4f_add(Vec4f a, Vec4f b) {
  return (Vec4f){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
// Multiply every component by the scalar `x`, lengthening (>1) or shortening
// (<1) the vector while keeping its direction. A negative scalar reverses it.
Vec2f vec2f_scale(Vec2f a, float x) { return (Vec2f){a.x * x, a.y * x}; }
Vec3f vec3f_scale(Vec3f a, float x) {
  return (Vec3f){a.x * x, a.y * x, a.z * x};
}
Vec4f vec4f_scale(Vec4f a, float x) {
  return (Vec4f){a.x * x, a.y * x, a.z * x, a.w * x};
}
// Dot product. Zero when the two are perpendicular, positive when they point
// the same general way, negative when opposed; for unit vectors it is the
// cosine of the angle between them.
float vec3f_dot(Vec3f a, Vec3f b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
// Componentwise difference, a - b: the vector that goes from b to a.
Vec3f vec3f_sub(Vec3f a, Vec3f b) {
  return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}
// Cross product: a vector perpendicular to both inputs, with length equal to
// the area of the parallelogram they span. Order matters -- swapping the
// arguments flips the result.
Vec3f vec3f_cross(Vec3f a, Vec3f b) {
  return (Vec3f){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
// The double-precision mirror of everything above, componentwise identical.
typedef struct Vec2d {
  double x;
  double y;
} Vec2d;
typedef struct Vec3d {
  double x;
  double y;
  double z;
} Vec3d;
typedef struct Vec4d {
  double x;
  double y;
  double z;
  double w;
} Vec4d;
Vec2d vec2d_add(Vec2d a, Vec2d b) { return (Vec2d){a.x + b.x, a.y + b.y}; }
Vec3d vec3d_add(Vec3d a, Vec3d b) {
  return (Vec3d){a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec4d vec4d_add(Vec4d a, Vec4d b) {
  return (Vec4d){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
Vec2d vec2d_scale(Vec2d a, double x) { return (Vec2d){a.x * x, a.y * x}; }
Vec3d vec3d_scale(Vec3d a, double x) {
  return (Vec3d){a.x * x, a.y * x, a.z * x};
}
Vec4d vec4d_scale(Vec4d a, double x) {
  return (Vec4d){a.x * x, a.y * x, a.z * x, a.w * x};
}
double vec3d_dot(Vec3d a, Vec3d b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
Vec3d vec3d_sub(Vec3d a, Vec3d b) {
  return (Vec3d){a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3d vec3d_cross(Vec3d a, Vec3d b) {
  return (Vec3d){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
#endif
