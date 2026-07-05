// texture.h — sprite-sheet storage: one GL_TEXTURE_2D_ARRAY shared by all sheets.
//
// Every sprite sheet the game loads becomes one layer of a single array texture
// (SHEET_LAYER_SIZE² per layer). Cells from the source PNG are repacked row-major
// into the layer, so source sheets of any shape fit as long as their cells do.
// The renderer binds the one array once — multiple sheets never cost extra
// texture binds or draw calls.
#pragma once

#include "gl.h"

namespace cv {

struct TextureArray {
    GLuint id = 0;
    int size = 0;     // layer width == height (square layers)
    int layers = 0;   // allocated layer count
    int used = 0;     // layers filled so far

    bool valid() const { return id != 0; }
};

// How one loaded sheet's cells are laid out inside its layer — the shader needs
// this to map a flat cell index to a UV rectangle (see uSheetGrid in renderer.cpp).
struct SheetGrid {
    float cols = 1.0f;   // columns per row after repacking into the layer
    float cellU = 1.0f;  // cell width  / layer size (UV scale)
    float cellV = 1.0f;  // cell height / layer size (UV scale)
};

// Allocates an RGBA8 array texture of `layers` square layers (`size`×`size`),
// nearest-filtered for crisp pixel art. Logs and returns invalid on failure.
TextureArray createTextureArray(int size, int layers);

// Loads an RGBA PNG laid out as a colsInFile × rowsInFile grid of equal cells,
// repacks the cells row-major into the next free layer (cell order — and thus
// every animation frame index — is preserved), and returns the layer index.
// Returns -1 on failure (file missing, cells too large, out of layers).
int loadSheetIntoArray(TextureArray& arr, const char* path,
                       int colsInFile, int rowsInFile, SheetGrid& outGrid);

void destroyTextureArray(TextureArray& arr);

} // namespace cv
