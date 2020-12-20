#include <math.h>
#include <SDL_image.h>
#include "platform.h"
#include "tiles.h"

#define PI  3.14159265358979323846

#define PNG_WIDTH    2048   // width (px) of the source PNG
#define PNG_HEIGHT   5568   // height (px) of the source PNG
#define TILE_WIDTH    128   // width (px) of a single tile in the source PNG
#define TILE_HEIGHT   232   // height (px) of a single tile in the source PNG
#define TILE_ROWS      24   // number of rows in the source PNG
#define TILE_COLS      16   // number of columns in the source PNG
#define TEXT_X_HEIGHT 100   // height (px) of the 'x' outline
#define TEXT_BASELINE  46   // height (px) of the blank space below the 'x' outline
#define MAX_TILE_SIZE  64   // maximum width or height (px) of screen tiles

// How each tile should be processed:
//  -  's' = stretch: tile stretches to fill the space
//  -  'f' = fit: preserve aspect ratio (but tile can stretch up to 20%)
//  -  't' = text: characters must line up vertically (max. stretch 40%)
//  -  '#' = same as 't' but allow vertical sub-pixel alignment
static const char TileProcessing[TILE_ROWS][TILE_COLS+1] = {
    "ffffffffffffffff", "ffffffffffffffff", "#t##########t#t#", "tttttttttttt###t",
    "#ttttttttttttttt", "ttttttttttt#####", "#ttttttttttttttt", "ttttttttttt#####",
    "################", "################", "################", "################",
    "tttttttttttttttt", "ttttttt#tttttttt", "tttttttttttttttt", "ttttttt#tttttttt",
    "ffsfsfsffsssssss", "ssfsfsffffffffff", "fffffffffffffsff", "ffffffffffffffff",
    "fsssfffffffffffs", "fsffffffffffffff", "ffffssssffssffff", "ffffsfffffssssff"
};

typedef struct ScreenTile {
    short charIndex;
    short foreRed, foreGreen, foreBlue;
    short backRed, backGreen, backBlue;
} ScreenTile;

static SDL_Surface *TilesPNG;
static SDL_Texture *Textures[4];
static int numTextures = 0;
static int8_t tilePadding[TILE_ROWS][TILE_COLS];
static int8_t tileShifts[TILE_ROWS][TILE_COLS][2][MAX_TILE_SIZE][3];
static boolean tileEmpty[TILE_ROWS][TILE_COLS];
static ScreenTile screenTiles[ROWS][COLS];
static int baseTileWidth = -1;
static int baseTileHeight = -1;

SDL_Window *Win = NULL;
int windowWidth = -1;
int windowHeight = -1;
boolean fullScreen = false;


static void sdlfatal(char *file, int line) {
    fprintf(stderr, "Fatal SDL error (%s:%d): %s\n", file, line, SDL_GetError());
    exit(1);
}


static void imgfatal(char *file, int line) {
    fprintf(stderr, "Fatal SDL_image error (%s:%d): %s\n", file, line, IMG_GetError());
    exit(1);
}


static int getPadding(int row, int column) {
    int padding;
    Uint32 *pixels = TilesPNG->pixels; // each pixel is encoded as 0xffBBGGRR
    for (padding = 0; padding < TILE_HEIGHT / 4; padding++) {
        for (int x = 0; x < TILE_WIDTH; x++) {
            int y1 = padding;
            int y2 = TILE_HEIGHT - padding - 1;
            if (pixels[(x + column * TILE_WIDTH) + (y1 + row * TILE_HEIGHT) * PNG_WIDTH] & 0xffU ||
                pixels[(x + column * TILE_WIDTH) + (y2 + row * TILE_HEIGHT) * PNG_WIDTH] & 0xffU)
            {
                return padding;
            }
        }
    }
    return padding;
}


static boolean isTileEmpty(int row, int column) {
    Uint32 *pixels = TilesPNG->pixels; // each pixel is encoded as 0xffBBGGRR
    for (int y = 0; y < TILE_HEIGHT; y++) {
        for (int x = 0; x < TILE_WIDTH; x++) {
            if (pixels[(x + column * TILE_WIDTH) + (y + row * TILE_HEIGHT) * PNG_WIDTH] & 0xffU) {
                return false;
            }
        }
    }
    return true;
}


static uint64_t xorshift64s(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545f4914f6cdd1dU;
}


