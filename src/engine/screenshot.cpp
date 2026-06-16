#include "screenshot.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "gl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "third_party/stb_image_write.h"
#pragma clang diagnostic pop

namespace cv {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::vector<unsigned char> readPixels(int w, int h) {
    std::vector<unsigned char> buf(static_cast<size_t>(w * h * 4));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    // Flip vertically: GL origin is bottom-left, image origin is top-left.
    std::vector<unsigned char> flipped(buf.size());
    const size_t rowBytes = static_cast<size_t>(w * 4);
    for (int y = 0; y < h; ++y)
        memcpy(flipped.data() + y * rowBytes,
               buf.data() + (h - 1 - y) * rowBytes, rowBytes);
    return flipped;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64Print(const unsigned char* data, size_t len) {
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (unsigned int)data[i] << 16;
        if (i + 1 < len) b |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) b |= (unsigned int)data[i + 2];
        putchar(B64[(b >> 18) & 0x3F]);
        putchar(B64[(b >> 12) & 0x3F]);
        putchar(i + 1 < len ? B64[(b >>  6) & 0x3F] : '=');
        putchar(i + 2 < len ? B64[(b      ) & 0x3F] : '=');
    }
    putchar('\n');
    fflush(stdout);
}

// stb_image_write callback that accumulates into a vector<unsigned char>.
static void pngAccum(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void captureFramebufferBase64(int w, int h) {
    auto pixels = readPixels(w, h);

    std::vector<unsigned char> png;
    stbi_write_png_to_func(pngAccum, &png, w, h, 4, pixels.data(), w * 4);

    puts("CV_SHOT_BEGIN");
    base64Print(png.data(), png.size());
    puts("CV_SHOT_END");
    fflush(stdout);
}

bool framebufferNonBlank(int w, int h) {
    auto pixels = readPixels(w, h);
    // Clear colour is (0.08, 0.08, 0.12) → (20, 20, 31) in 8-bit.
    // Count pixels that differ from it by more than a small tolerance.
    int nonBg = 0;
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* p = pixels.data() + i * 4;
        if (abs((int)p[0] - 20) > 10 ||
            abs((int)p[1] - 20) > 10 ||
            abs((int)p[2] - 31) > 10)
            nonBg++;
    }
    return nonBg > (w * h) / 100; // at least 1% non-background pixels
}

} // namespace cv
