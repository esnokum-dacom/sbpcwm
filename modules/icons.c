#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
#include <unistd.h>
#include <png.h>
#include <jpeglib.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <xcb/xcb.h>
#include <xcb/shape.h>

#include "sbcct.h"
#include "sbpcwm.h"
#include "icons.h"

#define ICON_CELL_W  132
#define ICON_IMG      72
#define ICON_TOP       6
#define ICON_LAB_PAD   5
#define ICON_LAB_TOP   3
#define ICON_LAB_BOT   5
#define ICON_MAX_LINES 2
#define ICON_DRAG_THRESHOLD 5
#define ICON_DOUBLE_CLICK_MS 400

typedef struct {
    int      w, h;
    uint32_t *px;
} IconImg;

typedef struct {
    int off, len;
} LabelChunk;

typedef struct {
    int        nlines;
    int        ellipsis;
    int        w, h, lh;
    LabelChunk line[ICON_MAX_LINES];
} LabelLayout;

typedef struct {
    xcb_window_t iw;
    IconImg     *img;
    LabelLayout lab;
} IconSlot;

static IconSlot *slots  = NULL;
static int       n_slots = 0;
static int       icons_on = 1;

static int drag_idx = -1;
static int drag_moved = 0;
static int drag_sx0 = 0, drag_sy0 = 0;
static int drag_rx0 = 0, drag_ry0 = 0;
static int drag_mon = 0;

