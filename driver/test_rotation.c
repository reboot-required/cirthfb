/*
 * Rotation unit test for cirthfb_flush.
 *
 * cirthfb registers a landscape framebuffer (XRES=250 wide, YRES=122 tall)
 * and rotates it 90° CW before sending pixels to the SSD1675 controller.
 *
 * Transform:  FB pixel (x, y)  →  EPD col = y,  row = XRES − 1 − x
 *
 * Source layout (landscape, 1 bpp MSB-first):
 *   stride = (XRES + 7) / 8 = 32 bytes/row
 *   size   = 32 × 122 = 3904 bytes
 *
 * Destination layout (portrait, 1 bpp MSB-first):
 *   stride = (YRES + 7) / 8 = 16 bytes/row
 *   size   = 16 × 250 = 4000 bytes
 *   access: epd[row * 16 + col/8]
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define XRES        250
#define YRES        122
#define SRC_STRIDE  ((XRES + 7) / 8)         /* 32 bytes/row — landscape source */
#define SRC_SIZE    (SRC_STRIDE * YRES)       /* 3904 bytes */
#define EPD_STRIDE  ((YRES + 7) / 8)         /* 16 bytes/row — portrait dest */
#define EPD_SIZE    (EPD_STRIDE * XRES)       /* 4000 bytes */

static int src_get(const uint8_t *fb, int x, int y)
{
    return (fb[y * SRC_STRIDE + x / 8] >> (7 - x % 8)) & 1;
}

static void epd_set(uint8_t *epd, int col, int row, int v)
{
    int byte = row * EPD_STRIDE + col / 8;
    int bit  = 7 - col % 8;
    if (v) epd[byte] |=  (uint8_t)(1u << bit);
    else   epd[byte] &= ~(uint8_t)(1u << bit);
}

static int epd_get(const uint8_t *epd, int col, int row)
{
    return (epd[row * EPD_STRIDE + col / 8] >> (7 - col % 8)) & 1;
}

/* Rotate landscape src 90° CW into portrait epd (1 = white, 0 = black). */
static void rotate_90cw(const uint8_t *src, uint8_t *epd)
{
    memset(epd, 0xFF, EPD_SIZE);
    for (int y = 0; y < YRES; y++) {
        for (int x = 0; x < XRES; x++) {
            int pixel = src_get(src, x, y);
            int col   = y;
            int row   = XRES - 1 - x;
            epd_set(epd, col, row, pixel);
        }
    }
}

/* ── helpers ───────────────────────────────────────────────────────────── */

static uint8_t src[SRC_SIZE];
static uint8_t epd[EPD_SIZE];

static void set_src_pixel(int x, int y) /* set one black pixel in src */
{
    memset(src, 0xFF, SRC_SIZE);
    int byte = y * SRC_STRIDE + x / 8;
    int bit  = 7 - x % 8;
    src[byte] &= ~(uint8_t)(1u << bit);
}

static int check_epd_black(int col, int row) { return epd_get(epd, col, row) == 0; }
static int check_epd_white(int col, int row) { return epd_get(epd, col, row) == 1; }

/* ── tests ─────────────────────────────────────────────────────────────── */

static int pass, fail;

#include <stdarg.h>
static void check(int cond, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (cond) pass++; else fail++;
}
#define CHECK(cond, ...) check((cond), __VA_ARGS__)

static void test_all_white(void)
{
    memset(src, 0xFF, SRC_SIZE);
    rotate_90cw(src, epd);
    CHECK(memcmp(epd, src, SRC_SIZE) == 0, "all-white → all-white");
}

/* FB corner pixels and their expected EPD positions:
 *   FB(0,0)           → EPD col=0,        row=XRES-1=249
 *   FB(XRES-1, 0)     → EPD col=0,        row=0
 *   FB(0, YRES-1)     → EPD col=YRES-1=121, row=XRES-1=249
 *   FB(XRES-1, YRES-1)→ EPD col=YRES-1=121, row=0
 */
static void test_corners(void)
{
    struct { int fx, fy, ec, er; const char *name; } cases[] = {
        {      0,      0,         0, XRES-1, "FB(0,0) → EPD(0,249)"             },
        { XRES-1,      0,         0,      0, "FB(249,0) → EPD(0,0)"             },
        {      0, YRES-1,    YRES-1, XRES-1, "FB(0,121) → EPD(121,249)"         },
        { XRES-1, YRES-1,    YRES-1,      0, "FB(249,121) → EPD(121,0)"         },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        set_src_pixel(cases[i].fx, cases[i].fy);
        rotate_90cw(src, epd);
        CHECK(check_epd_black(cases[i].ec, cases[i].er),
              "%s: target pixel is black", cases[i].name);
        /* every other corner must stay white */
        for (size_t j = 0; j < sizeof(cases)/sizeof(cases[0]); j++) {
            if (j == i) continue;
            CHECK(check_epd_white(cases[j].ec, cases[j].er),
                  "%s: corner (%d,%d) stays white",
                  cases[i].name, cases[j].ec, cases[j].er);
        }
    }
}

static void test_single_pixel_count(void)
{
    set_src_pixel(100, 60);
    rotate_90cw(src, epd);

    int black = 0;
    for (int row = 0; row < XRES; row++)
        for (int col = 0; col < YRES; col++)
            if (!epd_get(epd, col, row)) black++;

    CHECK(black == 1, "single input pixel → exactly 1 black output pixel (got %d)", black);
}

static void test_column_maps_to_epd_row(void)
{
    /* A full black column at x=0 in the source should become
     * a full black EPD row at row=XRES-1=249.  */
    memset(src, 0xFF, SRC_SIZE);
    for (int y = 0; y < YRES; y++) {
        src[y * SRC_STRIDE] &= 0x7F; /* clear bit 7 of byte 0 = pixel x=0 */
    }
    rotate_90cw(src, epd);

    int all_black = 1;
    for (int col = 0; col < YRES; col++) {
        if (epd_get(epd, col, XRES-1) != 0) { all_black = 0; break; }
    }
    CHECK(all_black, "source column x=0 → EPD row 249 is all black");
}

static void test_row_maps_to_epd_column(void)
{
    /* A full black row at y=0 in the source should become
     * a full black EPD column at col=0. */
    memset(src, 0xFF, SRC_SIZE);
    for (int x = 0; x < XRES; x++) {
        int byte = x / 8;
        int bit  = 7 - x % 8;
        src[byte] &= ~(uint8_t)(1u << bit);
    }
    rotate_90cw(src, epd);

    int all_black = 1;
    for (int row = 0; row < XRES; row++) {
        if (epd_get(epd, 0, row) != 0) { all_black = 0; break; }
    }
    CHECK(all_black, "source row y=0 → EPD column 0 is all black");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    test_all_white();
    test_corners();
    test_single_pixel_count();
    test_column_maps_to_epd_row();
    test_row_maps_to_epd_column();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
