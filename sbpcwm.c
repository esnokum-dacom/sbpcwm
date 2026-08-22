#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>
#include <xcb/xcb_aux.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <X11/extensions/Xinerama.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

#include "sbcct.h"
#include "sbpcwm.h"
#include "ctl.h"
#include "icons.h"

#define MOD Mod4Mask

const FcChar8 *close_sym = (FcChar8 *)"";
const FcChar8 *max_sym = (FcChar8 *)"󰝣";

static client *list = NULL;
static client *cur  = NULL;

static XftFont *title_font  = NULL;
static XftFont *button_font = NULL;

canvas_state canvas = { .pan_x = {0}, .pan_y = {0} };

static int sw, sh;

static int   pan_active   = 0;
static int   pan_start_x  = 0;
static int   pan_start_y  = 0;
static float pan_origin_x = 0;
static float pan_origin_y = 0;
static int   pan_mon      = 0;

static xcb_window_t fs_grab_win = XCB_NONE;

static xcb_window_t ctx_win = XCB_NONE;
static int ctx_x, ctx_y, ctx_w, ctx_h;
static int ctx_itemh;

static int running = 1;

static unsigned int numlock = 0;
char *client_get_title(xcb_window_t w);

Display          *dpy;
xcb_connection_t *conn;
int                scrno;
xcb_screen_t      *screen;
Visual            *visual;
Colormap           cmap;
int                depth;
static xcb_key_symbols_t *keysyms;

xcb_window_t root;

ColorScheme cols;
static xcb_window_t notify_win = XCB_NONE;
static long notify_until = 0;
static XftFont *notify_font = NULL;

static int randr_event_base = 0;

MonitorInfo mons[MAX_MONITORS];
int         n_mons = 0;
static xcb_atom_t canvas_atom_pan_x, canvas_atom_pan_y;
static xcb_atom_t sbcwm_atom_monitor;
static xcb_atom_t net_supported, net_wm_window_type, net_wm_window_type_dock,
                  net_wm_strut, net_wm_strut_partial, net_current_desktop,
                  net_supporting_wm_check, net_wm_name, net_wm_visible_name,
                  net_active_window, net_wm_state, net_wm_state_fullscreen,
                  ewmh_utf8_string, wm_delete_window,
                  wm_protocols, wm_normal_hints_atom, net_client_list_stacking;

static int strut[4] = {0, 0, 0, 0};

static xcb_window_t drag_subwindow = 0;
static uint16_t     drag_button    = 0;
static int16_t      drag_root_x    = 0;
static int16_t      drag_root_y    = 0;

#define MAX_DOCKS 32
static xcb_window_t docks[MAX_DOCKS];
static int          n_docks = 0;

Config *cfg;

static uint32_t wborder(void) { return cfg->border ? cfg->border_width : 0; }

#define TB_CONTENT_H (TITLEBAR_HEIGHT - 2 * wborder())

static long now_ms(void) {
    struct timespec t_s;
    clock_gettime(CLOCK_MONOTONIC, &t_s);
    return t_s.tv_sec * 1000L + t_s.tv_nsec / 1000000L;
}

char *copystr(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

const char *get_home(void) {
    const char *home = getenv("HOME");
    if (home)
        return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : NULL;
}

static xcb_atom_t get_atom(const char *name) {
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

void win_size(xcb_window_t w, int *x, int *y, unsigned int *wd, unsigned int *ht) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (wd) *wd = 0;
    if (ht) *ht = 0;
    if (!w) return;

    xcb_get_geometry_cookie_t ck = xcb_get_geometry(conn, w);
    xcb_get_geometry_reply_t *r = xcb_get_geometry_reply(conn, ck, NULL);
    if (!r) return;
    if (x)  *x  = r->x;
    if (y)  *y  = r->y;
    if (wd) *wd = r->width;
    if (ht) *ht = r->height;
    free(r);
}

void monitors_refresh(void) {
    xcb_randr_get_monitors_cookie_t ck = xcb_randr_get_monitors(conn, root, 1);
    xcb_randr_get_monitors_reply_t *r = xcb_randr_get_monitors_reply(conn, ck, NULL);
    n_mons = 0;
    if (r) {
        xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(r);
        for (; it.rem && n_mons < MAX_MONITORS; xcb_randr_monitor_info_next(&it)) {
            xcb_randr_monitor_info_t *m = it.data;
            mons[n_mons].x = m->x;
            mons[n_mons].y = m->y;
            mons[n_mons].w = m->width;
            mons[n_mons].h = m->height;
            n_mons++;
        }
        free(r);
    }
    if (n_mons == 0) {
        mons[0].x = 0; mons[0].y = 0; mons[0].w = sw; mons[0].h = sh;
        n_mons = 1;
    }
}

int mon_from_point(int px, int py) {
    for (int i = 0; i < n_mons; i++)
        if (px >= mons[i].x && px < mons[i].x + mons[i].w &&
            py >= mons[i].y && py < mons[i].y + mons[i].h)
            return i;
    return 0;
}

static void set_client_monitor(client *c, int mon) {
    c->mon = mon;
    uint32_t m = (uint32_t)mon;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, c->w, sbcwm_atom_monitor,
                         XCB_ATOM_CARDINAL, 32, 1, &m);
}

int mon_at_ptr(void) {
    xcb_query_pointer_cookie_t ck = xcb_query_pointer(conn, root);
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(conn, ck, NULL);
    int mon = 0;
    if (r) { mon = mon_from_point(r->root_x, r->root_y); free(r); }
    return mon;
}

int mon_at_win(xcb_window_t w) {
    int wx2, wy2; unsigned int ww2, wh2;
    win_size(w, &wx2, &wy2, &ww2, &wh2);
    return mon_from_point(wx2 + (int)ww2 / 2, wy2 + (int)wh2 / 2);
}

unsigned long hex_to_xcolor(const char *hex) {
    XColor color;
    XParseColor(dpy, cmap, hex, &color);
    XAllocColor(dpy, cmap, &color);
    return color.pixel;
}

void load_colors(void) {
    unsigned long fg = hex_to_xcolor("#ffffff");
    unsigned long bg = hex_to_xcolor("#151515"); cols.background = bg;
    cols.foreground = fg;
    for (int i = 0; i < 16; i++)
        cols.cs[i] = (i == 0) ? bg : fg;

    char path[256];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.cache/wal/colors", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[16];
    int i = 0;
    while (fgets(line, sizeof(line), f) && i < 16) {
        line[strcspn(line, "\n")] = 0;
        cols.cs[i++] = hex_to_xcolor(line);
    }
    fclose(f);

    cols.background = cols.cs[0];
    cols.foreground = cols.cs[15];
}

void xcolor_to_xftcolor(unsigned long pixel, XftColor *xft) {
    XColor xc = {0};
    xc.pixel = pixel;
    XQueryColor(dpy, cmap, &xc);
    XRenderColor rc = { .red = xc.red, .green = xc.green, .blue = xc.blue, .alpha = 0xffff };
    XftColorAllocValue(dpy, visual, cmap, &rc, xft);
}

XftFont *open_font(const char *name) {
    if (!name || !*name) name = "fixed";
    XftFont *f = XftFontOpenName(dpy, scrno, name);
    if (!f) {
        fprintf(stderr, "sbpcwm: cannot open font '%s', falling back to 'fixed'\n", name);
        f = XftFontOpenName(dpy, scrno, "fixed");
    }
    return f;
}

void fonts_reload(void) {
    if (title_font)  XftFontClose(dpy, title_font);
    title_font = open_font(cfg->fontb);
    if (button_font) XftFontClose(dpy, button_font);
    button_font = open_font(cfg->fontb);
    if (notify_font) XftFontClose(dpy, notify_font);
    notify_font = open_font(cfg->fonts);
}

static int dock_known(xcb_window_t w) {
    for (int i = 0; i < n_docks; i++)
        if (docks[i] == w) return 1;
    return 0;
}

static void dock_add(xcb_window_t w) {
    if (dock_known(w) || n_docks >= MAX_DOCKS) return;
    docks[n_docks++] = w;
}

static void dock_del(xcb_window_t w) {
    for (int i = 0; i < n_docks; i++) {
        if (docks[i] == w) {
            docks[i] = docks[--n_docks];
            return;
        }
    }
}

static void dock_track(xcb_window_t w) {
    if (icon_window_is_icon(w)) return;
    xcb_get_window_attributes_reply_t *wa =
        xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, w), NULL);
    if (wa && wa->override_redirect)
        dock_add(w);
    free(wa);
}

static void docks_raise(void) {
    for (int i = 0; i < n_docks; i++) {
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, docks[i], XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    }
    icons_lower();
}

xcb_window_t titlebar_create(client *c) {
    int x, y;
    unsigned int w, h;
    win_size(c->w, &x, &y, &w, &h);

    xcb_window_t titlebar = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        XCB_BACK_PIXMAP_NONE, 0,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, titlebar, root,
                       (int16_t)x, (int16_t)(y - TITLEBAR_HEIGHT), (uint16_t)w, TITLEBAR_HEIGHT, (uint16_t)wborder(),
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    xcb_map_window(conn, titlebar);
    return titlebar;
}