static uint64_t noise(uint64_t *state) {
    return (xorshift64s(state) >> 54) + 0x100000280U;
}


static double prepareTile(SDL_Surface *surface, int tileWidth, int tileHeight, int row, int column, boolean optimizing) {
    int8_t noShifts[3] = {0, 0, 0};
    int padding = tilePadding[row][column];         // how much blank spaces there is at the top and bottom of the source tile
    char processing = TileProcessing[row][column];  // how should this tile be processed?

    // Size of the area the glyph must fit into
    int fitWidth = max(1, baseTileWidth);
    int fitHeight = max(1, baseTileHeight);

    // Number of sine waves that can fit in the tile (for wall tops)
    const int numHorizWaves = max(2, min(6, round(fitWidth * .25)));
    const int numVertWaves = max(2, min(11, round(fitHeight * .25)));

    // Size of the downscaled glyph
    int glyphWidth, glyphHeight;

    // accumulator for pixel values (linear color space), encoded as
    // 0xCCCCCCCCSSSSSSSS where C is a counter and S is a sum of squares
    uint64_t *values = malloc(tileWidth * tileHeight * 8);
    memset(values, 0, tileWidth * tileHeight * 8);
    double blur = 0;

    // if the tile is empty, we can skip the downscaling
    if (tileEmpty[row][column]) goto downscaled;

    // decide how large we can draw the glyph
    if (processing == 's' || optimizing) {
        // stretch
        glyphWidth = fitWidth = tileWidth;
        glyphHeight = fitHeight = tileHeight;
    } else if (processing == 'f') {
        // fit
        double stretch = max(1.0, min(1.2, (double)(fitWidth * TILE_HEIGHT) / (fitHeight * TILE_WIDTH)));
        glyphWidth = max(1, min(fitWidth, round(1.2 * fitHeight * TILE_WIDTH / (TILE_HEIGHT - 2 * padding))));
        glyphHeight = max(1, min(fitHeight, round(stretch * fitWidth * (TILE_HEIGHT - 2 * padding) / TILE_WIDTH)));
    } else {
        // text
        glyphWidth = max(1, min(fitWidth, round(1.4 * fitHeight * TILE_WIDTH / TILE_HEIGHT)));
        glyphHeight = max(1, min(fitHeight, round(1.4 * fitWidth * TILE_HEIGHT / TILE_WIDTH)));
    }

    // map source pixels to target pixels...
    int scaledX[TILE_WIDTH], scaledY[TILE_HEIGHT];
    int stop0, stop1, stop2, stop3, stop4;
    double map0, map1, map2, map3, map4;
    int8_t *shifts;

    // ... horizontally:

    stop0 = 0;
    stop1 = TILE_WIDTH / 5; // 20%
    stop2 = TILE_WIDTH / 2; // 50%
    stop3 = TILE_WIDTH * 4/5; // 80%
    stop4 = TILE_WIDTH;

    shifts = (glyphWidth > MAX_TILE_SIZE ? noShifts : tileShifts[row][column][0][glyphWidth - 1]);
    map0 = (fitWidth - glyphWidth + (shifts[0] + shifts[1] < 0 ? 1 : 0)) / 2;
    map1 = map0 + glyphWidth * (double)(stop1 - stop0) / (stop4 - stop0) + shifts[0] * 0.1;
    map2 = map0 + glyphWidth * (double)(stop2 - stop0) / (stop4 - stop0) + shifts[2] * 0.1;
    map3 = map0 + glyphWidth * (double)(stop3 - stop0) / (stop4 - stop0) + shifts[1] * 0.1;
    map4 = map0 + glyphWidth;

    for (int x = stop0; x < stop1; x++) scaledX[x] = map0 + (map1 - map0) * (x - stop0) / (stop1 - stop0);
    for (int x = stop1; x < stop2; x++) scaledX[x] = map1 + (map2 - map1) * (x - stop1) / (stop2 - stop1);
    for (int x = stop2; x < stop3; x++) scaledX[x] = map2 + (map3 - map2) * (x - stop2) / (stop3 - stop2);
    for (int x = stop3; x < stop4; x++) scaledX[x] = map3 + (map4 - map3) * (x - stop3) / (stop4 - stop3);

    // ... vertically:

    if (processing == 't') {
        stop4 = TILE_HEIGHT;
        stop3 = stop4 - TEXT_BASELINE;
        stop2 = stop3 - TEXT_X_HEIGHT;
        stop1 = stop2 / 3;
        stop0 = 0;
    } else {
        stop0 = padding;
        stop4 = TILE_HEIGHT - padding;
        stop1 = stop0 + (stop4 - stop0) / 5;    // 20%
        stop2 = stop0 + (stop4 - stop0) / 2;    // 50%
        stop3 = stop0 + (stop4 - stop0) * 4/5;  // 80%
    }

    map0 = (fitHeight - glyphHeight) / 2;
    map1 = map0 + glyphHeight * (double)(stop1 - stop0) / (stop4 - stop0);
    map2 = map0 + glyphHeight * (double)(stop2 - stop0) / (stop4 - stop0);
    map3 = map0 + glyphHeight * (double)(stop3 - stop0) / (stop4 - stop0);
    map4 = map0 + glyphHeight;

    if (processing == 't') {
        // align stops #2 and #3 with output pixels
        map3 += round(map2) - map2;
        map2 = round(map2);
        map3 = max(map2 + 1, round(map3));
        map1 = map0 + (map2 - map0) / 3;
    }

    shifts = (glyphHeight > MAX_TILE_SIZE ? noShifts : tileShifts[row][column][1][glyphHeight - 1]);
    map1 += shifts[0] * 0.1;
    map2 += shifts[2] * 0.1;
    map3 += shifts[1] * 0.1;

    for (int y = 0; y < stop0; y++) scaledY[y] = -1; // not mapped
    for (int y = stop0; y < stop1; y++) scaledY[y] = map0 + (map1 - map0) * (y - stop0) / (stop1 - stop0);
    for (int y = stop1; y < stop2; y++) scaledY[y] = map1 + (map2 - map1) * (y - stop1) / (stop2 - stop1);
    for (int y = stop2; y < stop3; y++) scaledY[y] = map2 + (map3 - map2) * (y - stop2) / (stop3 - stop2);
    for (int y = stop3; y < stop4; y++) scaledY[y] = map3 + (map4 - map3) * (y - stop3) / (stop4 - stop3);
    for (int y = stop4; y < TILE_HEIGHT; y++) scaledY[y] = -1; // not mapped

    // downscale source tile to accumulator
    for (int y0 = 0; y0 < TILE_HEIGHT; y0++) {
        int y1 = scaledY[y0];
        if (y1 < 0 || y1 >= tileHeight) continue;
        uint64_t *dst = &values[y1 * tileWidth];
        Uint32 *src = TilesPNG->pixels; // each pixel is encoded as 0xffBBGGRR
        src += (column * TILE_WIDTH) + (row * TILE_HEIGHT + y0) * PNG_WIDTH;
        for (int x0 = 0; x0 < TILE_WIDTH; x0++) {
            uint64_t value = src[x0] & 0xffU;
            dst[scaledX[x0]] += (value * value) | 0x100000000U; // (gamma = 2.0, count = 1)
        }
        // interpolate skipped lines, if any
        if (y1 >= 2 && scaledY[y0 - 1] == y1 - 2) {
            for (int x1 = 0; x1 < tileWidth; x1++) {
                dst[x1 - tileWidth] = dst[x1 - 2*tileWidth] + dst[x1];
            }
        }
    }
    downscaled:

    // add floor dust (if the floor tile is blank)
    if (row == 20 && column == 2 && tileEmpty[row][column] && tileWidth > 2 && tileHeight > 2 && !optimizing) {
        int w = tileWidth - 2;
        int h = tileHeight - 2;
        uint64_t state = 1234567;
        uint32_t *idx = malloc(w * h * 4);

        // stitch edges together
        for (int x = 0; x < w; x += 4) values[x] = noise(&state);
        for (int y = 0; y < h; y += 4) values[y * tileWidth] = noise(&state);
        for (int x = 2; x < w; x += 4) values[(h+1) * tileWidth + x] = noise(&state);
        for (int y = 2; y < h; y += 4) values[y * tileWidth + (w+1)] = noise(&state);

        // fill center with isolated dots, randomly placed
        for (int i = 0; i < w * h; i++) idx[i] = i;             // array of indexes
        for (int i = 0; i < w * h - 1; i++) {                   // shuffle the array
            int j = xorshift64s(&state) % (w * h - i) + i;      // Fisher–Yates shuffle
            uint64_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;   // swap indexes
        }
        for (int i = 0; i < w * h; i++) {
            int x = 1 + (idx[i] % w);
            int y = 1 + (idx[i] / w);
            int p = x + y * tileWidth;
            if (!values[p+1] &&
                !values[p-1] &&
                !values[p+tileWidth] &&
                !values[p+tileWidth+1] &&
                !values[p+tileWidth-1] &&
                !values[p-tileWidth] &&
                !values[p-tileWidth+1] &&
                !values[p-tileWidth-1])
            {
                values[p] = noise(&state);
            }
        }
        free(idx);
    }

    // add wall tops: diagonal sine waves
    if ((row == 16 && column == 2 || row == 21 && column == 1 || row == 22 && column == 4) && !optimizing) {
        for (int y = 0; y < tileHeight; y++) {
            if (row != 21 && (y > tileHeight / 2 || (values[y * tileWidth] & 0xffffffffU))) break;
            for (int x = 0; x < tileWidth; x++) {
                double value = sin(2. * PI * ((double)x / tileWidth * numHorizWaves
                                            + (double)y / tileHeight * numVertWaves)) / 2. + 0.5;
                values[y * tileWidth + x] = (uint64_t)round(255 * 255 * value * value) | 0x100000000U;
            }
        }
    }

    // convert accumulator to image transparency
    for (int y = 0; y < tileHeight; y++) {
        Uint32 *pixel = surface->pixels; // each pixel is encoded as 0xAABBGGRR
        pixel += (column * tileWidth) + (row * tileHeight + y) * surface->w;
        for (int x = 0; x < tileWidth; x++) {
            uint64_t value = values[y * tileWidth + x];

            // average light intensity (linear scale, 0 .. 255*255)
            value = ((value >> 32) ? (value & 0xffffffffU) / (value >> 32) : 0);

            // metric for "blurriness": black (0) and white (255*255) pixels count for 0, gray pixels for 1
            if (optimizing) blur += sin(PI/(255*255) * value);

            // make text look less bold, at the cost of accuracy
            if (processing == 't' || processing == '#') {
                value = (value < 255*255/2 ? value / 2 : value * 3/2 - 255*255/2);
            }

            // opacity (gamma-compressed, 0 .. 255)
            uint32_t alpha = (value == 0 ? 0 : value > 64770 ? 255 : round(sqrt(value)));

            *pixel++ = (alpha << 24) | 0xffffffU;
        }
    }

    free(values);
    return blur; // (used by the optimizer)
}


