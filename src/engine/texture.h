// texture.h — loads a PNG sprite sheet into a single GL texture.
//
// Milestone 1 deliberately supports one texture (one sprite sheet) for the whole
// instance buffer. Multiple textures / atlases are a planned later evolution.
#pragma once

#include "gl.h"

namespace cv {

struct Texture {
    GLuint id = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return id != 0; }
};

// Loads an RGBA PNG from disk. Logs and returns an invalid Texture on failure.
// Uses nearest filtering by default (crisp pixel-art sprites).
Texture loadTexture(const char* path);

void destroyTexture(Texture& tex);

} // namespace cv