void titlebar_draw(client *c) {
    static ButtonTb btc;

    if (!c || !c->titlebar) return;

    unsigned int tw, th;
    int tx, ty;
    win_size(c->titlebar, &tx, &ty, &tw, &th);

    xcb_pixmap_t tpix = xcb_generate_id(conn);
    xcb_create_pixmap(conn, depth, tpix, root, (uint16_t)tw, (uint16_t)th);
    xcb_gcontext_t tgc = xcb_generate_id(conn);
    xcb_create_gc(conn, tgc, tpix, 0, NULL);

    XftColor color;
    xcolor_to_xftcolor(cols.foreground, &color);

    unsigned long bg;

    if (cfg->xr_colors) { bg = (c == cur) ? cols.cs[2] : 0x000000; } else { bg = (c == cur) ? cols.cs[2] : 0x000000; }

    uint32_t fgc = (uint32_t)bg;
    xcb_change_gc(conn, tgc, XCB_GC_FOREGROUND, &fgc);
    xcb_rectangle_t full = { 0, 0, (uint16_t)tw, (uint16_t)th };
    xcb_poly_fill_rectangle(conn, tpix, tgc, 1, &full);

    if (!button_font) button_font = open_font(cfg->fontb);
    if (!title_font)  title_font  = open_font(cfg->fontb);

    if (button_font) {
        XftDraw  *btn_draw = XftDrawCreate(dpy, tpix, visual, cmap);
        XftColor  btn_color;
        XRenderColor btn_xr = { .red = 65535, .green = 65535, .blue = 65535, .alpha = 65535 };
        XftColorAllocValue(dpy, visual, cmap, &btn_xr, &btn_color);

        btc.w = 22; btc.h = (int)th;
        btc.x = (int)tw - btc.w - 4;
        btc.y = 0;
        xcb_rectangle_t bclose = { (int16_t)btc.x, (int16_t)btc.y, (uint16_t)btc.w, (uint16_t)btc.h };
        xcb_poly_fill_rectangle(conn, tpix, tgc, 1, &bclose);
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, button_font, close_sym, (int)strlen((char *)close_sym), &ext);
        XftDrawStringUtf8(btn_draw, &btn_color, button_font,
            btc.x + (btc.w - ext.xOff) / 2,
            (int)((btc.h + button_font->ascent) / 2 - 2),
            close_sym, (int)strlen((char *)close_sym));

        int btn_w = 22;
        int btn_f = btc.x - btn_w - 2;
        xcb_rectangle_t bfs = { (int16_t)btn_f, 0, (uint16_t)btn_w, (uint16_t)th };
        xcb_poly_fill_rectangle(conn, tpix, tgc, 1, &bfs);
        XftTextExtentsUtf8(dpy, button_font, max_sym, (int)strlen((char *)max_sym), &ext);
        XftDrawStringUtf8(btn_draw, &btn_color, button_font,
            btn_f + (btn_w - ext.xOff) / 2,
            (int)((btc.h + button_font->ascent) / 2 - 2),
            max_sym, (int)strlen((char *)max_sym));

        XftColorFree(dpy, visual, cmap, &btn_color);
        XftDrawDestroy(btn_draw);
    }

    xcb_flush(conn);

    char buf[256];
    char *win_title = client_get_title(c->w);
    snprintf(buf, sizeof(buf), "%s", win_title ? win_title : "");
    free(win_title);

    XftDraw *draw = XftDrawCreate(dpy, tpix, visual, cmap);
    if (title_font) {
        XRenderColor xr = { .red = 65535, .green = 65535, .blue = 65535, .alpha = 65535 };
        XftColor tcolor;
        XftColorAllocValue(dpy, visual, cmap, &xr, &tcolor);
        XftDrawStringUtf8(draw, &tcolor, title_font, 10, (int)(th / 2) + (title_font->ascent / 2),
            (FcChar8 *)buf, (int)strlen(buf));
        XftColorFree(dpy, visual, cmap, &tcolor);
    }
    XftDrawDestroy(draw);
    XFlush(dpy);

    xcb_copy_area(conn, tpix, c->titlebar, tgc, 0, 0, 0, 0, (uint16_t)tw, (uint16_t)th);
    xcb_free_pixmap(conn, tpix);
    xcb_free_gc(conn, tgc);
    XftColorFree(dpy, visual, cmap, &color);
    xcb_flush(conn);
}

void titlebar_del(client *c) {
    if (!c) return;
    if (c->titlebar) {
        xcb_destroy_window(conn, c->titlebar);
        c->titlebar = 0;
    }
}

client *client_from_titlebar(xcb_window_t w) {
    for win
        if (c->titlebar == w) return c;
    return NULL;
}

int is_titlebar(xcb_window_t w) {
    return client_from_titlebar(w) != NULL;
}

typedef struct { int16_t x, y; uint16_t w, h; } ClipRect;

#define CLIP_MAX_PIECES 64

/* piece minus monitor rect -> up to 4 remainder strips.
   Returns -1 when disjoint (nothing to do), 0 when fully swallowed. */
static int clip_subtract_mon(const ClipRect *p, const MonitorInfo *m, ClipRect *out) {
    int n = 0;
    int px0 = p->x,        py0 = p->y;
    int px1 = p->x + p->w, py1 = p->y + p->h;
    int bx0 = m->x,        by0 = m->y;
    int bx1 = m->x + m->w, by1 = m->y + m->h;

    if (px0 >= bx1 || px1 <= bx0 || py0 >= by1 || py1 <= by0) return -1;

    if (px0 < bx0) {
        out[n].x = px0; out[n].y = py0; out[n].w = (uint16_t)(bx0 - px0); out[n].h = (uint16_t)(py1 - py0); n++;
    }
    if (px1 > bx1) {
        out[n].x = bx1; out[n].y = py0; out[n].w = (uint16_t)(px1 - bx1); out[n].h = (uint16_t)(py1 - py0); n++;
    }
    int mx0 = MAX(px0, bx0), mx1 = MIN(px1, bx1);
    if (mx1 > mx0) {
        if (py0 < by0) {
            out[n].x = mx0; out[n].y = py0; out[n].w = (uint16_t)(mx1 - mx0); out[n].h = (uint16_t)(by0 - py0); n++;
        }
        if (py1 > by1) {
            out[n].x = mx0; out[n].y = by1; out[n].w = (uint16_t)(mx1 - mx0); out[n].h = (uint16_t)(py1 - by1); n++;
        }
    }
    return n;
}

/* Shape a frame to itself minus every monitor except own_mon: clients may roam
   the whole canvas, but can never render or take input over another monitor. */
static void canvas_shape(xcb_window_t w, const ClipRect *frame, int own_mon) {
    if (!w) return;

    static ClipRect a[CLIP_MAX_PIECES], b[CLIP_MAX_PIECES];
    int na = 1, subbed = 0;
    a[0] = *frame;

    for (int m = 0; m < n_mons && na; m++) {
        if (m == own_mon) continue;
        int nb = 0;
        for (int i = 0; i < na && nb < CLIP_MAX_PIECES - 4; i++) {
            ClipRect rem[4];
            int k = clip_subtract_mon(&a[i], &mons[m], rem);
            if (k < 0) b[nb++] = a[i];               /* disjoint: keep */
            else { subbed = 1;                        /* k == 0 -> swallowed: drop */
                   for (int j = 0; j < k; j++) b[nb++] = rem[j]; }
        }
        memcpy(a, b, (size_t)nb * sizeof(ClipRect));
        na = nb;
    }

    if (!subbed) {
        xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, w, 0, 0, XCB_PIXMAP_NONE);
        return;
    }

    xcb_rectangle_t rects[CLIP_MAX_PIECES];
    int nr = 0;
    for (int i = 0; i < na; i++) {
        if (!a[i].w || !a[i].h) continue;
        rects[nr].x      = (int16_t)(a[i].x - frame->x);
        rects[nr].y      = (int16_t)(a[i].y - frame->y);
        rects[nr].width  = a[i].w;
        rects[nr].height = a[i].h;
        nr++;
    }

    if (nr == 0) {
        /* fully covered by other monitors: empty region -> hidden, no input */
        xcb_rectangle_t none = { 0, 0, 0, 0 };
        xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                             XCB_CLIP_ORDERING_UNSORTED, w, 0, 0, 1, &none);
    } else {
        xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                             XCB_CLIP_ORDERING_UNSORTED, w, 0, 0, (unsigned)nr, rects);
    }
}

static void canvas_clip_client(client *c, int sx, int sy) {
    unsigned int bw = wborder();
    int wb2 = 2 * (int)bw;

    ClipRect wf = { (int16_t)sx, (int16_t)sy,
                    (uint16_t)(c->width + wb2), (uint16_t)(c->height + wb2) };
    canvas_shape(c->w, &wf, c->mon);

    if (c->titlebar) {
        ClipRect tf = { (int16_t)sx, (int16_t)(sy - TITLEBAR_HEIGHT),
                        (uint16_t)(c->width + wb2), (uint16_t)TB_CONTENT_H };
        canvas_shape(c->titlebar, &tf, c->mon);
    }
}

void client_move(client *c, int x, int y) {
    if (!c) return;

    c->x = x;
    c->y = y;

    uint32_t values[2] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

    if (cur && c != cur) {
        uint32_t vst[2] = { c->w, XCB_STACK_MODE_ABOVE };
        xcb_configure_window(conn, cur->w, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, vst);
        if (cur->titlebar) {
            uint32_t tst[2] = { c->w, XCB_STACK_MODE_ABOVE };
            xcb_configure_window(conn, cur->titlebar, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, tst);
        }
    } else {
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    }

    if (c->titlebar) {
        uint32_t tv[2] = { (uint32_t)x, (uint32_t)(y - TITLEBAR_HEIGHT) };
        xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, tv);
        titlebar_update(c);

        uint32_t sv[2] = { c->w, XCB_STACK_MODE_ABOVE };
        xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, sv);
    }

    canvas_clip_client(c, x, y);

    docks_raise();
    xcb_flush(conn);
}

void update_borders(void) {
    for win {
        uint32_t values[1];
	if (c == cur) {
	    values[0] = cols.cs[2];
	} else {
	    values[0] = cols.cs[0];
	}
        xcb_change_window_attributes(conn, c->w, XCB_CW_BORDER_PIXEL, values);
        if (c->titlebar)
            xcb_change_window_attributes(conn, c->titlebar, XCB_CW_BORDER_PIXEL, values);
    }
    if (cur) {
        xcb_window_t w = cur->w;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_active_window, XCB_ATOM_WINDOW, 32, 1, &w);
    }
    xcb_flush(conn);
    update_client_list_stacking();
}

void update_border_widths(void) {
    uint32_t bw = wborder();
    for win {
        xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
        if (c->titlebar)
            xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
    }
    update_borders();
    canvas_apply_all();
    xcb_flush(conn);
}

void update_client_list_stacking(void) {
    xcb_query_tree_cookie_t qc = xcb_query_tree(conn, root);
    xcb_query_tree_reply_t *qr = xcb_query_tree_reply(conn, qc, NULL);
    if (!qr) return;
 
    xcb_window_t *children = xcb_query_tree_children(qr);
    int nchild = xcb_query_tree_children_length(qr);
 
    xcb_window_t *stack = malloc(sizeof(xcb_window_t) * (size_t)nchild);
    if (stack) {
        int n = 0;
        for (int i = 0; i < nchild; i++)
            for win
                if (c->w == children[i]) { stack[n++] = c->w; break; }
 
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_client_list_stacking,
                             XCB_ATOM_WINDOW, 32, (uint32_t)n, stack);
        free(stack);
        xcb_flush(conn);
    }
    free(qr);
}

void configure(client *c) {
    xcb_configure_notify_event_t ce = {0};
    ce.response_type = XCB_CONFIGURE_NOTIFY;
    ce.event = c->w;
    ce.window = c->w;
    ce.x = (int16_t)c->x;
    ce.y = (int16_t)c->y;
    ce.width = (uint16_t)c->width;
    ce.height = (uint16_t)c->height;
    ce.border_width = wborder();
    ce.above_sibling = XCB_NONE;
    ce.override_redirect = 0;
    xcb_send_event(conn, 0, c->w, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ce);
}