static void optimizeTiles() {
    for (int row = 0; row < TILE_ROWS; row++) {
        for (int column = 0; column < TILE_COLS; column++) {
            if (tileEmpty[row][column]) continue;
            char processing = TileProcessing[row][column];

            // show what we are doing
            char title[100];
            sprintf(title, "Brogue - Optimizing tile %d / %d ...\n", row * TILE_COLS + column + 1, TILE_ROWS * TILE_COLS);
            SDL_SetWindowTitle(Win, title);
            SDL_Surface *winSurface = SDL_GetWindowSurface(Win);
            if (!winSurface) sdlfatal(__FILE__, __LINE__);
            if (SDL_BlitSurface(TilesPNG, &(SDL_Rect){.x=column*TILE_WIDTH, .y=row*TILE_HEIGHT, .w=TILE_WIDTH, .h=TILE_HEIGHT},
                    winSurface, &(SDL_Rect){.x=0, .y=0, .w=TILE_WIDTH, .h=TILE_HEIGHT}) < 0) sdlfatal(__FILE__, __LINE__);
            if (SDL_UpdateWindowSurface(Win) < 0) sdlfatal(__FILE__, __LINE__);

            // horizontal shifts
            baseTileHeight = MAX_TILE_SIZE;
            for (baseTileWidth = 5; baseTileWidth <= MAX_TILE_SIZE; baseTileWidth++) {
                int8_t *shifts = tileShifts[row][column][0][baseTileWidth - 1];
                SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, baseTileWidth * TILE_COLS, baseTileHeight * TILE_ROWS, 32, SDL_PIXELFORMAT_ARGB8888);
                if (!surface) sdlfatal(__FILE__, __LINE__);

                for (int i = 0; i < 3; i++) {
                    for (int idx = 0; idx < (processing == 't' || processing == '#' ? 2 : 3); idx++) {
                        double bestResult = 1e20;
                        int8_t bestShift = 0;
                        int8_t midShift = (idx == 2 ? (shifts[0] + shifts[1]) / 2 : 0);
                        for (int8_t shift = midShift - 5; shift <= midShift + 5; shift++) {
                            shifts[idx] = shift;
                            if (processing == 't' || processing == '#') {
                                shifts[2] = (shifts[0] + shifts[1]) / 2;
                            }
                            double blur = prepareTile(surface, baseTileWidth, baseTileHeight, row, column, true);
                            if (blur < bestResult) {
                                bestResult = blur;
                                bestShift = shift;
                            }
                        }
                        shifts[idx] = bestShift;
                        if (processing == 't' || processing == '#') {
                            shifts[2] = (shifts[0] + shifts[1]) / 2;
                        }
                    }
                }

                SDL_FreeSurface(surface);
            }

            // vertical shifts
            baseTileWidth = MAX_TILE_SIZE;
            for (baseTileHeight = 7; baseTileHeight <= MAX_TILE_SIZE; baseTileHeight++) {
                int8_t *shifts = tileShifts[row][column][1][baseTileHeight - 1];
                SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, baseTileWidth * TILE_COLS, baseTileHeight * TILE_ROWS, 32, SDL_PIXELFORMAT_ARGB8888);
                if (!surface) sdlfatal(__FILE__, __LINE__);

                for (int i = 0; i < 3; i++) {
                    for (int idx = 0; idx < (processing == 't' ? 1 : 3); idx++) {
                        double bestResult = 1e20;
                        int8_t bestShift = 0;
                        int8_t midShift = (idx == 2 ? (shifts[0] + shifts[1]) / 2 : 0);
                        for (int8_t shift = midShift - 5; shift <= midShift + 5; shift++) {
                            shifts[idx] = shift;
                            double blur = prepareTile(surface, baseTileWidth, baseTileHeight, row, column, true);
                            if (blur < bestResult) {
                                bestResult = blur;
                                bestShift = shift;
                            }
                        }
                        shifts[idx] = bestShift;
                    }
                }

                SDL_FreeSurface(surface);
            }
        }
    }
    SDL_SetWindowTitle(Win, "Brogue");
}


