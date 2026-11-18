// text.cpp — see text.h.
#include "text.h"
#include "font_metrics.h"

namespace cv {

std::vector<Instance> makeText(const std::string& s, Vec3 pos,
                                float charW, float charH,
                                int fontSheet, Vec4 tint) {
    std::vector<Instance> glyphs;
    glyphs.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {
        int ascii = static_cast<int>(static_cast<unsigned char>(s[i]));
        int frame = ascii - FONT_FIRST_ASCII;
        if (frame < 0 || frame >= FONT_NUM_GLYPHS)
            frame = 0;   // out-of-range byte → blank "space" cell, not a wrong glyph

        Vec3 gpos = pos + Vec3{charW * static_cast<float>(i), 0.0f, 0.0f};
        // Upright, non-billboard quad (yaw 0, pitch 0) — the renderer doesn't
        // cull backfaces, so it reads correctly from either side, same as walls.
        Instance inst = makeQuad(gpos, {charW, charH}, 0.0f, 0.0f);
        inst.sheet = static_cast<float>(fontSheet);
        inst.tint  = tint;
        setAnimation(inst, frame, 1, 0.0f, 0.0f);
        glyphs.push_back(inst);
    }
    return glyphs;
}

std::vector<Instance> makeLabel(const std::string& s, Vec3 anchor,
                                 float glyphSize, float advance, float drop,
                                 int fontSheet, Vec4 tint) {
    std::vector<Instance> glyphs;
    glyphs.reserve(s.size());
    const float half = (static_cast<float>(s.size()) - 1.0f) * 0.5f;

    for (size_t i = 0; i < s.size(); ++i) {
        int ascii = static_cast<int>(static_cast<unsigned char>(s[i]));
        int frame = ascii - FONT_FIRST_ASCII;
        if (frame < 0 || frame >= FONT_NUM_GLYPHS) frame = 0;
        if (frame == 0) continue;   // a space needs no quad

        Instance inst = makeBillboard(anchor, {glyphSize, glyphSize});
        inst.billboard = BILLBOARD_GROUNDED;
        inst.offset = {(static_cast<float>(i) - half) * advance,
                       -(drop + glyphSize * 0.5f)};
        inst.sheet = static_cast<float>(fontSheet);
        inst.tint  = tint;
        setAnimation(inst, frame, 1, 0.0f, 0.0f);
        glyphs.push_back(inst);
    }
    return glyphs;
}

namespace {

// Glyph cell index of a byte, or -1 for anything the font doesn't cover.
int glyphOf(char c) {
    int frame = static_cast<int>(static_cast<unsigned char>(c)) - FONT_FIRST_ASCII;
    return (frame < 0 || frame >= FONT_NUM_GLYPHS) ? -1 : frame;
}

float advanceOf(char c, float size) {
    int g = glyphOf(c);
    return g < 0 ? 0.0f : FONT_ADVANCE[g] * size;
}

} // namespace

float textWidth(const std::string& s, float size) {
    float w = 0.0f;
    for (char c : s) w += advanceOf(c, size);
    return w;
}

TextBlock makeTextBlock(const std::string& s, float x, float yTop, float size,
                        float maxWidth, float lineHeight, float z,
                        int fontSheet, Vec4 tint) {
    TextBlock block;
    float pen = 0.0f;       // distance along the current line
    int line = 0;
    bool any = false;

    auto newLine = [&]() {
        if (pen > block.width) block.width = pen;
        pen = 0.0f;
        ++line;
    };

    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\n') { newLine(); any = true; ++i; continue; }
        if (s[i] == ' ') {
            if (pen > 0.0f) pen += advanceOf(' ', size);   // no indent after a wrap
            ++i;
            continue;
        }
        // One word: everything up to the next space or line break.
        size_t end = i;
        while (end < s.size() && s[end] != ' ' && s[end] != '\n') ++end;
        const std::string word = s.substr(i, end - i);
        if (pen > 0.0f && pen + textWidth(word, size) > maxWidth) newLine();

        for (char c : word) {
            const int g = glyphOf(c);
            const float adv = advanceOf(c, size);
            if (g > 0) {
                // The glyph sits centred in its cell; the cell is centred on
                // the letter's own advance.
                Instance inst = makeQuad(
                    {x + pen + adv * 0.5f, yTop - size * 0.5f - line * lineHeight, z},
                    {size, size}, 0.0f, 0.0f);
                inst.layer = LAYER_OVERLAY;
                inst.sheet = static_cast<float>(fontSheet);
                inst.tint  = tint;
                setAnimation(inst, g, 1, 0.0f, 0.0f);
                block.glyphs.push_back(inst);
            }
            pen += adv;
        }
        any = true;
        i = end;
    }
    if (pen > block.width) block.width = pen;
    block.lines = any ? line + 1 : 0;
    return block;
}

} // namespace cv