void resizeclient(client *c, int w, int h) {
    c->oldwidth = c->width;
    c->width = w;
    c->oldheight = c->height;
    c->height = h;

    uint32_t wv[2] = { (uint32_t)w, (uint32_t)h };
    xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, wv);
    configure(c);

    if (c->titlebar) {
        uint32_t tv[2] = { (uint32_t)w, TITLEBAR_HEIGHT };
        xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, tv);
    }
    canvas_clip_client(c, c->x, c->y);
    xcb_flush(conn);
}

void client_resize(client *c, unsigned int w, unsigned int h) {
    if (!c) return;
    int nw = (int)w, nh = (int)h;
    if (applysizehints(c, &nw, &nh))
        resizeclient(c, nw, nh);
    if (c->titlebar) titlebar_update(c);
}

void updatesizehints(client *c) {
    xcb_size_hints_t size = {0};
    xcb_get_property_cookie_t ck = xcb_icccm_get_wm_normal_hints(conn, c->w);
    if (!xcb_icccm_get_wm_normal_hints_reply(conn, ck, &size, NULL))
        size.flags = XCB_ICCCM_SIZE_HINT_P_SIZE;

    if (size.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) { c->basew = size.base_width; c->baseh = size.base_height; }
    else if (size.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) { c->basew = size.min_width; c->baseh = size.min_height; }
    else c->basew = c->baseh = 0;

    if (size.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) { c->incw = size.width_inc; c->inch = size.height_inc; }
    else c->incw = c->inch = 0;

    if (size.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) { c->maxw = size.max_width; c->maxh = size.max_height; }
    else c->maxw = c->maxh = 0;

    if (size.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) { c->minw = size.min_width; c->minh = size.min_height; }
    else if (size.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) { c->minw = size.base_width; c->minh = size.base_height; }
    else c->minw = c->minh = 0;

    if (size.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
        c->mina = (float)size.min_aspect_den / size.min_aspect_num;
        c->maxa = (float)size.max_aspect_num / size.max_aspect_den;
    } else c->mina = c->maxa = 0.0f;
}

int applysizehints(client *c, int *w, int *h) {
    int baseismin;

    *w = *w < 1 ? 1 : *w;
    *h = *h < 1 ? 1 : *h;

    baseismin = (c->basew == c->minw) && (c->baseh == c->minh);
    if (!baseismin) { *w -= c->basew; *h -= c->baseh; }

    if (c->mina > 0 && c->maxa > 0) {
        if (c->maxa < (float)*w / *h)
            *w = (int)(*h * c->maxa + 0.5);
        else if (c->mina < (float)*h / *w)
            *h = (int)(*w * c->mina + 0.5);
    }

    if (c->incw) *w -= *w % c->incw;
    if (c->inch) *h -= *h % c->inch;

    *w = MAX(*w + (baseismin ? 0 : c->basew), c->minw > 0 ? c->minw : 1);
    *h = MAX(*h + (baseismin ? 0 : c->baseh), c->minh > 0 ? c->minh : 1);

    if (c->maxw) *w = MIN(*w, c->maxw);
    if (c->maxh) *h = MIN(*h, c->maxh);

    return *w != c->width || *h != c->height;
}

char *client_get_title(xcb_window_t w) {
    xcb_icccm_get_wm_class_reply_t hint;
    if (xcb_icccm_get_wm_class_reply(conn, xcb_icccm_get_wm_class(conn, w), &hint, NULL)) {
        char *name = NULL;
        if (hint.class_name && *hint.class_name) name = copystr(hint.class_name);
        else if (hint.instance_name && *hint.instance_name) name = copystr(hint.instance_name);
        xcb_icccm_get_wm_class_reply_wipe(&hint);
        if (name) return name;
    }

    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, w, net_wm_name, ewmh_utf8_string, 0, 1024);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, ck, NULL);
    if (r) {
        int len = xcb_get_property_value_length(r);
        if (len > 0) {
            char *name = malloc((size_t)len + 1);
            if (name) {
                memcpy(name, xcb_get_property_value(r), (size_t)len);
                name[len] = 0;
                free(r);
                return name;
            }
        }
        free(r);
    }
    return copystr("unknown");
}

void titlebar_update(client *c) {
    if (c && c->titlebar) titlebar_draw(c);
}

static void canvas_sync_to_root(void) {
    unsigned long pan_x_raw[MAX_MONITORS];
    unsigned long pan_y_raw[MAX_MONITORS];
    for (int i = 0; i < MAX_MONITORS; i++) {
        memcpy(&pan_x_raw[i], &canvas.pan_x[i], sizeof(float));
        memcpy(&pan_y_raw[i], &canvas.pan_y[i], sizeof(float));
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, canvas_atom_pan_x,
                         XCB_ATOM_CARDINAL, 32, MAX_MONITORS, pan_x_raw);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, canvas_atom_pan_y,
                         XCB_ATOM_CARDINAL, 32, MAX_MONITORS, pan_y_raw);
}

void canvas_apply_all(void) {
    for win {
        if (c->f) continue;

        int m = c->mon;
        float px = canvas.pan_x[m];
        float py = canvas.pan_y[m];

        int sx = canvas_to_screen(c->cx, px);
        int sy = canvas_to_screen(c->cy, py);

        canvas_clip_client(c, sx, sy);

        client_move(c, sx, sy);
    }

    xcb_flush(conn);
    titlebar_update(cur);
    canvas_sync_to_root();
    icons_reposition();
}

void canvas_pan(int mon, float dx, float dy) {
    if (mon < 0 || mon >= MAX_MONITORS) return;
    canvas.pan_x[mon] += dx;
    canvas.pan_y[mon] += dy;
    canvas_apply_all();
}

void canvas_pan_key(const Arg arg) {
    int   mon  = mon_at_ptr();
    float step = (float)cfg->pan_step;
    switch (arg.i) {
        case 0: canvas_pan(mon, -step,  0);    break;
        case 1: canvas_pan(mon,  step,  0);    break;
        case 2: canvas_pan(mon,  0,    -step); break;
        case 3: canvas_pan(mon,  0,     step); break;
    }
}

void canvas_reset(const Arg arg) {
    (void)arg;
    int mon = mon_at_ptr();
    canvas.pan_x[mon] = 0;
    canvas.pan_y[mon] = 0;
    canvas_apply_all();
}

void ctx_close(void) {
    if (ctx_win == XCB_NONE) return;
    xcb_destroy_window(conn, ctx_win);
    ctx_win = XCB_NONE;
    xcb_flush(conn);
}

void ctx_open(int x, int y) {
    ctx_close();

    if (!cfg || cfg->nctx <= 0) return;

    if (!title_font) title_font = open_font(cfg->fontb);

    int padx = 8, pady = 4;
    int itemh;
    int w = 0, h;
    if (title_font) {
        itemh = title_font->ascent + title_font->descent + 2 * pady;
        for (int i = 0; i < cfg->nctx; i++) {
            XGlyphInfo ext;
            if (cfg->ctx[i].label) {
                XftTextExtentsUtf8(dpy, title_font, (const FcChar8 *)cfg->ctx[i].label,
                                   (int)strlen(cfg->ctx[i].label), &ext);
                if ((int)ext.xOff > w) w = (int)ext.xOff;
            }
        }
    } else {
        itemh = 24;
        for (int i = 0; i < cfg->nctx; i++) {
            int len = cfg->ctx[i].label ? (int)strlen(cfg->ctx[i].label) : 1;
            if (len * 10 > w) w = len * 10;
        }
    }

    w += 2 * padx;
    h = cfg->nctx * itemh + 2 * pady;

    int cx = CLAMP(x, 0, MAX(0, sw - w));
    int cy = CLAMP(y, 0, MAX(0, sh - h));

    ctx_x = cx; ctx_y = cy; ctx_w = w; ctx_h = h; ctx_itemh = itemh;

    unsigned long bg = cfg->ctxbg ? hex_to_xcolor(cfg->ctxbg) : cols.background;
    unsigned long bd = cfg->ctxborder ? hex_to_xcolor(cfg->ctxborder) : cols.cs[2];

    ctx_win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[] = { bg, bd, 1 };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, ctx_win, root,
                      (int16_t)cx, (int16_t)cy, (uint16_t)w, (uint16_t)h,
                      (uint16_t)wborder(), XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);
    xcb_map_window(conn, ctx_win);

    uint32_t stack = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, ctx_win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    xcb_flush(conn);

    if (title_font) {
        XftDraw *draw = XftDrawCreate(dpy, ctx_win, visual, cmap);
        XftColor color;
        xcolor_to_xftcolor(cols.foreground, &color);
        for (int i = 0; i < cfg->nctx; i++) {
            if (!cfg->ctx[i].label) continue;
            int ly = pady + i * itemh + (itemh + title_font->ascent - title_font->descent) / 2;
            XftDrawStringUtf8(draw, &color, title_font, padx, ly,
                              (const FcChar8 *)cfg->ctx[i].label, (int)strlen(cfg->ctx[i].label));
        }
        XftDrawDestroy(draw);
    }

    XFlush(dpy);
    xcb_flush(conn);
}

void win_focus(client *c) {
    client *prev = cur;
    ctx_close();

    if (!c)
        c = list ? list->prev : NULL;

    cur = c;

    if (c) {
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_PARENT, cur->w, XCB_CURRENT_TIME);
	titlebar_draw(cur);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_active_window,
                             XCB_ATOM_WINDOW, 32, 1, &cur->w);
    } else {
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
        xcb_delete_property(conn, root, net_active_window);
    }

    if (prev && prev != cur) titlebar_draw(prev);

    xcb_flush(conn);
    update_borders();
}

void titlebar_focus(xcb_window_t w) {
    client *own = client_from_titlebar(w);
    if (own) win_focus(own);
}

void canvas_focus(client *c) {
    if (!c || (cur && cur->f) || c->f) return;

    int m = c->mon;
    int mx = 0, my = 0, mw = sw, mh = sh;
    if (m < n_mons) { mx = mons[m].x; my = mons[m].y; mw = mons[m].w; mh = mons[m].h; }

    unsigned int cw, ch;
    win_size(c->w, NULL, NULL, &cw, &ch);

    float target_sx = mx + (mw - (int)cw) / 2.0f;
    float target_sy = my + (mh - (int)ch) / 2.0f;

    canvas.pan_x[m] = c->cx - target_sx;
    canvas.pan_y[m] = c->cy - target_sy;

    canvas_apply_all();

    uint32_t stack = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    if (c->titlebar) xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);

    docks_raise();
    win_focus(c);
    titlebar_update(cur);
}

static long phys_last_ms   = 0;
static int  phys_dirty     = 0;
static client *phys_held   = NULL;

#define THROW_SAMPLES 8
static int16_t th_x[THROW_SAMPLES];
static int16_t th_y[THROW_SAMPLES];
static long    th_t[THROW_SAMPLES];
static int     th_n;

typedef struct { float x0, y0, x1, y1; } PhysBox;

