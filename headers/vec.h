#ifndef VEC_H
#define VEC_H
/**
 * @file vec.h
 * @brief Plain fixed-size vector types and the arithmetic on them.
 *
 * Everything is passed and returned by value: these are two to four floats or
 * doubles, so copying beats pointer indirection and lets results compose as
 * expressions -- `vec3f_add(a, vec3f_scale(b, t))` rather than out-parameters.
 *
 * Naming is `vec<dim><suffix>_<op>`: the digit is the dimension (2, 3 or 4)
 * and the suffix the element type, `f` for float and `d` for double. The two
 * families are componentwise identical; only the element type differs.
 *
 * Coverage is deliberately uneven. `add` and `scale` exist for every
 * dimension; `dot`, `sub` and `cross` are 3D only, since nothing here needed
 * the 2D or 4D versions. There is no length, normalise or divide.
 *
 * @note This is pure algebra, with no notion of a terminal's non-square
 *       character cells. A distance that has to *look* right on a text screen
 *       needs the caller to weight the y axis itself; nothing in this header
 *       does that for you.
 */

/**
 * @brief 2D vector of floats, passed and returned by value.
 *
 * @var Vec2f::x
 *   First component. Used as the column / horizontal axis by the screen-space
 *   code in this directory (raster.h, terminal.h), increasing rightwards.
 * @var Vec2f::y
 *   Second component. Those callers treat it as the row axis increasing
 *   *downwards* -- screen convention, not maths convention. Nothing in this
 *   header enforces that; it is purely how the neighbouring headers read it.
 */
typedef struct Vec2f {
  float x; // column, increasing rightwards
  float y; // row, increasing *downwards* -- screen convention, not maths
} Vec2f;
/**
 * @brief 3D vector of floats, passed and returned by value.
 *
 * @var Vec3f::x
 *   First component.
 * @var Vec3f::y
 *   Second component.
 * @var Vec3f::z
 *   Third component; depth, in the screen-space uses in this directory.
 */
typedef struct Vec3f {
  float x;
  float y;
  float z; // depth
} Vec3f;
/**
 * @brief 4D vector of floats, passed and returned by value.
 *
 * @var Vec4f::x
 *   First component.
 * @var Vec4f::y
 *   Second component.
 * @var Vec4f::z
 *   Third component.
 * @var Vec4f::w
 *   Fourth component: the homogeneous coordinate in a transform, or the alpha
 *   channel when the vector carries RGBA.
 */
typedef struct Vec4f {
  float x;
  float y;
  float z;
  float w; // homogeneous coordinate, or the fourth channel
} Vec4f;
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x and y are the sums of the corresponding inputs.
 *         Order does not matter; addition commutes.
 */
Vec2f vec2f_add(Vec2f a, Vec2f b) { return (Vec2f){a.x + b.x, a.y + b.y}; }
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x, y and z are the sums of the corresponding inputs.
 *         Order does not matter; addition commutes.
 */
Vec3f vec3f_add(Vec3f a, Vec3f b) {
  return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z};
}
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x, y, z and w are the sums of the corresponding
 *         inputs. Order does not matter; addition commutes.
 */
