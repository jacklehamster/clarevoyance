#include "texture.h"

#include <cstring>
#include <vector>

#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

namespace cv {

TextureArray createTextureArray(int size, int layers) {
    TextureArray arr;
    if (size <= 0 || layers <= 0) {
        SDL_Log("createTextureArray: invalid size=%d layers=%d", size, layers);
        return arr;
    }

    glGenTextures(1, &arr.id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr.id);
    // GL_RGBA8 is a sized internal format — valid in both GL 3.3 core and
    // GLES 3.0 / WebGL 2 (unsized formats are not portable for texImage3D).
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, layers, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    arr.size = size;
    arr.layers = layers;
    arr.used = 0;
    return arr;
}

int loadSheetIntoArray(TextureArray& arr, const char* path,
                       int colsInFile, int rowsInFile, SheetGrid& outGrid) {
    if (!arr.valid()) {
        SDL_Log("loadSheetIntoArray: texture array not initialised");
        return -1;
    }
    if (arr.used >= arr.layers) {
        SDL_Log("loadSheetIntoArray: out of layers (%d) loading '%s'",
                arr.layers, path);
        return -1;
    }
    if (colsInFile <= 0 || rowsInFile <= 0) {
        SDL_Log("loadSheetIntoArray: invalid grid %dx%d for '%s'",
                colsInFile, rowsInFile, path);
        return -1;
    }

    // No vertical flip: sprite-sheet cells are indexed top-to-bottom in the
    // shader, and the quad's UVs put v=0 at the top corner.
    stbi_set_flip_vertically_on_load(0);
    int imgW = 0, imgH = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &imgW, &imgH, &channels, 4);
    if (!pixels) {
        SDL_Log("Sheet load failed for '%s': %s", path, stbi_failure_reason());
        return -1;
    }

    const int cellW = imgW / colsInFile;
    const int cellH = imgH / rowsInFile;
    if (cellW <= 0 || cellH <= 0 || cellW > arr.size || cellH > arr.size) {
        SDL_Log("loadSheetIntoArray: cell %dx%d of '%s' does not fit a %d layer",
                cellW, cellH, path, arr.size);
        stbi_image_free(pixels);
        return -1;
    }

    // Repack cells row-major into the layer. Flat cell order is preserved, so
    // animation frame indices are unchanged — only the wrap width differs.
    const int packCols = arr.size / cellW;
    const int packRows = arr.size / cellH;
    const int nCells = colsInFile * rowsInFile;
    if (nCells > packCols * packRows) {
        SDL_Log("loadSheetIntoArray: %d cells of '%s' exceed layer capacity %d",
                nCells, path, packCols * packRows);
        stbi_image_free(pixels);
        return -1;
    }

    std::vector<unsigned char> layer(
        static_cast<size_t>(arr.size) * arr.size * 4, 0);
    const size_t srcPitch = static_cast<size_t>(imgW) * 4;
    const size_t dstPitch = static_cast<size_t>(arr.size) * 4;
    const size_t cellRowBytes = static_cast<size_t>(cellW) * 4;
    for (int cell = 0; cell < nCells; ++cell) {
        const int srcCol = cell % colsInFile;
        const int srcRow = cell / colsInFile;
        const int dstCol = cell % packCols;
        const int dstRow = cell / packCols;
        for (int y = 0; y < cellH; ++y) {
            const unsigned char* src = pixels
                + (static_cast<size_t>(srcRow) * cellH + y) * srcPitch
                + static_cast<size_t>(srcCol) * cellRowBytes;
            unsigned char* dst = layer.data()
                + (static_cast<size_t>(dstRow) * cellH + y) * dstPitch
                + static_cast<size_t>(dstCol) * cellRowBytes;
            std::memcpy(dst, src, cellRowBytes);
        }
    }
    stbi_image_free(pixels);

    const int layerIndex = arr.used;
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr.id);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layerIndex,
                    arr.size, arr.size, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                    layer.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    arr.used++;

    outGrid.cols = static_cast<float>(packCols);
    outGrid.cellU = static_cast<float>(cellW) / arr.size;
    outGrid.cellV = static_cast<float>(cellH) / arr.size;

    SDL_Log("Loaded sheet '%s' (%dx%d, %d cells) into layer %d (repacked %d/row)",
            path, imgW, imgH, nCells, layerIndex, packCols);
    return layerIndex;
}

void destroyTextureArray(TextureArray& arr) {
    if (arr.id) {
        glDeleteTextures(1, &arr.id);
        arr.id = 0;
    }
    arr.size = arr.layers = arr.used = 0;
}

} // namespace cv