void physics_wake(client *c)  { if (c && !c->f) c->awake = 1; }
void physics_sleep(client *c) { if (c) { c->vx = 0; c->vy = 0; c->awake = 0; } }

void physics_init(void) { phys_last_ms = now_ms(); }

static void throw_reset(void) { th_n = 0; }

static void throw_sample(int x, int y) {
    int i = th_n % THROW_SAMPLES;
    th_x[i] = (int16_t)x;
    th_y[i] = (int16_t)y;
    th_t[i] = now_ms();
    th_n++;
}

void physics_drag_release(client *c, int px, int py) {
    if (!c || c->f || !PHYS_ENABLED) return;
    throw_sample(px, py);

    long   now = th_t[(th_n - 1) % THROW_SAMPLES];
    int    old = -1;
    for (int k = 1; k <= th_n && k <= THROW_SAMPLES; k++) {
        int idx = (th_n - k) % THROW_SAMPLES;
        if (now - th_t[idx] <= THROW_WINDOW_MS) old = idx;
        else break;
    }

    float vx = 0, vy = 0;
    if (old >= 0 && old != (th_n - 1) % THROW_SAMPLES) {
        long   dt = now - th_t[old];
        if (dt > 10) {
            vx = ((float)(th_x[(th_n-1) % THROW_SAMPLES] - th_x[old]) * 1000.0f / (float)dt) * THROW_BOOST;
            vy = ((float)(th_y[(th_n-1) % THROW_SAMPLES] - th_y[old]) * 1000.0f / (float)dt) * THROW_BOOST;
        }
    }

    float sp = sqrtf(vx*vx + vy*vy);
    if (sp > PHYS_VMAX) { vx *= PHYS_VMAX/sp; vy *= PHYS_VMAX/sp; }

    if (sp > PHYS_STOP_SPEED) {
        c->vx = vx;
        c->vy = vy;
        c->awake = 1;
    } else {
        physics_sleep(c);
    }
}

void physics_drag_start(client *c) {
    phys_held = c;
    if (!c) return;
    physics_sleep(c);
    throw_reset();
}

static void phys_box(const client *c, PhysBox *b) {
    unsigned int bw = wborder();
    int tb = cfg->titlebar ? TITLEBAR_HEIGHT : 0;
    b->x0 = c->cx - (float)bw;
    b->y0 = c->cy - (float)tb;
    b->x1 = c->cx + (float)c->width  + (float)bw;
    b->y1 = c->cy + (float)c->height + (float)bw;
}

static float phys_mass(const client *c) {
    float area = (float)c->width * (float)c->height;
    return MAX(area, 40000.0f);
}

static int phys_overlap(const PhysBox *a, const PhysBox *b, float *ox, float *oy) {
    *ox = MIN(a->x1, b->x1) - MAX(a->x0, b->x0);
    *oy = MIN(a->y1, b->y1) - MAX(a->y0, b->y0);
    return (*ox > 0.0f && *oy > 0.0f);
}

static void phys_resolve(client *a, client *b) {
    if (a == b || a->f || b->f || a->mon != b->mon || a->mon >= n_mons) return;

    PhysBox ba, bb, *pa = &ba, *pb = &bb;
    phys_box(a, pa);
    phys_box(b, pb);

    float ox, oy;
    if (!phys_overlap(pa, pb, &ox, &oy)) return;

    float ia = (a == phys_held) ? 0.0f : 1.0f / phys_mass(a);
    float ib = (b == phys_held) ? 0.0f : 1.0f / phys_mass(b);
    float isum = ia + ib;
    if (isum <= 0.0f) return;

    float nx = 0, ny = 0, pen;
    if (ox < oy) {
        nx  = ((a->cx + a->width * 0.5f) < (b->cx + b->width * 0.5f)) ? -1.0f : 1.0f;
        pen = ox;
    } else {
        ny  = ((a->cy + a->height * 0.5f) < (b->cy + b->height * 0.5f)) ? -1.0f : 1.0f;
        pen = oy;
    }

    float corr = MAX(pen - PHYS_SLOP, 0.0f) / isum * PHYS_CORRECT;
    a->cx += nx * corr * ia;
    a->cy += ny * corr * ia;
    b->cx -= nx * corr * ib;
    b->cy -= ny * corr * ib;

    float rv = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    if (rv < 0.0f) {
        float j = -(1.0f + PHYS_RESTITUTION) * rv / isum;
        a->vx += j * ia * nx;
        a->vy += j * ia * ny;
        b->vx -= j * ib * nx;
        b->vy -= j * ib * ny;
    }

    if (ia > 0.0f && sqrtf(a->vx*a->vx + a->vy*a->vy) > PHYS_STOP_SPEED) a->awake = 1;
    if (ib > 0.0f && sqrtf(b->vx*b->vx + b->vy*b->vy) > PHYS_STOP_SPEED) b->awake = 1;

    phys_dirty = 1;
}

void physics_push_away(client *pusher) {
    if (!pusher || !PHYS_ENABLED || pusher->f || !list) return;

    static float prev_cx = 0, prev_cy = 0;
    static long  prev_ms = 0;
    long  now = now_ms();
    float vpx = 0, vpy = 0;
    if (prev_ms && now > prev_ms) {
        vpx = (pusher->cx - prev_cx) * 1000.0f / (float)(now - prev_ms);
        vpy = (pusher->cy - prev_cy) * 1000.0f / (float)(now - prev_ms);
    }
    prev_cx = pusher->cx;
    prev_cy = pusher->cy;
    prev_ms = now;
    float vspeed = MAX(sqrtf(vpx*vpx + vpy*vpy), 140.0f);

    for win {
        client *o = c;
        if (o == pusher || o->f || o->mon != pusher->mon || o->mon >= n_mons) continue;

        PhysBox bp, bo;
        phys_box(pusher, &bp);
        phys_box(o, &bo);

        float ox, oy;
        if (!phys_overlap(&bp, &bo, &ox, &oy)) continue;

        float nx = 0, ny = 0, pen;
        if (ox < oy) {
            nx  = ((pusher->cx + pusher->width * 0.5f) < (o->cx + o->width * 0.5f)) ? -1.0f : 1.0f;
            pen = ox;
        } else {
            ny  = ((pusher->cy + pusher->height * 0.5f) < (o->cy + o->height * 0.5f)) ? -1.0f : 1.0f;
            pen = oy;
        }

        o->cx += nx * (pen + 2.0f);
        o->cy += ny * (pen + 2.0f);
        o->vx  = nx * vspeed;
        o->vy  = ny * vspeed;
        o->awake = 1;

        float px = canvas.pan_x[o->mon], py = canvas.pan_y[o->mon];
        client_move(o, canvas_to_screen(o->cx, px), canvas_to_screen(o->cy, py));
        phys_dirty = 1;
    }
}

static int phys_any_moving(void) {
    for win
        if (!c->f && c->awake) return 1;
    return 0;
}

static int phys_sleep_ok(const client *me) {
    PhysBox bc;
    phys_box(me, &bc);
    for win {
        if (c == me || c->f || c->mon != me->mon || c->mon >= n_mons) continue;
        PhysBox bo;
        phys_box(c, &bo);
        float ox, oy;
        if (phys_overlap(&bc, &bo, &ox, &oy)) return 0;
    }
    return 1;
}

void physics_tick(void) {
    if (!PHYS_ENABLED) return;

    long now = now_ms();
    float dt = (now - phys_last_ms) / 1000.0f;
    phys_last_ms = now;
    if (dt < 0.0f)   dt = 0.0f;
    if (dt > 0.05f)  dt = 0.05f;
    if (!list || !phys_any_moving()) return;

    float damp = expf(-PHYS_FRICTION * dt);

    for win {
        if (c->f || !c->awake || c == phys_held) continue;
        c->vx *= damp;
        c->vy *= damp;
        float sp = sqrtf(c->vx*c->vx + c->vy*c->vy);
        if (sp > PHYS_VMAX) { c->vx *= PHYS_VMAX/sp; c->vy *= PHYS_VMAX/sp; }
        c->cx += c->vx * dt;
        c->cy += c->vy * dt;
        phys_dirty = 1;
    }

    for (client *a = list; a;) {
        for (client *b = a->next; b && b != list; b = b->next)
            phys_resolve(a, b);
        a = (a->next == list) ? NULL : a->next;
    }

    for win {
        if (c->f || !c->awake || c == phys_held) continue;
        float sp = sqrtf(c->vx*c->vx + c->vy*c->vy);
        if (sp < PHYS_STOP_SPEED && phys_sleep_ok(c))
            physics_sleep(c);
    }

    if (phys_dirty) {
        canvas_apply_all();
        phys_dirty = 0;
    }
}


void win_prev(const Arg arg) {
    (void)arg;
    if (!cur || !list) return;
    int m = cur->mon;
    if (cur->f) return;

    client *c = cur->prev;
    int checked = 0, total = 0;
    for (client *t = list; t->prev != list; t = t->prev) total++;
    total++;

    while (c != cur && checked < total) {
        if (c->mon == m) break;
        c = c->prev ? c->prev : list;
        checked++;
    }
    if (c && c != cur && c->mon == m) canvas_focus(c);
}

void win_next(const Arg arg) {
    (void)arg;
    if (!cur || !list) return;
    int m = cur->mon;
    if (cur->f) return;

    client *c = cur->next;
    int checked = 0, total = 0;
    for (client *t = list; t->next != list; t = t->next) total++;
    total++;

    while (c != cur && checked < total) {
        if (c->mon == m) break;
        c = c->next ? c->next : list;
        checked++;
    }
    if (c && c != cur && c->mon == m) canvas_focus(c);
}

void notify_destroy(xcb_destroy_notify_event_t *gen_e) {
    xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)gen_e;

    if (e->window == ctx_win) ctx_win = XCB_NONE;

    dock_del(e->window);

    if (e->window == fs_grab_win) { xcb_ungrab_pointer(conn, XCB_CURRENT_TIME); fs_grab_win = XCB_NONE; }

    int managed = 0;
    for win
        if (c->w == e->window) { managed = 1; break; }
    if (!managed) return;

    win_del(e->window);

    win_focus(NULL);
    titlebar_update(cur);
}