static void init() {

    // load the large PNG
    char filename[BROGUE_FILENAME_MAX];
    sprintf(filename, "%s/assets/tiles.png", dataDirectory);
    SDL_Surface *image = IMG_Load(filename);
    if (!image) imgfatal(__FILE__, __LINE__);
    TilesPNG = SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!TilesPNG) sdlfatal(__FILE__, __LINE__);
    SDL_FreeSurface(image);

    // measure padding
    for (int row = 0; row < TILE_ROWS; row++) {
        for (int column = 0; column < TILE_COLS; column++) {
            tileEmpty[row][column] = isTileEmpty(row, column);
            tilePadding[row][column] = (TileProcessing[row][column] == 'f' ? getPadding(row, column) : 0);
        }
    }

    // load shifts
    sprintf(filename, "%s/assets/tiles.bin", dataDirectory);
    FILE *file = fopen(filename, "rb");
    if (file) {
        fread(tileShifts, 1, sizeof(tileShifts), file);
        fclose(file);
    } else {
        optimizeTiles();
        file = fopen(filename, "wb");
        fwrite(tileShifts, 1, sizeof(tileShifts), file);
        fclose(file);
    }
}


static void loadTiles(SDL_Renderer *renderer, int outputWidth, int outputHeight) {
    if (TilesPNG == NULL) init();

    // choose tile size
    int newBaseTileWidth = outputWidth / COLS;
    int newBaseTileHeight = outputHeight / ROWS;
    double tileAspectRatio = (double)(outputWidth * ROWS) / (outputHeight * COLS);
    if (newBaseTileHeight >= MAX_TILE_SIZE) {
        newBaseTileHeight = TILE_HEIGHT;
        newBaseTileWidth = round(newBaseTileHeight * tileAspectRatio);
    }
    if (newBaseTileWidth >= MAX_TILE_SIZE) {
        newBaseTileWidth = TILE_WIDTH;
        newBaseTileHeight = round(newBaseTileWidth / tileAspectRatio);
    }
    if (newBaseTileWidth == 0) newBaseTileWidth = 1;
    if (newBaseTileHeight == 0) newBaseTileHeight = 1;

    // if tile size has not changed, we don't need to rebuild the tiles
    if (baseTileWidth == newBaseTileWidth && baseTileHeight == newBaseTileHeight) {
        return;
    }

    baseTileWidth = newBaseTileWidth;
    baseTileHeight = newBaseTileHeight;

    // destroy the old textures
    for (int i = 0; i < 4; i++) {
        if (Textures[i]) SDL_DestroyTexture(Textures[i]);
        Textures[i] = NULL;
    }

    // choose the number of textures
    if (baseTileWidth >= MAX_TILE_SIZE || baseTileHeight >= MAX_TILE_SIZE) {
        numTextures = 1;
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    } else {
        numTextures = 4;
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }

    // The original image will be resized to 4 possible sizes:
    //  -  Textures[0]: tiles are  N    x  M    pixels
    //  -  Textures[1]: tiles are (N+1) x  M    pixels
    //  -  Textures[2]: tiles are  N    x (M+1) pixels
    //  -  Textures[3]: tiles are (N+1) x (M+1) pixels

    for (int i = 0; i < numTextures; i++) {

        // choose dimensions
        int tileWidth = baseTileWidth + (i == 1 || i == 3 ? 1 : 0);
        int tileHeight = baseTileHeight + (i == 2 || i == 3 ? 1 : 0);
        int surfaceWidth = 1, surfaceHeight = 1;
        while (surfaceWidth < tileWidth * TILE_COLS) surfaceWidth *= 2;
        while (surfaceHeight < tileHeight * TILE_ROWS) surfaceHeight *= 2;

        // downscale the tiles
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, surfaceWidth, surfaceHeight, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!surface) sdlfatal(__FILE__, __LINE__);
        for (int row = 0; row < TILE_ROWS; row++) {
            for (int column = 0; column < TILE_COLS; column++) {
                prepareTile(surface, tileWidth, tileHeight, row, column, false);
            }
        }

        // convert to texture
        Textures[i] = SDL_CreateTextureFromSurface(renderer, surface);
        if (!Textures[i]) sdlfatal(__FILE__, __LINE__);
        if (SDL_SetTextureBlendMode(Textures[i], SDL_BLENDMODE_BLEND) < 0) sdlfatal(__FILE__, __LINE__);
        SDL_FreeSurface(surface);
    }
}