static IconImg *png_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int w = (int)png_get_image_width(png, info);
    int h = (int)png_get_image_height(png, info);
    int ct = png_get_color_type(png, info);
    int bt = png_get_bit_depth(png, info);

    if (bt == 16) png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bt < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    png_bytepp rows = malloc(sizeof(png_bytep) * (size_t)h);
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    size_t rw = png_get_rowbytes(png, info);
    for (int y = 0; y < h; y++) {
        rows[y] = malloc(rw);
        if (!rows[y]) {
            for (int k = 0; k < y; k++) free(rows[k]);
            free(rows);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return NULL;
        }
    }
    png_read_image(png, rows);

    IconImg *img = malloc(sizeof(IconImg));
    if (!img) {
        for (int y = 0; y < h; y++) free(rows[y]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->px = malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
    if (!img->px) {
        free(img);
        for (int y = 0; y < h; y++) free(rows[y]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        png_bytep row = rows[y];
        for (int x = 0; x < w; x++) {
            png_bytep p = row + (size_t)x * 4;
            img->px[y * w + x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                                 ((uint32_t)p[1] << 8)  |  (uint32_t)p[2];
        }
    }
    for (int y = 0; y < h; y++) free(rows[y]);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return img;
}

static void img_free(IconImg *img) {
    if (!img) return;
    free(img->px);
    free(img);
}

struct jpeg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jpeg_err_exit(j_common_ptr cinfo) {
    struct jpeg_err_mgr *e = (struct jpeg_err_mgr *)cinfo->err;
    longjmp(e->jb, 1);
}

static IconImg *jpeg_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    struct jpeg_err_mgr jerr;
    struct jpeg_decompress_struct cinfo;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;

    int inited = 0;
    if (setjmp(jerr.jb)) {
        if (inited) jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    inited = 1;
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    int ow = (int)cinfo.image_width, oh = (int)cinfo.image_height;
    int f = 1;
    while ((ow / f > 1024 || oh / f > 1024) && f < 16) f += 8;
    if (f > 1) {
        cinfo.scale_num = 1;
        cinfo.scale_denom = f;
    }
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    int comp = cinfo.output_components;

    IconImg *img = malloc(sizeof(IconImg));
    if (!img) { jpeg_destroy_decompress(&cinfo); fclose(fp); return NULL; }
    img->w = w;
    img->h = h;
    img->px = malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
    if (!img->px) {
        free(img);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

    JSAMPROW row = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                              (unsigned int)(w * comp), 1)[0];
    for (int y = 0; y < h; y++) {
        jpeg_read_scanlines(&cinfo, &row, 1);
        for (int x = 0; x < w; x++) {
            if (comp >= 3) {
                uint8_t r = row[x * 3 + 0], g = row[x * 3 + 1], b = row[x * 3 + 2];
                img->px[y * w + x] = 0xff000000u | ((uint32_t)r << 16) |
                                     ((uint32_t)g << 8) | (uint32_t)b;
            } else {
                uint8_t g = row[x];
                img->px[y * w + x] = 0xff000000u | ((uint32_t)g << 16) |
                                     ((uint32_t)g << 8) | (uint32_t)g;
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return img;
}

static IconImg *img_load(const char *path) {
    if (!path || !path[0]) return NULL;
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")))
        return jpeg_load(path);
    return png_load(path);
}

static uint32_t img_premul(uint32_t c) {
    unsigned a = c >> 24 & 0xff;
    unsigned r = ((c >> 16 & 0xff) * a + 127) / 255;
    unsigned g = ((c >> 8  & 0xff) * a + 127) / 255;
    unsigned b = ((c       & 0xff) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void img_scale(uint32_t *dst, int dw, int dh,
                      const uint32_t *src, int sw, int sh) {
    for (int y = 0; y < dh; y++) {
        double fy0 = (double)y * sh / dh;
        double fy1 = (double)(y + 1) * sh / dh;
        int sy0 = y * sh / dh;
        int sy1 = ((y + 1) * sh + dh - 1) / dh;
        if (sy1 > sh) sy1 = sh;
        for (int x = 0; x < dw; x++) {
            double fx0 = (double)x * sw / dw;
            double fx1 = (double)(x + 1) * sw / dw;
            int sx0 = x * sw / dw;
            int sx1 = ((x + 1) * sw + dw - 1) / dw;
            if (sx1 > sw) sx1 = sw;

            double fa = 0, fr = 0, fg = 0, fb = 0, wsum = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                double wy = MIN((double)(sy + 1), fy1) - MAX((double)sy, fy0);
                if (wy <= 0) continue;
                for (int sx = sx0; sx < sx1; sx++) {
                    double wx = MIN((double)(sx + 1), fx1) - MAX((double)sx, fx0);
                    if (wx <= 0) continue;
                    double w = wx * wy;
                    uint32_t c = img_premul(src[sy * sw + sx]);
                    fa += w * (c >> 24 & 0xff);
                    fr += w * (c >> 16 & 0xff);
                    fg += w * (c >> 8 & 0xff);
                    fb += w * (c & 0xff);
                    wsum += w;
                }
            }

            if (wsum <= 0) {
                dst[y * dw + x] = 0;
                continue;
            }
            unsigned a = (unsigned)(fa / wsum + 0.5);
            unsigned r = (unsigned)(fr / wsum + 0.5);
            unsigned g = (unsigned)(fg / wsum + 0.5);
            unsigned b = (unsigned)(fb / wsum + 0.5);
            dst[y * dw + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static Visual   *argb_visual = NULL;
static Colormap  argb_cmap   = 0;

int icons_draw_thumb(Drawable d, const char *path, int x, int y,
                     int w, int h, unsigned long bg) {
    if (!path || !*path || w <= 0 || h <= 0) return 0;
    IconImg *img = img_load(path);
    if (!img || img->w <= 0 || img->h <= 0) {
        if (img) img_free(img);
        return 0;
    }

    uint32_t *buf = calloc((size_t)w * h, sizeof(uint32_t));
    if (!buf) { img_free(img); return 0; }

    double scale = MIN((double)w / img->w, (double)h / img->h);
    if (scale > 1) scale = 1;
    int dw = (int)(img->w * scale), dh = (int)(img->h * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    img_scale(buf + (size_t)((h - dh) / 2) * w + (w - dw) / 2, dw, dh,
              img->px, img->w, img->h);
    img_free(img);

    unsigned long r0 = (bg >> 16) & 0xff, g0 = (bg >> 8) & 0xff, b0 = bg & 0xff;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = buf[i];
        unsigned a = c >> 24 & 0xff;
        if (a == 0xff) continue;
        unsigned r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
        if (a == 0) { buf[i] = (uint32_t)(0xff000000u | (r0 << 16) | (g0 << 8) | b0); continue; }
        r += (unsigned)(r0 * (255 - a) / 255);
        g += (unsigned)(g0 * (255 - a) / 255);
        b += (unsigned)(b0 * (255 - a) / 255);
        buf[i] = (uint32_t)(0xff000000u | (CLAMP((int)r, 0, 255) << 16) |
                            (CLAMP((int)g, 0, 255) << 8) | CLAMP((int)b, 0, 255));
    }

    XImage *xi = XCreateImage(dpy, visual, depth, ZPixmap, 0, (char *)buf,
                              (unsigned)w, (unsigned)h, 32, 0);
    if (!xi) { free(buf); return 0; }
    xi->byte_order = ImageByteOrder(dpy);
    GC gc = XCreateGC(dpy, d, 0, NULL);
    XPutImage(dpy, d, gc, xi, 0, 0, (int)x, (int)y, (unsigned)w, (unsigned)h);
    XFreeGC(dpy, gc);
    XDestroyImage(xi);
    return 1;
}

static void find_argb_visual(void) {
    if (argb_visual) return;
    xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(screen);
    for (; dit.rem; xcb_depth_next(&dit)) {
        xcb_depth_t *d = dit.data;
        if (d->depth != 32) continue;
        xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(d);
        for (; vit.rem; xcb_visualtype_next(&vit)) {
            xcb_visualtype_t *vt = vit.data;
            XVisualInfo vinfo = { .visualid = vt->visual_id };
            int n = 0;
            XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask, &vinfo, &n);
            if (!vi || n < 1) continue;
            Visual *v = vi[0].visual;
            XFree(vi);
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, v);
            if (fmt && fmt->type == PictTypeDirect && fmt->direct.alphaMask) {
                argb_visual = v;
                argb_cmap = XCreateColormap(dpy, root, v, AllocNone);
                return;
            }
        }
    }
}

static uint32_t icon_px(const uint8_t *data, int stride, int x, int y, int lsb) {
    const uint8_t *p = data + (size_t)y * stride + (size_t)x * 4;
    if (lsb)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int utf8_len(const char *s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

static int utf8_back(const char *s, int n) {
    int i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return n - i;
}

static int utf8_is_break(char c) {
    return c == ' ' || c == '-';
}

static int text_w(XftFont *f, const char *s, int len) {
    if (!f || len <= 0) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, len, &ext);
    return (int)ext.xOff;
}

static XftFont *icon_font = NULL;
static char     *icon_font_name = NULL;

static XftFont *label_font(void) {
    const char *want = cfg ? cfg->fonts : NULL;
    if (icon_font && want && icon_font_name && !strcmp(want, icon_font_name)) return icon_font;
    if (icon_font) { XftFontClose(dpy, icon_font); icon_font = NULL; }
    free(icon_font_name);
    icon_font_name = NULL;
    if (want) {
        icon_font = open_font(want);
        if (icon_font) icon_font_name = copystr(want);
    }
    return icon_font;
}

static void label_layout(LabelLayout *L, const char *name) {
    memset(L, 0, sizeof *L);
    L->w = ICON_CELL_W;

    XftFont *f = label_font();
    L->lh = f ? f->ascent + f->descent : 16;
    if (L->lh < 1) L->lh = 1;

    int n = name ? (int)strlen(name) : 0;
    L->nlines = 1;
    L->line[0].off = 0;
    L->line[0].len = n;

    if (f && n > 0) {
        int maxw = L->w - 2 * ICON_LAB_PAD;
        if (text_w(f, name, n) > maxw) {
            L->nlines = 0;
            int start = 0;
            while (start < n && L->nlines < ICON_MAX_LINES) {
                int end = start, brk = -1;
                while (end < n) {
                    int clen = utf8_len(name + end);
                    if (end + clen > n) clen = n - end;
                    if (end > start &&
                        text_w(f, name + start, end + clen - start) > maxw) break;
                    if (utf8_is_break(name[end])) brk = end;
                    end += clen;
                }
                if (end < n && brk > start) end = brk;
                int len = end - start;
                while (len > 0 && name[start + len - 1] == ' ') len--;
                L->line[L->nlines].off = start;
                L->line[L->nlines].len = len;
                L->nlines++;
                start = end;
                while (start < n && name[start] == ' ') start++;
            }
            if (start < n) L->ellipsis = 1;
        }
    }
    if (L->nlines < 1) L->nlines = 1;

    L->h = ICON_TOP + ICON_IMG + ICON_LAB_TOP + L->nlines * L->lh + ICON_LAB_BOT;
}

static void label_line_draw(XftDraw *draw, XftFont *f, const XftColor *color,
                            const char *s, int len, int ellipsis, int ellw,
                            int cell_w, int ybase) {
    char buf[256];
    const char *str = s;
    int slen = len;

    if (ellipsis) {
        int maxw = cell_w - 2 * ICON_LAB_PAD;
        int n = len;
        if (n > (int)sizeof buf - 4) n = (int)sizeof buf - 4;
        memcpy(buf, s, (size_t)n);
        buf[n] = 0;
        while (n > 0 && text_w(f, buf, n) + ellw > maxw) n -= utf8_back(buf, n);
        memcpy(buf + n, "...", 4);
        str = buf;
        slen = n + 3;
    }

    int x = (cell_w - text_w(f, str, slen)) / 2;
    if (x < 0) x = 0;
    XftDrawStringUtf8(draw, color, f, x, ybase, (const FcChar8 *)str, slen);
}

static void label_draw(Drawable d, Visual *vis, Colormap cm,
                       const char *name, const LabelLayout *L) {
    if (!name || !name[0] || L->nlines < 1) return;
    XftDraw *draw = XftDrawCreate(dpy, d, vis, cm);
    if (!draw) return;
    XftFont *f = label_font();
    if (f) {
        XftColor color;

        xcolor_to_xftcolor(cols.icon_text, &color);
        int ellw = text_w(f, "...", 3);
        int y0 = ICON_TOP + ICON_IMG + ICON_LAB_TOP + f->ascent;
        for (int i = 0; i < L->nlines; i++) {
            int last = (i == L->nlines - 1);
            label_line_draw(draw, f, &color, name + L->line[i].off, L->line[i].len,
                            last && L->ellipsis, ellw, L->w, y0 + i * L->lh);
        }
        XftColorFree(dpy, vis, cm, &color);
    }
    XftDrawDestroy(draw);
}

static void icon_apply_shape(xcb_window_t iw, const uint32_t *scaled,
                             const char *name, const LabelLayout *L) {
    find_argb_visual();
    if (!argb_visual) return;

    int iw_w = L->w, iw_h = L->h;
    int ix = (iw_w - ICON_IMG) / 2;
    if (ix < 0) ix = 0;

    xcb_pixmap_t pm = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 32, pm, root, (uint16_t)iw_w, (uint16_t)iw_h);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, pm, 0, NULL);
    uint32_t zero = 0;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &zero);
    xcb_rectangle_t full = { 0, 0, (uint16_t)iw_w, (uint16_t)iw_h };
    xcb_poly_fill_rectangle(conn, pm, gc, 1, &full);
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc,
                  ICON_IMG, ICON_IMG, (int16_t)ix, ICON_TOP, 0, 32,
                  sizeof(uint32_t) * (size_t)ICON_IMG * ICON_IMG,
                  (const uint8_t *)scaled);
    xcb_free_gc(conn, gc);
    xcb_flush(conn);

    label_draw((Drawable)pm, argb_visual, argb_cmap, name, L);
    XFlush(dpy);

    xcb_get_image_cookie_t ck = xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm,
                                              0, 0, (uint16_t)iw_w, (uint16_t)iw_h, ~0u);
    xcb_get_image_reply_t *rep = xcb_get_image_reply(conn, ck, NULL);
    if (!rep) {
        xcb_free_pixmap(conn, pm);
        return;
    }

    const uint8_t *data = xcb_get_image_data(rep);
    int stride = iw_w * 4;
    int lsb = xcb_get_setup(conn)->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;

    xcb_rectangle_t *rects = malloc(sizeof(xcb_rectangle_t) *
                                    ((size_t)iw_w * iw_h / 2 + iw_h));
    int nr = 0;
    for (int y = 0; y < iw_h; y++) {
        for (int x = 0; x < iw_w; ) {
            if (((icon_px(data, stride, x, y, lsb) >> 24) & 0xff) >= 1) {
                int x0 = x;
                while (x < iw_w &&
                       ((icon_px(data, stride, x, y, lsb) >> 24) & 0xff) >= 1)
                    x++;
                rects[nr].x = (int16_t)x0;
                rects[nr].y = (int16_t)y;
                rects[nr].width = (uint16_t)(x - x0);
                rects[nr].height = 1;
                nr++;
            } else {
                x++;
            }
        }
    }

    xcb_pixmap_t mask = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 1, mask, iw, (uint16_t)iw_w, (uint16_t)iw_h);
    xcb_gcontext_t mgc = xcb_generate_id(conn);
    xcb_create_gc(conn, mgc, mask, 0, NULL);
    xcb_change_gc(conn, mgc, XCB_GC_FOREGROUND, &zero);
    xcb_poly_fill_rectangle(conn, mask, mgc, 1, &full);
    if (nr > 0) {
        uint32_t one = 1;
        xcb_change_gc(conn, mgc, XCB_GC_FOREGROUND, &one);
        xcb_poly_fill_rectangle(conn, mask, mgc, nr, rects);
    }
    xcb_free_gc(conn, mgc);
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, iw, 0, 0, mask);
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, iw, 0, 0, mask);
    xcb_free_pixmap(conn, mask);

    free(rects);
    free(rep);
    xcb_free_pixmap(conn, pm);
    xcb_flush(conn);
}

static void icon_draw(xcb_window_t iw, const LauncherIcon *ic,
                      const IconImg *img, const LabelLayout *L) {
    int iw_w = L->w, iw_h = L->h;
    int ix = (iw_w - ICON_IMG) / 2;
    if (ix < 0) ix = 0;

    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, iw, 0, NULL);

    uint32_t bg = (uint32_t)cols.background;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &bg);
    xcb_rectangle_t r = { 0, 0, (uint16_t)iw_w, (uint16_t)iw_h };
    xcb_poly_fill_rectangle(conn, iw, gc, 1, &r);
    xcb_free_gc(conn, gc);

    uint32_t *scaled = NULL;
    if (img && img->w > 0 && img->h > 0) {
        XRenderPictFormat *winfmt = XRenderFindVisualFormat(dpy, visual);
        Picture winpic = XRenderCreatePicture(dpy, iw, winfmt, 0, NULL);
        if (winpic) {
            double scale = MIN((double)ICON_IMG / img->w, (double)ICON_IMG / img->h);
            if (scale > 1) scale = 1;
            int dw = (int)(img->w * scale), dh = (int)(img->h * scale);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            int ox = (ICON_IMG - dw) / 2, oy = (ICON_IMG - dh) / 2;

            scaled = malloc(sizeof(uint32_t) * (size_t)ICON_IMG * ICON_IMG);
            memset(scaled, 0, sizeof(uint32_t) * (size_t)ICON_IMG * ICON_IMG);
            img_scale(scaled + (size_t)oy * ICON_IMG + ox, dw, dh,
                      img->px, img->w, img->h);

            xcb_pixmap_t pm = xcb_generate_id(conn);
            xcb_create_pixmap(conn, 32, pm, root, ICON_IMG, ICON_IMG);
            xcb_gcontext_t igc = xcb_generate_id(conn);
            xcb_create_gc(conn, igc, pm, 0, NULL);
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, igc,
                          ICON_IMG, ICON_IMG, 0, 0, 0, 32,
                          sizeof(uint32_t) * (size_t)ICON_IMG * ICON_IMG,
                          (const uint8_t *)scaled);
            xcb_free_gc(conn, igc);
            xcb_flush(conn);

            XRenderPictFormat *argbfmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
            Picture imgpic = XRenderCreatePicture(dpy, pm, argbfmt, 0, NULL);
            XRenderComposite(dpy, PictOpOver, imgpic, None, winpic,
                             0, 0, 0, 0, ix, ICON_TOP, ICON_IMG, ICON_IMG);
            XRenderFreePicture(dpy, imgpic);
            XFlush(dpy);
            xcb_free_pixmap(conn, pm);
            XRenderFreePicture(dpy, winpic);
        }
    }

    label_draw((Drawable)iw, visual, cmap, ic ? ic->name : NULL, L);

    if (!scaled)
        scaled = calloc((size_t)ICON_IMG * ICON_IMG, sizeof(uint32_t));
    icon_apply_shape(iw, scaled, ic ? ic->name : NULL, L);
    free(scaled);

    XFlush(dpy);
    xcb_flush(conn);
}