void client_message(xcb_generic_event_t *gen_e) {
    xcb_client_message_event_t *e = (xcb_client_message_event_t *)gen_e;

    if (e->type == wm_protocols && e->data.data32[0] == wm_delete_window) {
        win_del(e->window);
        return;
    }

    if (e->type == net_active_window) {
        client *target = NULL;
        for win
            if (c->w == e->window) { target = c; break; }
        if (target) canvas_focus(target);
        return;
    }

    if (e->type == net_wm_state) {
        client *target = NULL;
        for win
            if (c->w == e->window) { target = c; break; }
        if (!target) return;

        for (int i = 1; i <= 2; i++) {
            xcb_atom_t prop = e->data.data32[i];
            if (prop == XCB_ATOM_NONE) continue;

            int action = e->data.data32[0];
            int want_fs;

            if (action == 0) want_fs = 0;
            else if (action == 1) want_fs = 1;
            else want_fs = !target->f;

            if (prop == net_wm_state_fullscreen && want_fs != target->f) {
                client *old_cur = cur;
                cur = target;
                win_fs((Arg){0});
                cur = old_cur ? old_cur : target;
            }
        }
        return;
    }
}

void configure_notify(xcb_configure_notify_event_t *e) {
    if (e->window == root) {
        sw = e->width;
        sh = e->height;
        monitors_refresh();
        canvas_apply_all();
        return;
    }

    if (dock_known(e->window))
        return;

    update_client_list_stacking();
}

void focusin(xcb_focus_in_event_t *e) {
    if (e->mode == XCB_NOTIFY_MODE_GRAB || e->mode == XCB_NOTIFY_MODE_UNGRAB)
        return;
    if (ctx_win != XCB_NONE) ctx_close();
    if (cur && e->event != cur->w)
        xcb_set_input_focus(conn, XCB_INPUT_FOCUS_PARENT, cur->w, XCB_CURRENT_TIME);
}
 
void notify_unmap(xcb_unmap_notify_event_t *e) {
    xcb_window_t w = e->window;

    dock_del(w);

    int managed = 0;
    for win
        if (c->w == w) { managed = 1; break; }
    if (!managed) return;

    win_del(w);
    win_focus(NULL);
}

void notify_enter(xcb_enter_notify_event_t *e) {
    if (e->mode != XCB_NOTIFY_MODE_NORMAL || e->detail == XCB_NOTIFY_DETAIL_INFERIOR)
        return;

    for win
        if (c->w == e->event) {
            win_focus(c);
            if (c->titlebar) titlebar_update(c);
        }
    titlebar_update(cur);
}

void notify_property(xcb_property_notify_event_t *e) {
    for win {
        if (c->w == e->window) {
            if (e->atom == XCB_ATOM_WM_NAME || e->atom == net_wm_visible_name || e->atom == net_wm_name)
                titlebar_update(c);
            else if (e->atom == wm_normal_hints_atom)
                updatesizehints(c);
            break;
        }
    }
}

