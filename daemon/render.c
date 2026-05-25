#include "render.h"
#include "font_5x8.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Device layer
 * ---------------------------------------------------------------------- */

static int      fb_fd  = -1;
static uint8_t *fb_buf = MAP_FAILED;

int render_open(const char *dev)
{
    fb_fd = open(dev, O_RDWR);
    if (fb_fd < 0) {
        perror("render_open: open");
        return -1;
    }

    fb_buf = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_buf == MAP_FAILED) {
        perror("render_open: mmap");
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    return 0;
}

void render_close(void)
{
    if (fb_buf != MAP_FAILED) {
        munmap(fb_buf, FB_SIZE);
        fb_buf = MAP_FAILED;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}

/* White background: 0xFF = all pixels off (white for e-ink) */
void render_clear(void)
{
    if (fb_buf != MAP_FAILED)
        memset(fb_buf, 0xFF, FB_SIZE);
}

void render_flush(void)
{
    if (fb_fd >= 0 && fb_buf != MAP_FAILED)
        pwrite(fb_fd, fb_buf, FB_SIZE, 0);
}

uint8_t *render_fb(void)
{
    return (fb_buf != MAP_FAILED) ? fb_buf : NULL;
}

/* -------------------------------------------------------------------------
 * Drawing layer — all operate on the caller-supplied buffer
 * ---------------------------------------------------------------------- */

/*
 * 1bpp packed, MSB-first: byte = y*(FB_WIDTH/8) + x/8, bit = 7-(x%8)
 * v=0 → black (bit clear), v=1 → white (bit set)
 */
void set_pixel(uint8_t *fb, int x, int y, int v)
{
    if (!fb || x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT)
        return;

    int byte_idx = y * FB_STRIDE + x / 8;
    int bit      = 7 - (x % 8);

    if (v)
        fb[byte_idx] |=  (uint8_t)(1u << bit);
    else
        fb[byte_idx] &= ~(uint8_t)(1u << bit);
}

void draw_char(uint8_t *fb, int x, int y, char c)
{
    if ((unsigned char)c < FONT_FIRST || (unsigned char)c > FONT_LAST)
        c = '?';

    const uint8_t *glyph = font5x8[(unsigned char)c - FONT_FIRST];

    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            int px = x + col;
            int py = y + row;
            if (px >= FB_WIDTH || py >= FB_HEIGHT)
                continue;
            int v = !((glyph[col] >> (7 - row)) & 1);
            set_pixel(fb, px, py, v);
        }
    }
}

void draw_string(uint8_t *fb, int x, int y, const char *s)
{
    while (*s && x + FONT_W <= FB_WIDTH) {
        draw_char(fb, x, y, *s++);
        x += FONT_STRIDE;
    }
}

/* -------------------------------------------------------------------------
 * Standalone render test — produces a PBM to stdout
 * gcc -DRENDER_TEST render.c -o render_test -I. && ./render_test > font_test.pbm
 * ---------------------------------------------------------------------- */
#ifdef RENDER_TEST
int main(void)
{
    uint8_t buf[FB_SIZE];
    memset(buf, 0xFF, FB_SIZE);

    int x = 2, y = 2, count = 0;
    for (int c = FONT_FIRST; c <= FONT_LAST; c++) {
        draw_char(buf, x, y, (char)c);
        x += FONT_STRIDE;
        count++;
        if (count % 40 == 0) {
            x = 2;
            y += FONT_H + 2;
        }
    }

    printf("P4\n%d %d\n", FB_WIDTH, FB_HEIGHT);
    fwrite(buf, 1, FB_SIZE, stdout);
    return 0;
}
#endif /* RENDER_TEST */
