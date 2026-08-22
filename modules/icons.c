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

#define ICON_SIZE 100
#define ICON_IMG   72
#define ICON_GAP   14
#define ICON_DRAG_THRESHOLD 5
#define ICON_DOUBLE_CLICK_MS 400

typedef struct {
    int      w, h;
    uint32_t *px;
} IconImg;

typedef struct {
    xcb_window_t iw;
    IconImg     *img;
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

static void icon_apply_shape(xcb_window_t iw, const uint32_t *scaled, const LauncherIcon *ic) {
    find_argb_visual();
    if (!argb_visual) return;

    xcb_pixmap_t pm = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 32, pm, root, ICON_SIZE, ICON_SIZE);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, pm, 0, NULL);
    uint32_t zero = 0;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &zero);
    xcb_rectangle_t full = { 0, 0, ICON_SIZE, ICON_SIZE };
    xcb_poly_fill_rectangle(conn, pm, gc, 1, &full);
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc,
                  ICON_IMG, ICON_IMG, ICON_GAP, 6, 0, 32,
                  sizeof(uint32_t) * (size_t)ICON_IMG * ICON_IMG,
                  (const uint8_t *)scaled);
    xcb_free_gc(conn, gc);
    xcb_flush(conn);

    if (ic && ic->name && ic->name[0]) {
        XftDraw *draw = XftDrawCreate(dpy, pm, argb_visual, argb_cmap);
        if (draw) {
            XftFont *f = open_font(cfg->fonts);
            if (f) {
                XftColor color;
                xcolor_to_xftcolor(cols.foreground, &color);
                XGlyphInfo ext;
                XftTextExtentsUtf8(dpy, f, (const FcChar8 *)ic->name,
                                   (int)strlen(ic->name), &ext);
                int nx = (ICON_SIZE - ext.xOff) / 2;
                if (nx < 0) nx = 0;
                int ny = ICON_SIZE - 8 + (f->ascent - f->descent) / 2;
                XftDrawStringUtf8(draw, &color, f, nx, ny,
                                  (const FcChar8 *)ic->name, (int)strlen(ic->name));
                XftColorFree(dpy, argb_visual, argb_cmap, &color);
                XftFontClose(dpy, f);
            }
            XftDrawDestroy(draw);
        }
    }
    XFlush(dpy);

    xcb_get_image_cookie_t ck = xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm,
                                              0, 0, ICON_SIZE, ICON_SIZE, ~0u);
    xcb_get_image_reply_t *rep = xcb_get_image_reply(conn, ck, NULL);
    if (!rep) {
        xcb_free_pixmap(conn, pm);
        return;
    }

    const uint8_t *data = xcb_get_image_data(rep);
    int stride = ICON_SIZE * 4;
    int lsb = xcb_get_setup(conn)->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;

    xcb_rectangle_t *rects = malloc(sizeof(xcb_rectangle_t) *
                                    ((size_t)ICON_SIZE * ICON_SIZE / 2 + ICON_SIZE));
    int nr = 0;
    for (int y = 0; y < ICON_SIZE; y++) {
        for (int x = 0; x < ICON_SIZE; ) {
            if (((icon_px(data, stride, x, y, lsb) >> 24) & 0xff) >= 1) {
                int x0 = x;
                while (x < ICON_SIZE &&
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
    xcb_create_pixmap(conn, 1, mask, iw, ICON_SIZE, ICON_SIZE);
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

static void icon_draw(xcb_window_t iw, const LauncherIcon *ic, const IconImg *img) {
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, iw, 0, NULL);

    uint32_t bg = cfg->ctxbg ? (uint32_t)hex_to_xcolor(cfg->ctxbg) : (uint32_t)cols.background;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &bg);
    xcb_rectangle_t r = { 0, 0, ICON_SIZE, ICON_SIZE };
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
                             0, 0, 0, 0, ICON_GAP, 6, ICON_IMG, ICON_IMG);
            XRenderFreePicture(dpy, imgpic);
            XFlush(dpy);
            xcb_free_pixmap(conn, pm);
            XRenderFreePicture(dpy, winpic);
        }
    }

    if (ic && ic->name && ic->name[0]) {
        XftDraw *draw = XftDrawCreate(dpy, iw, visual, cmap);
        XftFont *f = open_font(cfg->fonts);
        if (f) {
            XftColor color;
            xcolor_to_xftcolor(cols.foreground, &color);
            XGlyphInfo ext;
            XftTextExtentsUtf8(dpy, f, (const FcChar8 *)ic->name,
                               (int)strlen(ic->name), &ext);
            int nx = (ICON_SIZE - ext.xOff) / 2;
            if (nx < 0) nx = 0;
            int ny = ICON_SIZE - 8 + (f->ascent - f->descent) / 2;
            XftDrawStringUtf8(draw, &color, f, nx, ny,
                              (const FcChar8 *)ic->name, (int)strlen(ic->name));
            XftColorFree(dpy, visual, cmap, &color);
            XftFontClose(dpy, f);
        }
        XftDrawDestroy(draw);
    }

    if (!scaled)
        scaled = calloc((size_t)ICON_IMG * ICON_IMG, sizeof(uint32_t));
    icon_apply_shape(iw, scaled, ic);
    free(scaled);

    XFlush(dpy);
    xcb_flush(conn);
}