void notify_motion(xcb_motion_notify_event_t *e) {
    if (pan_active) {
        float dx = e->root_x - pan_start_x;
        float dy = e->root_y - pan_start_y;
        canvas.pan_x[pan_mon] = pan_origin_x - dx;
        canvas.pan_y[pan_mon] = pan_origin_y - dy;
        canvas_apply_all();
        return;
    }

    if (icon_handle_motion(e)) return;

    if (!cur || !drag_subwindow || cur->f) return;
 
    int xd = e->root_x - drag_root_x;
    int yd = e->root_y - drag_root_y;
 
    if (drag_button == XCB_BUTTON_INDEX_1) {
        throw_sample(e->root_x, e->root_y);

        int new_sx = cur->wx + xd;
        int new_sy = cur->wy + yd;
 
        int cenx = new_sx + (int)(cur->ww / 2);
        int ceny = new_sy + (int)(cur->wh / 2);
 
	for (int i = 0; i < n_mons; i++) {
	    if (cenx >= mons[i].x && cenx < mons[i].x + mons[i].w &&
		ceny >= mons[i].y && ceny < mons[i].y + mons[i].h) {
		set_client_monitor(cur, i);
		    break;
	    }
 
	}
 
        client_move(cur, new_sx, new_sy);
 
        if (cur->titlebar) {
            uint32_t stack = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(conn, cur->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        }
        docks_raise();
 
        int m = cur->mon;
        cur->cx = (float)new_sx + canvas.pan_x[m];
        cur->cy = (float)new_sy + canvas.pan_y[m];

        physics_push_away(cur);
    } else if (drag_button == XCB_BUTTON_INDEX_3) {
        client_resize(cur, (unsigned)MAX(1, (int)cur->ww + xd), (unsigned)MAX(1, (int)cur->wh + yd));
    }
}

void key_press(xcb_key_press_event_t *e) {
    xcb_keysym_t keysym = xcb_key_press_lookup_keysym(keysyms, e, 0);
    for (unsigned int i = 0; i < (unsigned)cfg->nkeys; ++i)
	if (cfg->keys[i].keysym == keysym && mod_clean(cfg->keys[i].mod) == mod_clean(e->state))
	    cfg->keys[i].function(cfg->keys[i].arg);

    for (int i = 0; i < cfg->nshortcuts; ++i) {
        if (cfg->shortcuts[i].keysym == keysym &&
            mod_clean(cfg->shortcuts[i].mod) == mod_clean(e->state)) {
            Arg a = { .com = (const char **)cfg->shortcuts[i].cmd };
            run(a);
            return;
        }
    }
}

static void begin_pointer_grab(void) {
    xcb_grab_pointer_cookie_t gck = xcb_grab_pointer(conn, 0, root,
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(conn, gck, NULL);
    if (gr) free(gr);
}

void button_press(xcb_button_press_event_t *gen_e) {
    xcb_button_press_event_t *e = (xcb_button_press_event_t *)gen_e;

    if (icon_handle_press(e)) return;

    if (e->event == root && (e->child == XCB_NONE || e->child == ctx_win)) {
        if (ctx_win != XCB_NONE && e->detail == XCB_BUTTON_INDEX_1) {
            int idx = (e->root_y - ctx_y) / ctx_itemh;
            ctx_close();
            if (idx >= 0 && idx < cfg->nctx && cfg->ctx[idx].function)
                cfg->ctx[idx].function(cfg->ctx[idx].arg);
            return;
        }
        if (e->detail == XCB_BUTTON_INDEX_3) {
            ctx_open(e->root_x, e->root_y);
            return;
        }
        if (e->detail == XCB_BUTTON_INDEX_1 || e->detail == XCB_BUTTON_INDEX_2) {
            pan_active   = 1;
            pan_mon      = mon_at_ptr();
            pan_start_x  = e->root_x;
            pan_start_y  = e->root_y;
            pan_origin_x = canvas.pan_x[pan_mon];
            pan_origin_y = canvas.pan_y[pan_mon];
        }
        ctx_close();
        return;
    }

    if (!cur) return;
 
    if (e->detail == XCB_BUTTON_INDEX_2) {
        pan_active   = 1;
        pan_mon      = mon_at_ptr();
        pan_start_x  = e->root_x;
        pan_start_y  = e->root_y;
        pan_origin_x = canvas.pan_x[pan_mon];
        pan_origin_y = canvas.pan_y[pan_mon];
        return;
    }
 
    if (is_titlebar(e->event) && !(e->state & MOD)) {
        client *c = client_from_titlebar(e->event);
        if (!c) return;
 
        unsigned int tw, th;
        win_size(c->titlebar, NULL, NULL, &tw, &th);
 
        int btn_w  = 22;
        int btn_x = (int)tw - 26;
        int btn_f  = btn_x - btn_w - 2;

        if (e->detail == XCB_BUTTON_INDEX_1 && e->event_x >= btn_x) { win_kill((Arg){0}); return; }
        if (e->detail == XCB_BUTTON_INDEX_1 && e->event_x >= btn_f && e->event_x < btn_x) { win_fs((Arg){0}); return; }
 
        win_focus(c);
        win_size(c->w, &c->wx, &c->wy, &c->ww, &c->wh);
        drag_subwindow = c->w;
        drag_button = e->detail;
        drag_root_x = e->root_x;
        drag_root_y = e->root_y;
        begin_pointer_grab();
        physics_drag_start(c);
 
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, c->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        xcb_configure_window(conn, c->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        docks_raise();
        xcb_flush(conn);
        return;
    }
  
    if (!e->child) return;
  
    client *target = NULL;
    for win {
        if (c->w == e->child || c->titlebar == e->child) { target = c; break; }
    }
  
    if (target) {
        win_focus(target);
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, target->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        if (target->titlebar) xcb_configure_window(conn, target->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        win_size(target->w, &target->wx, &target->wy, &target->ww, &target->wh);
    } else {
        win_size(e->child, &cur->wx, &cur->wy, &cur->ww, &cur->wh);
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, e->child, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
        if (cur && cur->titlebar) xcb_configure_window(conn, cur->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    }
    docks_raise();
  
    drag_subwindow = e->child;
    drag_button = e->detail;
    drag_root_x = e->root_x;
    drag_root_y = e->root_y;

    client *held = NULL;
    for win if (c->w == e->child || c->titlebar == e->child) { held = c; break; }
    begin_pointer_grab();
    physics_drag_start(held);
 
    xcb_flush(conn);
}

void button_release(xcb_button_release_event_t *e) {
    if (icon_handle_release(e)) return;
    if (pan_active) pan_active = 0;

    if (drag_subwindow && drag_button == XCB_BUTTON_INDEX_1) {
        client *dc = NULL;
        for win if (c->w == drag_subwindow) dc = c;
        if (dc) physics_drag_release(dc, e->root_x, e->root_y);
    }

    phys_held   = NULL;
    drag_subwindow = 0;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);
}

void win_add(xcb_window_t w) {
    client *c = calloc(1, sizeof(client));
    if (!c) exit(1);

    c->w = w;
    set_client_monitor(c, mon_at_win(w));

    int sx = 0, sy = 0;
    unsigned int dw2, dh2;
    win_size(w, &sx, &sy, &dw2, &dh2);
    int m = c->mon;
    c->cx = (float)sx + canvas.pan_x[m];
    c->cy = (float)sy + canvas.pan_y[m];

    c->x = sx;
    c->y = sy;
    c->width  = (int)dw2;
    c->height = (int)dh2;
    updatesizehints(c);

    if (cfg->titlebar) {
        c->titlebar = titlebar_create(c);
        titlebar_update(c);
    }

    if (list) {
        list->prev->next = c;
        c->prev          = list->prev;
        list->prev       = c;
        c->next          = list;
    } else {
        list       = c;
        list->prev = list->next = list;
    }
    xcb_flush(conn);
}

void win_del(xcb_window_t w) {
    client *x = NULL;
    for win if (c->w == w) x = c;
    if (!list || !x) return;

    if (phys_held == x) {
        phys_held = NULL;
        drag_subwindow = 0;
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    }

    if (x->titlebar) titlebar_del(x);
    
    if (x->prev == x) list = NULL;
    if (list == x)    list = x->next;
    if (x->next) x->next->prev = x->prev;
    if (x->prev) x->prev->next = x->next;
    
    if (x == cur) cur = NULL;
    
    free(x);
    xcb_flush(conn);
}

void win_kill(const Arg arg) {
    (void)arg;
    if (!cur) return;

    xcb_icccm_get_wm_protocols_reply_t protocols;
    int has_delete = 0;
    if (xcb_icccm_get_wm_protocols_reply(conn,
            xcb_icccm_get_wm_protocols(conn, cur->w, wm_protocols), &protocols, NULL)) {
        for (uint32_t i = 0; i < protocols.atoms_len; i++)
            if (protocols.atoms[i] == wm_delete_window) { has_delete = 1; break; }
        xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    }

    if (has_delete) {
        xcb_client_message_event_t ev = {0};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = cur->w;
        ev.type = wm_protocols;
        ev.data.data32[0] = wm_delete_window;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(conn, 0, cur->w, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    } else {
        xcb_kill_client(conn, cur->w);
    }
    xcb_flush(conn);
}

void win_center(const Arg arg) {
    (void)arg;
    if (!cur || cur->f) return;

    unsigned int ww_, wh_;
    win_size(cur->w, NULL, NULL, &ww_, &wh_);

    xcb_query_pointer_reply_t *ptr = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    int mx = 0, my = 0, mw = sw, mh = sh;
    if (ptr) {
        int m = mon_from_point(ptr->root_x, ptr->root_y);
        if (m < n_mons) { mx = mons[m].x; my = mons[m].y; mw = mons[m].w; mh = mons[m].h; }
        free(ptr);
    }

    int total_w = (int)ww_;
    int total_h = (int)wh_ + wborder();

    int sx = mx + (mw - total_w) / 2;
    int sy = my + (mh - total_h) / 2;

    client_move(cur, sx, sy);

    set_client_monitor(cur, mon_at_win(cur->w));
    int m = cur->mon;
    cur->cx = (float)sx + canvas.pan_x[m];
    cur->cy = (float)sy + canvas.pan_y[m];
}

static int spawn_spot_free(const client *skip, int sx, int sy, unsigned int ww, unsigned int wh) {
    unsigned int bw = wborder();
    int tb = cfg->titlebar ? TITLEBAR_HEIGHT : 0;

    int ax0 = sx - (int)bw - 1,          ay0 = sy - tb - (int)bw - 1;
    int ax1 = sx + (int)ww + (int)bw + 1, ay1 = sy + (int)wh + (int)bw + 1;

    for win {
        if (c == skip || c->f || !c->width || !c->height) continue;
        int bx0 = c->x - (int)bw,             by0 = c->y - tb;
        int bx1 = c->x + c->width + (int)bw,  by1 = c->y + c->height + (int)bw;
        if (ax0 < bx1 && ax1 > bx0 && ay0 < by1 && ay1 > by0) return 0;
    }
    return 1;
}

static int spawn_search_rings(const client *skip, int bx, int by,
                              unsigned int ww, unsigned int wh, int require_inside,
                              int mx, int my, int mw, int mh, int *ox, int *oy) {
    unsigned int bw = wborder();
    int bwi = (int)bw;
    int tw = (int)ww + 2 * (int)bw;
    int th = (int)wh + 2 * (int)bw + wborder();

    for (int r = 1; r <= SPAWN_SEARCH_MAX; r++) {
        for (int t = -r; t <= r; t++) {
            int cand[4][2] = {
                { bx + r * SPAWN_SEARCH_STEP, by + t * SPAWN_SEARCH_STEP },
                { bx - r * SPAWN_SEARCH_STEP, by + t * SPAWN_SEARCH_STEP },
                { bx + t * SPAWN_SEARCH_STEP, by + r * SPAWN_SEARCH_STEP },
                { bx + t * SPAWN_SEARCH_STEP, by - r * SPAWN_SEARCH_STEP },
            };
            for (int k = 0; k < 4; k++) {
                int sx = cand[k][0], sy = cand[k][1];
                if (require_inside &&
                    (sx < mx + bwi || sx > mx + mw - tw ||
                     sy < my + bwi || sy > my + mh - th))
                    continue;
                if (spawn_spot_free(skip, sx, sy, ww, wh)) { *ox = sx; *oy = sy; return 1; }
            }
        }
    }
    return 0;
}

void win_place_free(client *c) {
    if (!c || c->f) return;

    unsigned int ww_, wh_;
    win_size(c->w, NULL, NULL, &ww_, &wh_);

    xcb_query_pointer_reply_t *ptr = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    int mx = 0, my = 0, mw = sw, mh = sh;
    if (ptr) {
        int m = mon_from_point(ptr->root_x, ptr->root_y);
        if (m < n_mons) { mx = mons[m].x; my = mons[m].y; mw = mons[m].w; mh = mons[m].h; }
        free(ptr);
    }

    unsigned int bw = wborder();
    int tw = (int)ww_ + 2 * (int)bw;
    int th = (int)wh_ + 2 * (int)bw + wborder();

    int bx = mx + (mw - tw) / 2;
    int by = my + (mh - th) / 2;

    int fx = bx, fy = by;

    if (!spawn_spot_free(c, bx, by, ww_, wh_)) {
        if (!spawn_search_rings(c, bx, by, ww_, wh_, 1, mx, my, mw, mh, &fx, &fy) &&
            !spawn_search_rings(c, bx, by, ww_, wh_, 0, 0, 0, 0, 0, &fx, &fy)) {
            fx = bx + (SPAWN_SEARCH_MAX + 1) * SPAWN_SEARCH_STEP;
            fy = by + (SPAWN_SEARCH_MAX + 1) * SPAWN_SEARCH_STEP;
        }
    }

    client_move(c, fx, fy);

    set_client_monitor(c, mon_at_win(c->w));
    int m = c->mon;
    c->cx = (float)fx + canvas.pan_x[m];
    c->cy = (float)fy + canvas.pan_y[m];
}

void win_fs(const Arg arg) {
    (void)arg;
    if (!cur) return;
    if (!cur->f) win_size(cur->w, &cur->wx, &cur->wy, &cur->ww, &cur->wh);

    char buf[256];
    char *win_title = client_get_title(cur->w);

    xcb_query_pointer_reply_t *ptr = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    int mx = 0, my = 0, mw = sw, mh = sh;
    if (ptr) {
        int m = mon_from_point(ptr->root_x, ptr->root_y);
        if (m < n_mons) { mx = mons[m].x; my = mons[m].y; mw = mons[m].w; mh = mons[m].h; }
        free(ptr);
    }

    cur->f = !cur->f;

    if (cur->f) {
	xcb_grab_pointer_cookie_t gck = xcb_grab_pointer(conn, 1, root,
	    XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
	    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, cur->w, XCB_NONE, XCB_CURRENT_TIME);
	xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(conn, gck, NULL);
	if (gr && gr->status == XCB_GRAB_STATUS_SUCCESS) fs_grab_win = cur->w;
	free(gr);

        if (cfg->titlebar) {
            resizeclient(cur, mw, mh - TITLEBAR_HEIGHT);
            client_move(cur, mx, my + TITLEBAR_HEIGHT);
        } else {
            resizeclient(cur, mw, mh);
            client_move(cur, mx, my);

        }
	snprintf(buf, sizeof(buf), "%s: Fulscreen", win_title ? win_title : "");
        if (cur->titlebar) titlebar_update(cur);
        if (!cfg->titlebar && cur->titlebar) xcb_unmap_window(conn, cur->titlebar);
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, cur->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    } else {
	if (fs_grab_win == cur->w) { xcb_ungrab_pointer(conn, XCB_CURRENT_TIME); fs_grab_win = XCB_NONE; }
        resizeclient(cur, (int)cur->ww, (int)cur->wh);
        client_move(cur, cur->wx, cur->wy);
	snprintf(buf, sizeof(buf), "%s: Floating", win_title ? win_title : "");
        if (cur->titlebar) {
            xcb_map_window(conn, cur->titlebar);
            uint32_t stack = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(conn, cur->titlebar, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
            titlebar_update(cur);
        }
        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, cur->w, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
    }

    docks_raise();
    notify_show(buf, 0x202020);

    xcb_atom_t fs_atom = cur->f ? net_wm_state_fullscreen : XCB_ATOM_NONE;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, cur->w, net_wm_state,
                        XCB_ATOM_ATOM, 32, 1, &fs_atom);

    free(win_title);
    xcb_flush(conn);
}

void win_round_corners(xcb_window_t w, int rad) {
    unsigned int rw, rh;
    unsigned int dia = 2 * (unsigned int)rad;
    win_size(w, NULL, NULL, &rw, &rh);
    if (rw < dia || rh < dia) return;

    xcb_pixmap_t mask = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 1, mask, w, (uint16_t)rw, (uint16_t)rh);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, mask, 0, NULL);

    uint32_t fg0 = 0;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &fg0);
    xcb_rectangle_t full = { 0, 0, (uint16_t)rw, (uint16_t)rh };
    xcb_poly_fill_rectangle(conn, mask, gc, 1, &full);

    uint32_t fg1 = 1;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &fg1);
    xcb_arc_t arcs[4] = {
        { 0, 0, (uint16_t)dia, (uint16_t)dia, 0, 23040 },
        { (int16_t)(rw - dia - 1), 0, (uint16_t)dia, (uint16_t)dia, 0, 23040 },
        { 0, (int16_t)(rh - dia - 1), (uint16_t)dia, (uint16_t)dia, 0, 23040 },
        { (int16_t)(rw - dia - 1), (int16_t)(rh - dia - 1), (uint16_t)dia, (uint16_t)dia, 0, 23040 },
    };
    xcb_poly_fill_arc(conn, mask, gc, 4, arcs);

    xcb_rectangle_t rects[2] = {
        { (int16_t)rad, 0, (uint16_t)(rw - dia), (uint16_t)rh },
        { 0, (int16_t)rad, (uint16_t)rw, (uint16_t)(rh - dia) },
    };
    xcb_poly_fill_rectangle(conn, mask, gc, 2, rects);

    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, w, 0, 0, mask);
    xcb_free_pixmap(conn, mask);
    xcb_free_gc(conn, gc);
}

