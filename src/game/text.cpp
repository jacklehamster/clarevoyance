// text.cpp — see text.h.
#include "text.h"

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

} // namespace cv
