#include "math3d.h"
#include <math.h>

Vec3 vec3_make(float x, float y, float z)
{
    Vec3 v = { x, y, z };
    return v;
}

Vec3 vec3_sub(Vec3 a, Vec3 b)
{
    return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

Vec3 vec3_cross(Vec3 a, Vec3 b)
{
    return vec3_make(a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x);
}

float vec3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 vec3_normalize(Vec3 v)
{
    float len = sqrtf(vec3_dot(v, v));
    if (len <= 1e-8f)
        return vec3_make(0.0f, 0.0f, 0.0f);
    return vec3_make(v.x / len, v.y / len, v.z / len);
}

Mat4 mat4_identity(void)
{
    Mat4 r = { { { 0 } } };
    for (int i = 0; i < 4; i++)
        r.m[i][i] = 1.0f;
    return r;
}

Mat4 mat4_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a.m[i][k] * b.m[k][j];
            r.m[i][j] = sum;
        }
    }
    return r;
}

Vec4 mat4_mul_vec4(Mat4 a, Vec4 v)
{
    Vec4 r;
    r.x = a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w;
    r.y = a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w;
    r.z = a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w;
    r.w = a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w;
    return r;
}

Mat4 mat4_rotate_x(float angle)
{
    Mat4 r = mat4_identity();
    float c = cosf(angle), s = sinf(angle);
    r.m[1][1] = c;  r.m[1][2] = -s;
    r.m[2][1] = s;  r.m[2][2] = c;
    return r;
}

Mat4 mat4_rotate_y(float angle)
{
    Mat4 r = mat4_identity();
    float c = cosf(angle), s = sinf(angle);
    r.m[0][0] = c;  r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}

Mat4 mat4_scale(float s)
{
    Mat4 r = mat4_identity();
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}

Mat4 mat4_translate(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}

Mat4 mat4_perspective(float fovy_rad, float aspect, float znear, float zfar)
{
    Mat4 r = { { { 0 } } };
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (zfar + znear) / (znear - zfar);
    r.m[2][3] = (2.0f * zfar * znear) / (znear - zfar);
    r.m[3][2] = -1.0f;
    return r;
}

Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up)
{
    Vec3 f = vec3_normalize(vec3_sub(target, eye));
    Vec3 s = vec3_normalize(vec3_cross(f, up));
    Vec3 u = vec3_cross(s, f);

    Mat4 r = mat4_identity();
    r.m[0][0] = s.x; r.m[0][1] = s.y; r.m[0][2] = s.z; r.m[0][3] = -vec3_dot(s, eye);
    r.m[1][0] = u.x; r.m[1][1] = u.y; r.m[1][2] = u.z; r.m[1][3] = -vec3_dot(u, eye);
    r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z; r.m[2][3] = vec3_dot(f, eye);
    return r;
}