static int icon_screen_pos(const LauncherIcon *ic, int m, int w, int h,
                           int *sx, int *sy) {
    if (n_mons <= 0) return 0;
    if (m < 0) m = 0;
    if (m >= n_mons) m = n_mons - 1;
    int mx = mons[m].x, my = mons[m].y, mw = mons[m].w, mh = mons[m].h;
    *sx = (int)(ic->x - canvas.pan_x[m]);
    *sy = (int)(ic->y - canvas.pan_y[m]);
    if (*sx + w <= mx || *sx >= mx + mw ||
        *sy + h <= my || *sy >= my + mh) {
        *sx = mx - w - 8000;
        *sy = my;
    }
    return 1;
}

static void icon_window_create(int i, const LauncherIcon *ic) {
    LabelLayout L;
    label_layout(&L, ic->name);

    int sx = 0, sy = 0;
    if (!icon_screen_pos(ic, ic->mon, L.w, L.h, &sx, &sy)) return;

    xcb_window_t iw = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BACKING_STORE |
                    XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t bg = (uint32_t)cols.background;
    uint32_t em = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                  XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION;
    uint32_t values[] = { bg, XCB_BACKING_STORE_ALWAYS, 1, em };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, iw, root,
                      (int16_t)sx, (int16_t)sy, (uint16_t)L.w, (uint16_t)L.h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    IconImg *img = img_load(ic->image);
    if (!img && ic->image && ic->image[0])
        fprintf(stderr, "sbpcwm: cannot load icon image '%s'\n", ic->image);
    icon_draw(iw, ic, img, &L);

    slots[i].iw = iw;
    slots[i].img = img;
    slots[i].lab = L;
    xcb_map_window(conn, iw);
    xcb_flush(conn);
}