void configure_request(xcb_configure_request_event_t *e) {
    int sx = e->x, sy = e->y;
    uint16_t mask = e->value_mask;
    client *target = NULL;

    for win {
        if (c->w == e->window) {
            target = c;
            int m = c->mon;
            sx = canvas_to_screen(c->cx, canvas.pan_x[m]);
            sy = canvas_to_screen(c->cy, canvas.pan_y[m]);
            mask |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
            break;
        }
    }

    uint32_t values[7];
    int i = 0;
    if (mask & XCB_CONFIG_WINDOW_X)            values[i++] = (uint32_t)sx;
    if (mask & XCB_CONFIG_WINDOW_Y)            values[i++] = (uint32_t)sy;
    if (mask & XCB_CONFIG_WINDOW_WIDTH)      { values[i++] = e->width;  if (target) target->width = e->width; }
    if (mask & XCB_CONFIG_WINDOW_HEIGHT)     { values[i++] = e->height; if (target) target->height = e->height; }
    if (mask & XCB_CONFIG_WINDOW_SIBLING)      values[i++] = e->sibling;
    if (mask & XCB_CONFIG_WINDOW_STACK_MODE)   values[i++] = e->stack_mode;
    xcb_configure_window(conn, e->window, mask, values);

    if (target && target->titlebar) {
        unsigned int tw = 0, th = 0;
        win_size(target->w, NULL, NULL, &tw, &th);
        uint32_t tv[4] = { (uint32_t)sx, (uint32_t)(sy - TITLEBAR_HEIGHT), tw, TITLEBAR_HEIGHT };
        xcb_configure_window(conn, target->titlebar,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, tv);
        titlebar_update(target);
    }
    docks_raise();
    xcb_flush(conn);
}

static int win_is_dock(xcb_window_t w) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, w, net_wm_window_type, XCB_ATOM_ATOM, 0, 1);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, ck, NULL);
    int dock = 0;
    if (r && xcb_get_property_value_length(r) > 0) {
        xcb_atom_t t = *(xcb_atom_t *)xcb_get_property_value(r);
        dock = (t == net_wm_window_type_dock);
    }
    free(r);
    return dock;
}

static void update_struts(xcb_window_t w) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, w, net_wm_strut_partial, XCB_ATOM_CARDINAL, 0, 12);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, ck, NULL);
    if (r && xcb_get_property_value_length(r) >= (int)(4 * sizeof(uint32_t))) {
        uint32_t *s = xcb_get_property_value(r);
        strut[0] = MAX(strut[0], (int)s[0]);
        strut[1] = MAX(strut[1], (int)s[1]);
        strut[2] = MAX(strut[2], (int)s[2]);
        strut[3] = MAX(strut[3], (int)s[3]);
        free(r);
        return;
    }
    free(r);

    ck = xcb_get_property(conn, 0, w, net_wm_strut, XCB_ATOM_CARDINAL, 0, 4);
    r = xcb_get_property_reply(conn, ck, NULL);
    if (r && xcb_get_property_value_length(r) == (int)(4 * sizeof(uint32_t))) {
        uint32_t *s = xcb_get_property_value(r);
        strut[0] = MAX(strut[0], (int)s[0]);
        strut[1] = MAX(strut[1], (int)s[1]);
        strut[2] = MAX(strut[2], (int)s[2]);
        strut[3] = MAX(strut[3], (int)s[3]);
    }
    free(r);
}

void expose_event(xcb_expose_event_t *e) {
    xcb_window_t w = e->window;

    if (icons_redraw_win(w)) return;

    client *c = client_from_titlebar(w);
    if (c) { titlebar_update(c); return; }
}

void map_request(xcb_map_request_event_t *e) {
    xcb_window_t w = e->window;
 
    xcb_get_window_attributes_reply_t *wa =
        xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, w), NULL);
    if (!wa || wa->override_redirect) { free(wa); return; }
    free(wa);
 
    if (win_is_dock(w)) {
        update_struts(w);
        dock_add(w);
        xcb_map_window(conn, w);
        xcb_flush(conn);
        return;
    }
 
    for win
        if (c->w == w) { xcb_map_window(conn, w); docks_raise(); xcb_flush(conn); return; }
 
 
    xcb_window_t transient_for = XCB_NONE;
    xcb_icccm_get_wm_transient_for_reply(conn, xcb_icccm_get_wm_transient_for(conn, w), &transient_for, NULL);
 
    static xcb_atom_t skip_types[7];
    static int skip_types_inited = 0;
    if (!skip_types_inited) {
        skip_types[0] = get_atom("_NET_WM_WINDOW_TYPE_UTILITY");
        skip_types[1] = get_atom("_NET_WM_WINDOW_TYPE_SPLASH");
        skip_types[2] = get_atom("_NET_WM_WINDOW_TYPE_TOOLTIP");
        skip_types[3] = get_atom("_NET_WM_WINDOW_TYPE_NOTIFICATION");
        skip_types[4] = get_atom("_NET_WM_WINDOW_TYPE_POPUP_MENU");
        skip_types[5] = get_atom("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
        skip_types[6] = get_atom("_NET_WM_WINDOW_TYPE_MENU");
        skip_types_inited = 1;
    }
 
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, w, net_wm_window_type, XCB_ATOM_ATOM, 0, 1);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, ck, NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        xcb_atom_t t = *(xcb_atom_t *)xcb_get_property_value(r);
        for (unsigned int i = 0; i < sizeof(skip_types) / sizeof(skip_types[0]); i++) {
            if (t == skip_types[i]) {
                free(r);
                xcb_map_window(conn, w);
                docks_raise();
                xcb_flush(conn);
                return;
            }
        }
    }
    free(r);
 
    uint32_t evmask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_ENTER_WINDOW |
                       XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(conn, w, XCB_CW_EVENT_MASK, &evmask);
 
    uint32_t bw = wborder();
    xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
 
    int nx = 0, ny = 0; unsigned int nw = 0, nh = 0;
    win_size(w, &nx, &ny, &nw, &nh);
    win_add(w);
 
    client *oc = cur;
    cur = list->prev;
 
    if (transient_for != XCB_NONE) {
        int px = 0, py = 0; unsigned int pw = 0, ph = 0;
        win_size(transient_for, &px, &py, &pw, &ph);
        if (pw && ph) {
            int cx = px + ((int)pw - (int)nw) / 2;
            int cy = py + ((int)ph - (int)nh) / 2;
            client_move(cur, cx, cy);
        } else if (nx + ny == 0) {
            win_place_free(cur);
        }
    } else if (nx + ny == 0) {
        win_place_free(cur);
    }
 
 
    if (cur->titlebar) titlebar_update(cur);
 
    xcb_map_window(conn, w);
    cur = oc;
    win_focus(list->prev);
    docks_raise();
    xcb_flush(conn);
}
 
void mapping_notify(xcb_mapping_notify_event_t *e) {
    if (e->request == XCB_MAPPING_KEYBOARD || e->request == XCB_MAPPING_MODIFIER) {
        xcb_refresh_keyboard_mapping(keysyms, e);
        input_grab(root);
    }
}

void run(const Arg arg) {
    if (fork()) return;
    if (conn) close(xcb_get_file_descriptor(conn));
    setsid();
    execvp((char *)arg.com[0], (char **)arg.com);
    exit(1);
}

void quit(const Arg arg) {
	running = 0;
}

void notify_show(const char *msg, uint32_t bg) {
    if (notify_win != XCB_NONE) {
        xcb_destroy_window(conn, notify_win);
        notify_win = XCB_NONE;
    }

    int w = 260, h = 42;
    int x = sw - w - 10;
    int y = 10;

    notify_win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[] = { bg, 1 };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, notify_win, root,
                      (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    xcb_flush(conn);
    xcb_map_window(conn, notify_win);
    XftDraw *draw = XftDrawCreate(dpy, notify_win, visual, cmap);

    if (!notify_font)
        notify_font = open_font(cfg->fonts);

    if (notify_font) {
        XRenderColor xr = { .red = 65535, .green = 65535, .blue = 65535, .alpha = 65535 };
        XftColor color;
        if (XftColorAllocValue(dpy, visual, cmap, &xr, &color)) {
            XGlyphInfo ext;
            XftTextExtentsUtf8(dpy, notify_font, (const FcChar8 *)msg, (int)strlen(msg), &ext);
            int nx = MAX(0, (w - ext.xOff) / 2);
            int ny = h / 2 + (notify_font->ascent - notify_font->descent) / 2;
            XftDrawStringUtf8(draw, &color, notify_font, nx, ny, (const FcChar8 *)msg, (int)strlen(msg));
            XftColorFree(dpy, visual, cmap, &color);
        }
        XftDrawDestroy(draw);
    }

    XFlush(dpy);

    uint32_t stack = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, notify_win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);

    notify_until = now_ms() + 2000;
    xcb_flush(conn);
}

static void notify_cleanup(void) {
    if (notify_win != XCB_NONE && now_ms() >= notify_until) {
        xcb_destroy_window(conn, notify_win);
        notify_win = XCB_NONE;
        xcb_flush(conn);
    }
}

void reload_config(const Arg arg) {
    (void)arg;
    char cfgdir[256];
    snprintf(cfgdir, sizeof(cfgdir), "%s/.config/sbcwm/config.lua", get_home());

    Config *new_cfg = config_load(cfgdir);
    if (!new_cfg) {
        fprintf(stderr, "sbpcwm: failed to reload config\n");
        notify_show("Config failed", 0x202020);
        return;
    }

    Config *old_cfg = cfg;
    cfg = new_cfg;
    input_grab(root);
    if (strcmp(old_cfg->fonts, new_cfg->fonts) || strcmp(old_cfg->fontb, new_cfg->fontb))
        fonts_reload();
    config_free(old_cfg);
    icons_load_state(cfg);
    icons_rebuild();
    xcb_flush(conn);
    notify_show("Config reloaded", 0x202020);
}

