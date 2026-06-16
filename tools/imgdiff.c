// imgdiff.c — pixel-diff two PNGs. Exits 0 if mean abs diff is within
// tolerance, 1 otherwise. Uses the already-vendored stb_image.
//
// Usage: imgdiff <a.png> <b.png> [tolerance]
//   tolerance: mean abs per-channel diff 0-255 (default 6 ≈ 2.5%)
//
// Build: clang -O2 -Isrc/engine -o tools/imgdiff tools/imgdiff.c
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "../src/engine/third_party/stb_image.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: imgdiff <a.png> <b.png> [tolerance=6]\n");
        return 2;
    }
    double tol = argc >= 4 ? atof(argv[3]) : 6.0;

    int wa, ha, ca, wb, hb, cb;
    unsigned char* a = stbi_load(argv[1], &wa, &ha, &ca, 4);
    unsigned char* b = stbi_load(argv[2], &wb, &hb, &cb, 4);

    if (!a) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    if (!b) { fprintf(stderr, "cannot load %s\n", argv[2]); return 2; }

    if (wa != wb || ha != hb) {
        fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", wa, ha, wb, hb);
        stbi_image_free(a); stbi_image_free(b);
        return 1;
    }

    long long total = 0;
    long long diffPixels = 0;
    int pixels = wa * ha;
    for (int i = 0; i < pixels * 4; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        total += d;
        if (i % 4 != 3 && d > 15) diffPixels++; // ignore alpha channel for pixel count
    }
    stbi_image_free(a); stbi_image_free(b);

    double meanDiff = (double)total / (pixels * 4);
    double pctDiff  = (double)diffPixels / pixels * 100.0;

    printf("mean abs diff: %.2f/255  |  %.1f%% pixels differ (>15 per channel)\n",
           meanDiff, pctDiff);

    if (meanDiff > tol) {
        printf("FAIL: %.2f > tolerance %.2f\n", meanDiff, tol);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