void icons_rebuild(void) {
    icons_cleanup();
    if (!icons_on || !cfg || cfg->nicons <= 0 || n_mons <= 0) return;

    slots = calloc((size_t)cfg->nicons, sizeof(IconSlot));
    n_slots = cfg->nicons;
    for (int i = 0; i < cfg->nicons; i++) {
        LauncherIcon *ic = &cfg->icons[i];
        if (!ic->name || !ic->cmd || !ic->cmd[0]) continue;
        icon_window_create(i, ic);
    }
    icons_lower();
}

void icons_cleanup(void) {
    if (slots) {
        for (int i = 0; i < n_slots; i++) {
            if (slots[i].iw) xcb_destroy_window(conn, slots[i].iw);
            img_free(slots[i].img);
        }
        free(slots);
    }
    slots = NULL;
    n_slots = 0;
    drag_idx = -1;
    if (icon_font) { XftFontClose(dpy, icon_font); icon_font = NULL; }
    free(icon_font_name);
    icon_font_name = NULL;
    if (conn) xcb_flush(conn);
}

void icons_reposition(void) {
    if (!slots || !cfg) return;
    for (int i = 0; i < n_slots; i++) {
        if (!slots[i].iw) continue;
        LauncherIcon *ic = &cfg->icons[i];
        int sx = 0, sy = 0;
        if (!icon_screen_pos(ic, ic->mon, slots[i].lab.w, slots[i].lab.h, &sx, &sy))
            continue;
        uint32_t v[2] = { (uint32_t)sx, (uint32_t)sy };
        xcb_configure_window(conn, slots[i].iw, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    }
    xcb_flush(conn);
}

int icons_visible(void) { return icons_on; }

void icons_lower(void) {
    if (!slots) return;
    for (int i = 0; i < n_slots; i++) {
        if (!slots[i].iw) continue;
        uint32_t stack = XCB_STACK_MODE_BELOW;
        xcb_configure_window(conn, slots[i].iw, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    }
    if (conn) xcb_flush(conn);
}

int icon_window_is_icon(xcb_window_t w) {
    if (!slots) return 0;
    for (int i = 0; i < n_slots; i++)
        if (slots[i].iw == w) return 1;
    return 0;
}

void toggle_icons(const Arg arg) {
    (void)arg;
    icons_on = !icons_on;
    if (icons_on) icons_rebuild();
    else          icons_cleanup();
}

static int icon_index_for_win(xcb_window_t w) {
    if (!slots) return -1;
    for (int i = 0; i < n_slots; i++)
        if (slots[i].iw == w) return i;
    return -1;
}

int icons_redraw_win(xcb_window_t w) {
    int i = icon_index_for_win(w);
    if (i < 0 || !slots[i].iw || !cfg) return 0;
    icon_draw(slots[i].iw, &cfg->icons[i], slots[i].img, &slots[i].lab);
    return 1;
}

int icon_handle_press(xcb_button_press_event_t *e) {
    int i = icon_index_for_win(e->event);
    if (i < 0) return 0;

    ctx_close();
    if (conn) xcb_flush(conn);

    if (e->detail == XCB_BUTTON_INDEX_3) {
        ctx_open(e->root_x, e->root_y);
        return 1;
    }
    if (e->detail != XCB_BUTTON_INDEX_1) return 1;

    drag_idx = i;
    drag_moved = 0;
    win_size(slots[i].iw, &drag_sx0, &drag_sy0, NULL, NULL);
    drag_rx0 = e->root_x;
    drag_ry0 = e->root_y;
    drag_mon = cfg->icons[i].mon;
    return 1;
}

int icon_handle_motion(xcb_motion_notify_event_t *e) {
    if (drag_idx < 0) return 0;

    int dx = e->root_x - drag_rx0;
    int dy = e->root_y - drag_ry0;
    if (!drag_moved && (abs(dx) + abs(dy)) > ICON_DRAG_THRESHOLD)
        drag_moved = 1;
    if (!drag_moved) return 1;

    int nsx = drag_sx0 + dx;
    int nsy = drag_sy0 + dy;

    int m = mon_from_point(nsx + slots[drag_idx].lab.w / 2,
                           nsy + slots[drag_idx].lab.h / 2);
    if (m < 0 || m >= n_mons) m = drag_mon;

    uint32_t v[2] = { (uint32_t)nsx, (uint32_t)nsy };
    if (conn)
        xcb_configure_window(conn, slots[drag_idx].iw, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);

    LauncherIcon *ic = &cfg->icons[drag_idx];
    ic->mon = m;
    ic->x = nsx + (int)canvas.pan_x[m];
    ic->y = nsy + (int)canvas.pan_y[m];
    if (conn) xcb_flush(conn);
    return 1;
}

static xcb_timestamp_t dbl_time = 0;
static int dbl_idx = -1;

int icon_handle_release(xcb_button_release_event_t *e) {
    if (drag_idx < 0) return 0;
    int i = drag_idx;
    drag_idx = -1;
    if (e->detail != XCB_BUTTON_INDEX_1) return 1;

    if (drag_moved) {
        dbl_idx = -1;
        icons_save();
    } else if (i == dbl_idx && e->time - dbl_time < ICON_DOUBLE_CLICK_MS) {
        dbl_idx = -1;
        if (cfg->icons[i].cmd) {
            Arg a = { .com = (const char **)cfg->icons[i].cmd };
            run(a);
        }
    } else {
        dbl_idx = i;
        dbl_time = e->time;
    }
    return 1;
}

static void write_lua_str(FILE *f, const char *s) {
    if (!s) s = "";
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') fputc('\\', f);
        fputc(*p, f);
    }
}

void icons_save(void) {
    if (!cfg) return;
    const char *home = get_home();
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/sbcwm/icons.lua", home);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "sbpcwm: cannot write icon state %s\n", path);
        return;
    }
    fprintf(f, "-- sbcwm desktop icon state (managed by sbpcwm; edit config.lua for defaults)\n");
    fprintf(f, "icons = {\n");
    for (int i = 0; i < cfg->nicons; i++) {
        LauncherIcon *ic = &cfg->icons[i];
        fprintf(f, "  { name = \"");
        write_lua_str(f, ic->name);
        fprintf(f, "\", image = \"");
        write_lua_str(f, ic->image);
        fprintf(f, "\", x = %d, y = %d, mon = %d, cmd = { ",
                ic->x, ic->y, ic->mon);
        for (int j = 0; j < ic->ncmd; j++) {
            if (j) fprintf(f, ", ");
            fputc('"', f);
            write_lua_str(f, ic->cmd[j]);
            fputc('"', f);
        }
        fprintf(f, " } },\n");
    }
    fprintf(f, "}\n");
    fclose(f);
}

void icons_load_state(Config *c) {
    const char *home = get_home();
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/sbcwm/icons.lua", home);
    if (access(path, F_OK) != 0) return;
    config_apply_icon_state(path, c);
}
