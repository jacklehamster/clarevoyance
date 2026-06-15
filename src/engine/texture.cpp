#include "texture.h"

#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

namespace cv {

Texture loadTexture(const char* path) {
    Texture tex;

    // No vertical flip: we index sprite-sheet cells top-to-bottom in the shader,
    // and the quad's UVs put v=0 at the top corner. Keeps cell math intuitive.
    stbi_set_flip_vertically_on_load(0);
    int channels = 0;
    unsigned char* pixels = stbi_load(path, &tex.width, &tex.height, &channels, 4);
    if (!pixels) {
        SDL_Log("Texture load failed for '%s': %s", path, stbi_failure_reason());
        return tex;
    }

    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex.width, tex.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    SDL_Log("Loaded texture '%s' (%dx%d)", path, tex.width, tex.height);
    return tex;
}

void destroyTexture(Texture& tex) {
    if (tex.id) {
        glDeleteTextures(1, &tex.id);
        tex.id = 0;
    }
}

} // namespace cv
