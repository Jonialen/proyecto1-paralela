#ifndef MATH3D_H
#define MATH3D_H

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Vec4;

/* Row-major 4x4 matrix: m[row][col]. */
typedef struct { float m[4][4]; } Mat4;

Vec3 vec3_make(float x, float y, float z);
Vec3 vec3_sub(Vec3 a, Vec3 b);
Vec3 vec3_cross(Vec3 a, Vec3 b);
float vec3_dot(Vec3 a, Vec3 b);
Vec3 vec3_normalize(Vec3 v);

Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);
Vec4 mat4_mul_vec4(Mat4 a, Vec4 v);

Mat4 mat4_rotate_x(float angle);
Mat4 mat4_rotate_y(float angle);
Mat4 mat4_scale(float s);
Mat4 mat4_translate(float x, float y, float z);
Mat4 mat4_perspective(float fovy_rad, float aspect, float znear, float zfar);
Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up);

#endif /* MATH3D_H */
