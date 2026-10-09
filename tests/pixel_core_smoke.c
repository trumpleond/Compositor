#include "BrushPixels.h"
#include "DitherPixels.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0, sizeof pixels);
    pixels[(1 * 4 + 1) * 4 + 3] = 255;
    pixels[(2 * 4 + 2) * 4 + 3] = 255;

    size_t bounds[4] = { 0, 0, 0, 0 };
    brush_alpha_bounds(pixels, 4, 4, 16, bounds);
    if (bounds[0] != 1 || bounds[1] != 1 || bounds[2] != 3 || bounds[3] != 3) {
        fprintf(stderr, "unexpected alpha bounds\n");
        return 1;
    }

    DitherParams params = { 0 };
    params.style = DITHER_BAYER_2;
    params.levels = 2;
    params.diffusion = 1.0f;
    params.contrast = 0.0f;
    params.cell = 2;
    params.dark[0] = params.dark[1] = params.dark[2] = 0;
    params.light[0] = params.light[1] = params.light[2] = 255;

    if (!dither_apply(pixels, 4, 4, 16, &params)) {
        fprintf(stderr, "dither_apply failed\n");
        return 1;
    }
    return 0;
}
