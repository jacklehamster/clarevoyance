// gl.h — single point of inclusion for the OpenGL headers.
//
// Every engine translation unit includes this instead of a platform GL header
// directly. When we add the browser (Emscripten/WebGL) and Switch (GLES) targets,
// the platform branching lives here and nowhere else.
#pragma once

#if defined(__APPLE__)
    #include <OpenGL/gl3.h>
#elif defined(__EMSCRIPTEN__)
    #include <GLES3/gl3.h>
#else
    // Linux desktop: GLEW loads all GL function pointers at runtime
    #include <GL/glew.h>
#endif