void input_grab(xcb_window_t rootw) {
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlock, numlock | XCB_MOD_MASK_LOCK };

    xcb_get_modifier_mapping_reply_t *modmap =
        xcb_get_modifier_mapping_reply(conn, xcb_get_modifier_mapping(conn), NULL);
    if (modmap) {
        xcb_keycode_t *mk = xcb_get_modifier_mapping_keycodes(modmap);
        xcb_keycode_t *num_kc = xcb_key_symbols_get_keycode(keysyms, 0xff7f);
        if (num_kc) {
            for (unsigned int i = 0; i < 8; i++)
                for (int k = 0; k < modmap->keycodes_per_modifier; k++)
                    if (mk[i * modmap->keycodes_per_modifier + k] == num_kc[0])
                        numlock = (1u << i);
            free(num_kc);
        }
        free(modmap);
    }

    xcb_ungrab_key(conn, XCB_GRAB_ANY, rootw, XCB_MOD_MASK_ANY);

    for (unsigned int i = 0; i < (unsigned)cfg->nkeys; i++) {
        xcb_keycode_t *kc = xcb_key_symbols_get_keycode(keysyms, cfg->keys[i].keysym);
        if (!kc) continue;
        for (unsigned int j = 0; j < sizeof(modifiers) / sizeof(*modifiers); j++)
            xcb_grab_key(conn, 1, rootw, (uint16_t)(cfg->keys[i].mod | modifiers[j]), kc[0],
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        free(kc);
    }

    for (int i = 0; i < cfg->nshortcuts; i++) {
        xcb_keycode_t *kc = xcb_key_symbols_get_keycode(keysyms, cfg->shortcuts[i].keysym);
        if (!kc) continue;
        for (unsigned int j = 0; j < sizeof(modifiers) / sizeof(*modifiers); j++)
            xcb_grab_key(conn, 1, rootw, (uint16_t)(cfg->shortcuts[i].mod | modifiers[j]), kc[0],
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        free(kc);
    }

    for (int i = 1; i < 4; i++)
        for (unsigned int j = 0; j < sizeof(modifiers) / sizeof(*modifiers); j++)
            xcb_grab_button(conn, 1, rootw,
                             XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                             XCB_EVENT_MASK_POINTER_MOTION,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             XCB_NONE, XCB_NONE, (uint8_t)i, (uint16_t)(MOD | modifiers[j]));

    xcb_flush(conn);
}

void ws_focusnext(const Arg arg) {
    (void)arg;
    xcb_query_pointer_reply_t *ptr = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    if (!ptr) return;
    if (n_mons < 2) { free(ptr); return; }

    int cur_mon = mon_from_point(ptr->root_x, ptr->root_y);
    int next = (cur_mon + 1) % n_mons;
    xcb_warp_pointer(conn, XCB_NONE, root, 0, 0, 0, 0,
                      (int16_t)(mons[next].x + mons[next].w / 2),
                      (int16_t)(mons[next].y + mons[next].h / 2));
    titlebar_update(cur);
    free(ptr);
    xcb_flush(conn);
}

void move_nextmon(const Arg arg) {
    (void)arg;

    if (!cur) return;

    xcb_query_pointer_reply_t *ptr = xcb_query_pointer_reply(conn, xcb_query_pointer(conn, root), NULL);
    if (!ptr) return;
    if (n_mons < 2) { free(ptr); return; }
    if (cur->f) { free(ptr); return; }

    int cur_mon = mon_from_point(ptr->root_x, ptr->root_y);
    int next = (cur_mon + 1) % n_mons;
    xcb_warp_pointer(conn, XCB_NONE, root, 0, 0, 0, 0,
                      (int16_t)(mons[next].x + mons[next].w / 2),
                      (int16_t)(mons[next].y + mons[next].h / 2));

    if (cur) {
        unsigned int cw, ch;
        int cwx, cwy;
        win_size(cur->w, &cwx, &cwy, &cw, &ch);
        int new_sx = mons[next].x + (mons[next].w - (int)cw) / 2;
        int new_sy = mons[next].y + (mons[next].h - (int)ch) / 2;
        client_move(cur, new_sx, new_sy);
        titlebar_update(cur);
        set_client_monitor(cur, next);
        cur->cx = (float)new_sx + canvas.pan_x[next];
        cur->cy = (float)new_sy + canvas.pan_y[next];
    }

    free(ptr);
    xcb_flush(conn);
}

void notify_screen_change(xcb_randr_screen_change_notify_event_t *e) {
    (void)e;
    monitors_refresh();
    canvas_apply_all();
}

static void handle_xcb_event(xcb_generic_event_t *ev) {
    uint8_t type = ev->response_type & ~0x80;

    if (type == XCB_MOTION_NOTIFY) {
        xcb_generic_event_t *next;
        xcb_motion_notify_event_t *last = (xcb_motion_notify_event_t *)ev;
        while ((next = xcb_poll_for_queued_event(conn))) {
            if ((next->response_type & ~0x80) != XCB_MOTION_NOTIFY) {
                free(ev);
                handle_xcb_event(next);
                return;
            }
            free(ev);
            ev = next;
            last = (xcb_motion_notify_event_t *)ev;
        }
        notify_motion(last);
        free(ev);
        return;
    }

    switch (type) {
        case XCB_BUTTON_PRESS:      button_press((xcb_button_press_event_t *)ev); break;
        case XCB_BUTTON_RELEASE:    button_release((xcb_button_release_event_t *)ev); break;
        case XCB_CONFIGURE_NOTIFY:  configure_notify((xcb_configure_notify_event_t *)ev); break;
        case XCB_CONFIGURE_REQUEST: configure_request((xcb_configure_request_event_t *)ev); break;
        case XCB_KEY_PRESS:         key_press((xcb_key_press_event_t *)ev); break;
        case XCB_EXPOSE:            expose_event((xcb_expose_event_t *)ev); break;
        case XCB_MAP_REQUEST:       map_request((xcb_map_request_event_t *)ev); break;
        case XCB_MAP_NOTIFY:        dock_track(((xcb_map_notify_event_t *)ev)->window); break;
        case XCB_MAPPING_NOTIFY:    mapping_notify((xcb_mapping_notify_event_t *)ev); break;
        case XCB_DESTROY_NOTIFY:    notify_destroy((xcb_destroy_notify_event_t *)ev); break;
        case XCB_UNMAP_NOTIFY:      notify_unmap((xcb_unmap_notify_event_t *)ev); break;
        case XCB_ENTER_NOTIFY:      notify_enter((xcb_enter_notify_event_t *)ev); break;
        case XCB_PROPERTY_NOTIFY:   notify_property((xcb_property_notify_event_t *)ev); break;
        case XCB_FOCUS_IN:          focusin((xcb_focus_in_event_t *)ev); break;
        case XCB_CLIENT_MESSAGE:    client_message((xcb_generic_event_t *)ev); break;
        default:
            if (randr_event_base && type == randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
                notify_screen_change((xcb_randr_screen_change_notify_event_t *)ev);
            break;
    }

    free(ev);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) exit(1);

    conn = XGetXCBConnection(dpy);
    if (!conn || xcb_connection_has_error(conn)) exit(1);
    XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

    signal(SIGCHLD, SIG_IGN);

    scrno  = DefaultScreen(dpy);
    screen = xcb_aux_get_screen(conn, scrno);
    root   = screen->root;
    sw     = screen->width_in_pixels;
    sh     = screen->height_in_pixels;
    visual = DefaultVisual(dpy, scrno);
    cmap   = DefaultColormap(dpy, scrno);
    depth  = DefaultDepth(dpy, scrno);

    keysyms = xcb_key_symbols_alloc(conn);

    net_supporting_wm_check  = get_atom("_NET_SUPPORTING_WM_CHECK");
    net_wm_name              = get_atom("_NET_WM_NAME");
    net_wm_visible_name      = get_atom("_NET_WM_VISIBLE_NAME");
    net_active_window        = get_atom("_NET_ACTIVE_WINDOW");
    ewmh_utf8_string         = get_atom("UTF8_STRING");
    net_supported            = get_atom("_NET_SUPPORTED");
    net_wm_window_type       = get_atom("_NET_WM_WINDOW_TYPE");
    net_wm_window_type_dock  = get_atom("_NET_WM_WINDOW_TYPE_DOCK");
    net_wm_strut             = get_atom("_NET_WM_STRUT");
    net_wm_strut_partial     = get_atom("_NET_WM_STRUT_PARTIAL");
    net_current_desktop      = get_atom("_NET_CURRENT_DESKTOP");
    net_wm_state             = get_atom("_NET_WM_STATE");
    net_wm_state_fullscreen  = get_atom("_NET_WM_STATE_FULLSCREEN");
    wm_delete_window         = get_atom("WM_DELETE_WINDOW");
    wm_protocols             = get_atom("WM_PROTOCOLS");
    wm_normal_hints_atom     = XCB_ATOM_WM_NORMAL_HINTS;
    net_client_list_stacking = get_atom("_NET_CLIENT_LIST_STACKING");

    canvas_atom_pan_x = get_atom("_CANVAS_PAN_X");
    canvas_atom_pan_y = get_atom("_CANVAS_PAN_Y");
    sbcwm_atom_monitor = get_atom("_SBCWM_MONITOR");

    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        xcb_randr_query_version_cookie_t vck = xcb_randr_query_version(conn, 1, 5);
        xcb_randr_query_version_reply_t *vr = xcb_randr_query_version_reply(conn, vck, NULL);
        free(vr);

        randr_event_base = randr_ext->first_event;
        xcb_randr_select_input(conn, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }

    monitors_refresh();
    canvas_sync_to_root();

    xcb_query_tree_cookie_t dck = xcb_query_tree(conn, root);
    xcb_query_tree_reply_t *dtr = xcb_query_tree_reply(conn, dck, NULL);
    if (dtr) {
        xcb_window_t *ch = xcb_query_tree_children(dtr);
        for (int i = 0; i < xcb_query_tree_children_length(dtr); i++)
            dock_track(ch[i]);
        free(dtr);
    }

    xcb_window_t wmcheck = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, wmcheck, root, 0, 0, 1, 1, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_supporting_wm_check,
                         XCB_ATOM_WINDOW, 32, 1, &wmcheck);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wmcheck, net_supporting_wm_check,
                         XCB_ATOM_WINDOW, 32, 1, &wmcheck);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wmcheck, net_wm_name,
                         ewmh_utf8_string, 8, 6, "sbpcwm");
 
    xcb_atom_t supported[] = {
        net_supporting_wm_check, net_wm_name, net_wm_window_type,
        net_wm_window_type_dock, net_wm_strut, net_wm_strut_partial,
        net_current_desktop, net_client_list_stacking, net_wm_state,
    };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_supported,
                         XCB_ATOM_ATOM, 32, sizeof(supported) / sizeof(*supported), supported);

    uint32_t cur_ws = 0;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, net_current_desktop,
                         XCB_ATOM_CARDINAL, 32, 1, &cur_ws);

    uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                         XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                         XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION;
    xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, &root_mask);

    xcb_font_t cursor_font = xcb_generate_id(conn);
    xcb_open_font(conn, cursor_font, strlen("cursor"), "cursor");
    xcb_cursor_t cursor = xcb_generate_id(conn);
    xcb_create_glyph_cursor(conn, cursor, cursor_font, cursor_font, 68, 68 + 1,
                             0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_change_window_attributes(conn, root, XCB_CW_CURSOR, &cursor);
    xcb_close_font(conn, cursor_font);


    char cfgdir[256];

    snprintf(cfgdir, sizeof(cfgdir), "%s/.config/sbcwm/config.lua", get_home());

    cfg = config_load(cfgdir);

    if (!cfg) {
	fprintf(stderr, "sbpcwm: failed to load config, exiting\n");
	exit(1);
    }

    load_colors();

    input_grab(root);

    icons_load_state(cfg);
    if (cfg->ui) icons_rebuild();

    xcb_flush(conn);

    ctl_init();

    physics_init();

    int xfd = xcb_get_file_descriptor(conn);
    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd = xfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = ctl_fd();
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, pfds[1].fd >= 0 ? 2 : 1, 16);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfds[1].fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP)))
            ctl_accept();

        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_event(conn)))
                handle_xcb_event(ev);
            if (xcb_connection_has_error(conn)) break;
        }

        physics_tick();
        notify_cleanup();
    }

    ctl_cleanup();

    return 0;
}