static int icon_screen_pos(const LauncherIcon *ic, int m, int *sx, int *sy) {
    if (n_mons <= 0) return 0;
    if (m < 0) m = 0;
    if (m >= n_mons) m = n_mons - 1;
    int mx = mons[m].x, my = mons[m].y, mw = mons[m].w, mh = mons[m].h;
    *sx = (int)(ic->x - canvas.pan_x[m]);
    *sy = (int)(ic->y - canvas.pan_y[m]);
    if (*sx + ICON_SIZE <= mx || *sx >= mx + mw ||
        *sy + ICON_SIZE <= my || *sy >= my + mh) {
        *sx = mx - ICON_SIZE - 8000;
        *sy = my;
    }
    return 1;
}

static void icon_window_create(int i, const LauncherIcon *ic) {
    int sx = 0, sy = 0;
    if (!icon_screen_pos(ic, ic->mon, &sx, &sy)) return;

    xcb_window_t iw = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BACKING_STORE |
                    XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t bg = cfg->ctxbg ? (uint32_t)hex_to_xcolor(cfg->ctxbg) : (uint32_t)cols.background;
    uint32_t em = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                  XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION;
    uint32_t values[] = { bg, XCB_BACKING_STORE_ALWAYS, 1, em };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, iw, root,
                      (int16_t)sx, (int16_t)sy, ICON_SIZE, ICON_SIZE, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    IconImg *img = img_load(ic->image);
    if (!img && ic->image && ic->image[0])
        fprintf(stderr, "sbpcwm: cannot load icon image '%s'\n", ic->image);
    icon_draw(iw, ic, img);

    slots[i].iw = iw;
    slots[i].img = img;
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
    if (conn) xcb_flush(conn);
}

void icons_reposition(void) {
    if (!slots || !cfg) return;
    for (int i = 0; i < n_slots; i++) {
        if (!slots[i].iw) continue;
        LauncherIcon *ic = &cfg->icons[i];
        int sx = 0, sy = 0;
        if (!icon_screen_pos(ic, ic->mon, &sx, &sy)) continue;
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
    icon_draw(slots[i].iw, &cfg->icons[i], slots[i].img);
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

    int m = mon_from_point(nsx + ICON_SIZE / 2, nsy + ICON_SIZE / 2);
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
    fprintf(f, "-- sbpcwm desktop icon state (managed by sbpcwm; edit config.lua for defaults)\n");
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
    config_load_icons(path, c);
}
