#include "shader.h"

#include <SDL.h>

namespace cv {

static const char* shaderHeader(GLenum type) {
#ifdef __EMSCRIPTEN__
    if (type == GL_FRAGMENT_SHADER)
        return "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    return "#version 300 es\n";
#else
    (void)type;
    return "#version 330 core\n";
#endif
}

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    const char* parts[2] = { shaderHeader(type), src };
    glShaderSource(s, 2, parts, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        SDL_Log("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint buildProgram(const char* vertSrc, const char* fragSrc) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        SDL_Log("Program link error: %s", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

void setUniform(GLuint program, const char* name, float v) {
    glUniform1f(glGetUniformLocation(program, name), v);
}

void setUniform(GLuint program, const char* name, int v) {
    glUniform1i(glGetUniformLocation(program, name), v);
}

void setUniform(GLuint program, const char* name, Vec3 v) {
    glUniform3f(glGetUniformLocation(program, name), v.x, v.y, v.z);
}

void setUniform(GLuint program, const char* name, const Mat4& v) {
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, v.m);
}

void setUniformArray(GLuint program, const char* name, const Vec4* v, int count) {
    glUniform4fv(glGetUniformLocation(program, name), count,
                 reinterpret_cast<const float*>(v));
}

} // namespace cv
