#include "vshot.h"

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORED_MAX 65535u

static uint32_t crc_table[256];

static uint32_t crc32(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int b = 0; b < 8; b++)
                k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint8_t *be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
    return p + 4;
}

static uint8_t *chunk(uint8_t *p, const char *type, uint32_t len)
{
    be32(p, len);
    memcpy(p + 4, type, 4);
    return be32(p + 8 + len, crc32(0, p + 4, len + 4));
}

int vshot_combo(uint32_t buttons)
{
    static uint32_t prev;
    int hit = (buttons & VSHOT_COMBO) == VSHOT_COMBO && (prev & VSHOT_COMBO) != VSHOT_COMBO;
    prev = buttons;
    return hit;
}

int vshot_take(const char *data_dir)
{
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0 || !fb.base || !fb.width)
        return -1;
    if (fb.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8)
        return -2;
    uint32_t w = fb.width, h = fb.height, row = 1 + 3 * w, raw = h * row;
    uint32_t blocks = (raw + STORED_MAX - 1) / STORED_MAX, zlen = 2 + 5 * blocks + raw + 4;
    size_t size = 8 + (12 + 13) + (12 + zlen) + 12;
    uint8_t *img = malloc(raw), *png = malloc(size);
    if (!img || !png) {
        free(img);
        free(png);
        return -3;
    }

    sceDisplayWaitVblankStart();
    const uint8_t *src = fb.base;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + 4 * (size_t)fb.pitch * y;
        uint8_t *d = img + (size_t)row * y;
        *d++ = 0;
        for (uint32_t x = 0; x < w; x++, s += 4, d += 3)
            d[0] = s[0], d[1] = s[1], d[2] = s[2];
    }

    uint8_t *p = png;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    p += 8;
    uint8_t *ihdr = p + 8;
    be32(ihdr, w), be32(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = chunk(p, "IHDR", 13);
    uint8_t *z = p + 8, *q = z;
    *q++ = 0x78, *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (uint32_t off = 0; off < raw; off += STORED_MAX) {
        uint32_t n = raw - off < STORED_MAX ? raw - off : STORED_MAX;
        *q++ = off + n == raw;
        q[0] = n, q[1] = n >> 8, q[2] = ~n, q[3] = ~n >> 8;
        memcpy(q + 4, img + off, n);
        for (uint32_t i = 0; i < n; i++) {
            a = (a + img[off + i]) % 65521;
            b = (b + a) % 65521;
        }
        q += 4 + n;
    }
    be32(q, b << 16 | a);
    p = chunk(p, "IDAT", zlen);
    p = chunk(p, "IEND", 0);
    free(img);

    char path[256];
    static int next = -1;
    snprintf(path, sizeof(path), "%s/shots", data_dir);
    sceIoMkdir(path, 0777);
    if (next < 0) {
        SceIoStat st;
        for (next = 0; next < 999; next++) {
            snprintf(path, sizeof(path), "%s/shots/shot-%03d.png", data_dir, next);
            if (sceIoGetstat(path, &st) < 0)
                break;
        }
    }
    int n = next++;
    snprintf(path, sizeof(path), "%s/shots/shot-%03d.png", data_dir, n);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int ok = fd >= 0 && sceIoWrite(fd, png, p - png) == p - png;
    if (fd >= 0)
        sceIoClose(fd);
    free(png);
    return ok ? n : -4;
}
