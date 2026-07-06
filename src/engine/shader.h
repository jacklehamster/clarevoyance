// shader.h — GLSL compile/link helpers and a thin uniform-setter wrapper.
#pragma once

#include "gl.h"
#include "mathx.h"

namespace cv {

// Compiles a single shader stage. Logs and returns 0 on failure.
GLuint compileShader(GLenum type, const char* src);

// Compiles + links a vertex/fragment program. Logs and returns 0 on failure.
GLuint buildProgram(const char* vertSrc, const char* fragSrc);

// Uniform helpers. Each looks the location up by name; fine at our call volume,
// and keeps call sites readable. Cache locations later if it ever shows up hot.
void setUniform(GLuint program, const char* name, float v);
void setUniform(GLuint program, const char* name, int v);
void setUniform(GLuint program, const char* name, Vec3 v);
void setUniform(GLuint program, const char* name, const Mat4& v);
void setUniformArray(GLuint program, const char* name, const Vec4* v, int count);

} // namespace cv