void updateTile(int row, int column, short charIndex,
    short foreRed, short foreGreen, short foreBlue,
    short backRed, short backGreen, short backBlue)
{
    screenTiles[row][column] = (ScreenTile){
        .charIndex = charIndex,
        .foreRed = foreRed,
        .foreGreen = foreGreen,
        .foreBlue = foreBlue,
        .backRed = backRed,
        .backGreen = backGreen,
        .backBlue = backBlue
    };
}


void updateScreen() {
    if (!Win) return;

    SDL_Renderer *renderer = SDL_GetRenderer(Win);
    if (!renderer) {
        renderer = SDL_CreateRenderer(Win, -1, 0);
        if (!renderer) sdlfatal(__FILE__, __LINE__);
    }

    int outputWidth, outputHeight;
    if (SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight) < 0) sdlfatal(__FILE__, __LINE__);
    if (outputWidth == 0 || outputHeight == 0) return;

    loadTiles(renderer, outputWidth, outputHeight);

    if (SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) < 0) sdlfatal(__FILE__, __LINE__);
    if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0) < 0) sdlfatal(__FILE__, __LINE__);
    if (SDL_RenderClear(renderer) < 0) sdlfatal(__FILE__, __LINE__);

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            SDL_Rect dest;
            ScreenTile *tile = &screenTiles[y][x];
            int tileRow = tile->charIndex / 16;
            int tileColumn = tile->charIndex % 16;
            int tileWidth = ((x+1) * outputWidth / COLS) - (x * outputWidth / COLS);
            int tileHeight = ((y+1) * outputHeight / ROWS) - (y * outputHeight / ROWS);
            if (tileWidth == 0 || tileHeight == 0) continue;

            dest.x = x * outputWidth / COLS;
            dest.y = y * outputHeight / ROWS;
            dest.w = tileWidth;
            dest.h = tileHeight;

            // paint the background
            if (tile->backRed || tile->backGreen || tile->backBlue) {
                if (SDL_SetRenderDrawColor(renderer,
                    round(2.55 * tile->backRed),
                    round(2.55 * tile->backGreen),
                    round(2.55 * tile->backBlue), 255) < 0) sdlfatal(__FILE__, __LINE__);
                if (SDL_RenderFillRect(renderer, &dest) < 0) sdlfatal(__FILE__, __LINE__);
            }

            // blend the foreground
            if (!tileEmpty[tileRow][tileColumn]
                    || tileRow == 21 && tileColumn == 1  // wall top (procedural)
                    || tileRow == 20 && tileColumn == 2) // floor (possibly procedural)
            {
                SDL_Rect src;
                SDL_Texture *texture;

                if (numTextures == 4) {
                    // use the appropriate downscaled texture, which the renderer can copy 1:1
                    texture = Textures[(tileWidth > baseTileWidth ? 1 : 0) + (tileHeight > baseTileHeight ? 2 : 0)];
                    src.x = tileColumn * tileWidth;
                    src.y = tileRow * tileHeight;
                    src.w = tileWidth;
                    src.h = tileHeight;
                } else {
                    // use a single texture, let the renderer do the interpolation
                    texture = Textures[0];
                    src.x = tileColumn * baseTileWidth;
                    src.y = tileRow * baseTileHeight;
                    src.w = baseTileWidth;
                    src.h = baseTileHeight;
                }

                if (SDL_SetTextureColorMod(texture,
                    round(2.55 * tile->foreRed),
                    round(2.55 * tile->foreGreen),
                    round(2.55 * tile->foreBlue)) < 0) sdlfatal(__FILE__, __LINE__);
                if (SDL_RenderCopy(renderer, texture, &src, &dest) < 0) sdlfatal(__FILE__, __LINE__);
            }
        }
    }

    SDL_RenderPresent(renderer);
}