Vec4f vec4f_add(Vec4f a, Vec4f b) {
  return (Vec4f){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 2 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec2f vec2f_scale(Vec2f a, float x) { return (Vec2f){a.x * x, a.y * x}; }
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 3 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec3f vec3f_scale(Vec3f a, float x) {
  return (Vec3f){a.x * x, a.y * x, a.z * x};
}
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 4 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec4f vec4f_scale(Vec4f a, float x) {
  return (Vec4f){a.x * x, a.y * x, a.z * x, a.w * x};
}
/**
 * @brief Dot product of two 3D vectors.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return a.x*b.x + a.y*b.y + a.z*b.z. Zero when the two are perpendicular,
 *         positive when they point the same general way, negative when
 *         opposed. For unit-length inputs this is the cosine of the angle
 *         between them; otherwise it is scaled by both lengths.
 *
 * @note Only defined for the 3D types.
 */
float vec3f_dot(Vec3f a, Vec3f b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
/**
 * @brief Componentwise difference, a - b.
 *
 * @param a The vector to subtract *from* -- the destination, read as a point.
 * @param b The vector to subtract -- the origin.
 * @return The vector that goes from @p b to @p a. Argument order matters:
 *         swapping them negates the result.
 *
 * @note Only defined for the 3D types.
 */
Vec3f vec3f_sub(Vec3f a, Vec3f b) {
  return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}
/**
 * @brief Cross product of two 3D vectors.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector perpendicular to both inputs, with length equal to the area
 *         of the parallelogram they span -- so parallel inputs give the zero
 *         vector. Order matters: swapping the arguments flips the result.
 *
 * @note Only defined for the 3D types.
 */
Vec3f vec3f_cross(Vec3f a, Vec3f b) {
  return (Vec3f){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
/**
 * @brief 2D vector of doubles, passed and returned by value.
 *
 * @var Vec2d::x
 *   First component. Used as the column / horizontal axis by the screen-space
 *   code in this directory (raster.h, terminal.h), increasing rightwards.
 * @var Vec2d::y
 *   Second component. Those callers treat it as the row axis increasing
 *   *downwards* -- screen convention, not maths convention. Nothing in this
 *   header enforces that; it is purely how the neighbouring headers read it.
 */
typedef struct Vec2d {
  double x;
  double y;
} Vec2d;
/**
 * @brief 3D vector of doubles, passed and returned by value.
 *
 * @var Vec3d::x
 *   First component.
 * @var Vec3d::y
 *   Second component.
 * @var Vec3d::z
 *   Third component; depth, in the screen-space uses in this directory.
 */
typedef struct Vec3d {
  double x;
  double y;
  double z;
} Vec3d;
/**
 * @brief 4D vector of doubles, passed and returned by value.
 *
 * @var Vec4d::x
 *   First component.
 * @var Vec4d::y
 *   Second component.
 * @var Vec4d::z
 *   Third component.
 * @var Vec4d::w
 *   Fourth component: the homogeneous coordinate in a transform, or the alpha
 *   channel when the vector carries RGBA.
 */
typedef struct Vec4d {
  double x;
  double y;
  double z;
  double w;
} Vec4d;
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x and y are the sums of the corresponding inputs.
 *         Order does not matter; addition commutes.
 */
Vec2d vec2d_add(Vec2d a, Vec2d b) { return (Vec2d){a.x + b.x, a.y + b.y}; }
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x, y and z are the sums of the corresponding inputs.
 *         Order does not matter; addition commutes.
 */
Vec3d vec3d_add(Vec3d a, Vec3d b) {
  return (Vec3d){a.x + b.x, a.y + b.y, a.z + b.z};
}
/**
 * @brief Componentwise sum, a + b.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector whose x, y, z and w are the sums of the corresponding
 *         inputs. Order does not matter; addition commutes.
 */
Vec4d vec4d_add(Vec4d a, Vec4d b) {
  return (Vec4d){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 2 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec2d vec2d_scale(Vec2d a, double x) { return (Vec2d){a.x * x, a.y * x}; }
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 3 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec3d vec3d_scale(Vec3d a, double x) {
  return (Vec3d){a.x * x, a.y * x, a.z * x};
}
/**
 * @brief Multiply every component by a scalar.
 *
 * @param a Vector to scale.
 * @param x Scalar factor. Greater than 1 lengthens, between 0 and 1 shortens,
 *          and a negative value also reverses the direction. Note this is a
 *          scalar, not a vector -- it shares the name of the x component.
 * @return @p a with all 4 components multiplied by @p x. The direction is
 *         unchanged unless @p x is negative.
 */
Vec4d vec4d_scale(Vec4d a, double x) {
  return (Vec4d){a.x * x, a.y * x, a.z * x, a.w * x};
}
/**
 * @brief Dot product of two 3D vectors.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return a.x*b.x + a.y*b.y + a.z*b.z. Zero when the two are perpendicular,
 *         positive when they point the same general way, negative when
 *         opposed. For unit-length inputs this is the cosine of the angle
 *         between them; otherwise it is scaled by both lengths.
 *
 * @note Only defined for the 3D types.
 *
 * Double-precision mirror of Vec3f's version; componentwise identical.
 */
double vec3d_dot(Vec3d a, Vec3d b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
/**
 * @brief Componentwise difference, a - b.
 *
 * @param a The vector to subtract *from* -- the destination, read as a point.
 * @param b The vector to subtract -- the origin.
 * @return The vector that goes from @p b to @p a. Argument order matters:
 *         swapping them negates the result.
 *
 * @note Only defined for the 3D types.
 *
 * Double-precision mirror of Vec3f's version; componentwise identical.
 */
Vec3d vec3d_sub(Vec3d a, Vec3d b) {
  return (Vec3d){a.x - b.x, a.y - b.y, a.z - b.z};
}
/**
 * @brief Cross product of two 3D vectors.
 *
 * @param a First vector.
 * @param b Second vector.
 * @return A vector perpendicular to both inputs, with length equal to the area
 *         of the parallelogram they span -- so parallel inputs give the zero
 *         vector. Order matters: swapping the arguments flips the result.
 *
 * @note Only defined for the 3D types.
 *
 * Double-precision mirror of Vec3f's version; componentwise identical.
 */
Vec3d vec3d_cross(Vec3d a, Vec3d b) {
  return (Vec3d){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
#endif
