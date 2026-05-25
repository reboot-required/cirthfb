#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdint.h>

#ifndef FB_WIDTH
#define FB_WIDTH  250
#endif
#ifndef FB_HEIGHT
#define FB_HEIGHT 122
#endif
#define FB_STRIDE ((FB_WIDTH + 7) / 8)
#define FB_SIZE   (FB_STRIDE * FB_HEIGHT)

/* Device layer — manage the mmap'd framebuffer */
int       render_open(const char *dev);
void      render_close(void);
void      render_clear(void);
void      render_flush(void);
uint8_t  *render_fb(void);

/* Drawing layer — operate on an arbitrary 1bpp buffer */
void set_pixel(uint8_t *fb, int x, int y, int v);
void draw_char(uint8_t *fb, int x, int y, char c);
void draw_string(uint8_t *fb, int x, int y, const char *s);

#endif /* RENDER_H */