/*
Creates or resizes the game window with the currently loaded font.
*/
void resizeWindow(int width, int height) {

    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) < 0) sdlfatal(__FILE__, __LINE__);

    // 70% of monitor size by default
    if (width < 0) width = mode.w * 7/10;
    if (height < 0) height = mode.h * 7/10;

    // go to fullscreen mode if the window is as big as the screen
    if (width >= mode.w && height >= mode.h) fullScreen = true;

    if (Win == NULL) {
        // create the window
        Win = SDL_CreateWindow("Brogue",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!Win) sdlfatal(__FILE__, __LINE__);

        // set its icon
        char filename[BROGUE_FILENAME_MAX];
        sprintf(filename, "%s/assets/icon.png", dataDirectory);
        SDL_Surface *icon = IMG_Load(filename);
        if (!icon) imgfatal(__FILE__, __LINE__);
        SDL_SetWindowIcon(Win, icon);
        SDL_FreeSurface(icon);
    }

    if (fullScreen) {
        if (!(SDL_GetWindowFlags(Win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
            // switch to fullscreen mode
            if (SDL_SetWindowFullscreen(Win, SDL_WINDOW_FULLSCREEN_DESKTOP) < 0) sdlfatal(__FILE__, __LINE__);
        }
    } else {
        if (SDL_GetWindowFlags(Win) & SDL_WINDOW_FULLSCREEN_DESKTOP) {
            // switch to windowed mode
            if (SDL_SetWindowFullscreen(Win, 0) < 0) sdlfatal(__FILE__, __LINE__);
        } else {
            // what is the current size?
            SDL_GetWindowSize(Win, &windowWidth, &windowHeight);
            if (windowWidth != width || windowHeight != height) {
                // resize the window
                SDL_SetWindowSize(Win, width, height);
                SDL_RestoreWindow(Win);
            }
        }
    }

    SDL_GetWindowSize(Win, &windowWidth, &windowHeight);
    refreshScreen();
    updateScreen();
}


SDL_Surface *captureScreen() {
    if (!Win) return NULL;

    // get the renderer
    SDL_Renderer *renderer = SDL_GetRenderer(Win);
    if (!renderer) return NULL;

    // get its size
    int outputWidth = 0;
    int outputHeight = 0;
    if (SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight) < 0) sdlfatal(__FILE__, __LINE__);
    if (outputWidth == 0 || outputHeight == 0) return NULL;

    // take a screenshot
    SDL_Surface *screenshot = SDL_CreateRGBSurfaceWithFormat(0, outputWidth, outputHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!screenshot) sdlfatal(__FILE__, __LINE__);
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, screenshot->pixels, outputWidth * 4) < 0) sdlfatal(__FILE__, __LINE__);
    return screenshot;
}
