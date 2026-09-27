#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "sbcct.h"
#include "sbpcwm.h"
#include "ctl.h"
#include "icons.h"
#include "sbcs.h"

typedef enum { PAGE_OPTS, PAGE_KEYS, PAGE_ICONS, PAGE_CTX } SbcsPageKind;

typedef struct {
    const char *title;
    SbcsPageKind kind;
    const char *opt[SBCS_MAX_OPTS];
} SbcsPage;

static const SbcsPage pages[] = {
    { "System",         PAGE_OPTS, { "defaultsh", "fonts", "fontb", NULL } },
    { "Window Manager", PAGE_OPTS, { "pan_step", "titlebar", "focus_follow",
                                      "border", "border_width", NULL } },
    { "Appearance",     PAGE_OPTS, { "ctxbg", "ctxborder", "deco", NULL } },
    { "Keybinds",       PAGE_KEYS, { NULL } },
    { "Icons",          PAGE_ICONS, { NULL } },
    { "Context Menu",   PAGE_CTX,   { NULL } },
    { "Other",          PAGE_OPTS, { NULL } },
};

xcb_window_t sbcs_win = XCB_NONE;
static Pixmap sbcs_buf;
static GC      sbcs_gc;
static xcb_gcontext_t sbcs_cgx;
static int sbcs_x, sbcs_y;
static int sbcs_w = SBCS_W, sbcs_h = SBCS_H;

enum { BLIT_COPY, BLIT_PUT, BLIT_DIRECT };
static int sbcs_blit = BLIT_COPY;

static int   sbcs_page;
static int   edit_row = -1;
static int   edit_col;
static int   edit_grab;
static int   row_off;
static int   nav_off;
static char  edit_buf[SBCS_VAL_LEN];
static int   edit_len, edit_pos;
static XftFont *edit_font;
static XftFont *ui_font;
static char    *ui_font_name;
static Cursor cur_normal, cur_text, cur_hand, last_cursor;

static int n_pages(void) { return (int)(sizeof pages / sizeof *pages); }

static int page_opts(int p) {
    if (pages[p].kind != PAGE_OPTS) return 0;
    int n = 0;
    while (n < SBCS_MAX_OPTS && pages[p].opt[n]) n++;
    return n;
}

static int opt_listed(const char *name) {
    for (int p = 0; p < n_pages() - 1; p++)
        for (int i = 0; i < page_opts(p); i++)
            if (!strcmp(pages[p].opt[i], name)) return 1;
    return 0;
}

static const CtlOpt *page_opt(int p, int i) {
    if (pages[p].kind != PAGE_OPTS) return NULL;
    if (pages[p].opt[0]) {
        if (i >= page_opts(p)) return NULL;
        return ctl_find_opt(pages[p].opt[i]);
    }

    size_t n = 0;
    const CtlOpt *list = ctl_opt_list(&n);
    for (size_t k = 0; k < n; k++)
        if (!opt_listed(list[k].name)) {
            if (i == 0) return &list[k];
            i--;
        }
    return NULL;
}

static int page_rows(int p) {
    if (pages[p].kind == PAGE_KEYS)  return sbcs_nkeys;
    if (pages[p].kind == PAGE_ICONS) return sbcs_nicons;
    if (pages[p].kind == PAGE_CTX)   return sbcs_nctx;
    if (pages[p].opt[0]) return page_opts(p);

    size_t n = 0;
    const CtlOpt *list = ctl_opt_list(&n);
    int rows = 0;
    for (size_t k = 0; k < n; k++)
        if (!opt_listed(list[k].name)) rows++;
    return rows;
}

static int is_list_page(int p) { return pages[p].kind != PAGE_OPTS; }
static int current_kind(void) { return pages[sbcs_page].kind; }

static void ui_font_sync(void) {
    const char *want = cfg ? cfg->fonts : NULL;
    if (ui_font && want && ui_font_name && !strcmp(want, ui_font_name)) return;
    if (ui_font) { XftFontClose(dpy, ui_font); ui_font = NULL; }
    free(ui_font_name);
    ui_font_name = NULL;
    if (want) {
        ui_font = open_font(want);
        if (ui_font) ui_font_name = copystr(want);
    }
}

static int font_lh(void) {
    ui_font_sync();
    int lh = ui_font ? ui_font->ascent + ui_font->descent : 14;
    return CLAMP(lh, 8, 40);
}

static int pad(void)      { return CLAMP(sbcs_w / 50, 8, 30); }
static int nav_w(void)    { return CLAMP(sbcs_w * 28 / 100, 104, 240); }
static int foot_h(void)   { return CLAMP(sbcs_h / 18, 20, 46); }
static int content_w(void){ return sbcs_w - nav_w() - 2 * pad(); }
static int content_h(void){ return sbcs_h - foot_h() - pad(); }

static int content_top(void) { return pad(); }
static int content_bot(void) { return sbcs_h - foot_h(); }
static int nav_bot(void)     { return sbcs_h - foot_h(); }
static int row_h(void)  { return CLAMP(content_h() / 15, font_lh() + 6, 40); }
static int item_h(void) { return CLAMP(content_h() / 16, font_lh() + 8, 40); }

static int field_h(void) { return CLAMP(row_h() - 6, font_lh() + 2, row_h() - 4); }
static int chip_h(void)  { return CLAMP(field_h() - 2, 12, 26); }
static int thumb(void)   { return CLAMP(field_h() - 4, 10, 30); }
static int box_sz(void)  { return CLAMP(field_h() - 4, 10, 24); }

static int row_y(int i)     { return content_top() + i * row_h(); }

static int opts_label_w = 0;

static int field_w(void) {
    if (sbcs_page >= 0 && pages[sbcs_page].kind == PAGE_OPTS && opts_label_w > 0) {
        int w = sbcs_w - pad() - (nav_w() + pad() + opts_label_w) - 10;
        return CLAMP(w, 100, 520);
    }
    return CLAMP(content_w() * 55 / 100, 100, 520);
}
static int field_x(void)    { return sbcs_w - pad() - field_w(); }
static int name_x(void)     { return nav_w() + pad(); }
static int name_maxw(void)  {
    int w = field_x() - 10 - name_x();
    if (sbcs_page >= 0 && pages[sbcs_page].kind == PAGE_OPTS && opts_label_w > 0 && w < opts_label_w)
        w = opts_label_w;
    return w > 40 ? w : 40;
}
static int box_x(void)      { return sbcs_w - pad() - box_sz(); }
static int vis_rows(void)   { return (content_bot() - content_top()) / row_h(); }
static int nav_vis(void)    { return (nav_bot() - content_top()) / item_h(); }

static int add_row_y(void) { return row_y(0); }
static int item_row_y(int i) { return row_y(i + 1 - row_off); }
static int opt_row_y(int i)  { return row_y(i - row_off); }

static int nav_max_off(void) {
    int over = n_pages() - nav_vis();
    return over > 0 ? over : 0;
}
static void nav_clamp(void) {
    if (nav_off > nav_max_off()) nav_off = nav_max_off();
    if (nav_off < 0) nav_off = 0;
}
static void nav_scroll_to(int i) {
    if (i < nav_off) nav_off = i;
    else if (i >= nav_off + nav_vis()) nav_off = i - nav_vis() + 1;
    nav_clamp();
}

static int key_w(void)   { return CLAMP(content_w() * 22 / 100, 56, 150); }
static int del_w(void)   { return CLAMP(row_h() / 2, 16, 24); }
static int del_box(void) { return CLAMP(row_h() - 12, 10, 20); }
static int del_x(void)   { return sbcs_w - pad() - del_w(); }

static int key_field_x(void) { return name_x(); }
static int chip_area_x(void) { return key_field_x() + key_w() + 12; }
static int chip_area_w(void) {
    int w = del_x() - 6 - chip_area_x() - 40;
    return w > 120 ? w : 120;
}
static int chip_w(void)  { return CLAMP(chip_area_w() * 55 / 100 / 4 - 4, 34, 90); }
static int chip_x(int c) { return chip_area_x() + c * (chip_w() + 4); }
static int key_cmd_x(void) { return chip_x(0) + 4 * (chip_w() + 4) + 14; }
static int key_cmd_w(void) { int w = del_x() - 6 - key_cmd_x(); return w > 40 ? w : 40; }

static int icon_name_x(void) { return name_x(); }
static int icon_name_w(void) { return CLAMP(content_w() * 24 / 100, 64, 190); }
static int icon_img_x(void)  { return icon_name_x() + icon_name_w() + 10; }
static int icon_img_w(void)  { return CLAMP((del_x() - 34 - icon_img_x()) * 48 / 100,
                                            90, 460); }
static int icon_cmd_x(void)  { return icon_img_x() + icon_img_w() + 20; }
static int icon_cmd_w(void)  { int w = del_x() - 6 - icon_cmd_x(); return w > 40 ? w : 40; }

static int ctx_lbl_x(void) { return name_x(); }
static int ctx_lbl_w(void) { return CLAMP(content_w() * 28 / 100, 64, 200); }
static int ctx_fn_x(void)  { return ctx_lbl_x() + ctx_lbl_w() + 10; }
static int ctx_fn_w(void)  { return CLAMP((del_x() - 40 - ctx_fn_x()) * 34 / 100,
                                          80, 200); }
static int ctx_arg_x(void) { return ctx_fn_x() + ctx_fn_w() + 10; }
static int ctx_arg_w(void) { int w = del_x() - 6 - ctx_arg_x(); return w > 40 ? w : 40; }

static int row_at(int y) {
    if (y < content_top() || y >= content_bot()) return -1;
    int i = (y - content_top()) / row_h() + row_off;
    return i < page_rows(sbcs_page) ? i : -1;
}

static int list_item_at(int y) {
    if (y < content_top() || y >= content_bot()) return -1;
    int r = (y - content_top()) / row_h();
    if (r < 1) return -1;
    int i = r - 1 + row_off;
    return i < page_rows(sbcs_page) ? i : -1;
}

static int page_visible(void) { return is_list_page(sbcs_page) ? vis_rows() - 1 : vis_rows(); }
static int page_max_off(void) {
    int over = page_rows(sbcs_page) - page_visible();
    return over > 0 ? over : 0;
}
static void page_clamp(void) {
    if (row_off > page_max_off()) row_off = page_max_off();
    if (row_off < 0) row_off = 0;
    nav_clamp();
}
static void page_scroll_to(int i) {
    if (i < row_off) row_off = i;
    else if (i >= row_off + page_visible()) row_off = i - page_visible() + 1;
    page_clamp();
}

static int item_at(int y) {
    if (y < content_top() || y >= nav_bot()) return -1;
    int i = (y - content_top()) / item_h() + nav_off;
    return i < n_pages() ? i : -1;
}

static unsigned long panel_bg(void) {
    return cols.background;
}

static unsigned long panel_bd(void) {
    return cols.foreground;
}

static unsigned long lighten(unsigned long c, int percent) {
    unsigned r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    r = (unsigned)CLAMP((int)r * percent / 100, 0, 255);
    g = (unsigned)CLAMP((int)g * percent / 100, 0, 255);
    b = (unsigned)CLAMP((int)b * percent / 100, 0, 255);
    return (r << 16) | (g << 8) | b;
}

static int bg_is_light(void) {
    unsigned long bg = panel_bg();
    int lum = (int)(((bg >> 16) & 0xff) * 30 + ((bg >> 8) & 0xff) * 59 +
                    (bg & 0xff) * 11) / 100;
    return lum > 140;
}

static void text_shade(int percent, XftColor *out) {
    XRenderColor rc;
    unsigned short v = (unsigned short)(0xffff * CLAMP(percent, 0, 100) / 100);
    if (bg_is_light()) v = (unsigned short)(0xffff - v);
    rc.red = rc.green = rc.blue = v;
    rc.alpha = 0xffff;
    XftColorAllocValue(dpy, visual, cmap, &rc, out);
}

static int text_wn(XftFont *f, const char *s, int len) {
    if (!f || !s || len <= 0) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, len, &ext);
    return (int)ext.xOff;
}

static int text_w(XftFont *f, const char *s) {
    return text_wn(f, s, (int)strlen(s));
}

static int utf8_trim(const char *s, int len) {
    while (len > 0 && ((unsigned char)s[len] & 0xc0) == 0x80) len--;
    return len;
}

static void fit_text(XftFont *f, const char *s, int maxw, char *out, size_t n) {
    if (!s) s = "";
    int len = (int)strlen(s);
    if (len < (int)n && text_wn(f, s, len) <= maxw) {
        snprintf(out, n, "%s", s);
        return;
    }
    int ew = text_w(f, "...");
    while (len > 0 && text_wn(f, s, len) + ew > maxw)
        len = utf8_trim(s, len - 1);
    if (len < 0) len = 0;
    if ((size_t)len > n - 4) len = (int)n - 4;
    memcpy(out, s, (size_t)len);
    memcpy(out + len, "...", 4);
}

static void draw_text(XftDraw *d, const XftColor *c, XftFont *f, int x, int y,
                      const char *s) {
    XftDrawStringUtf8(d, c, f, x, y, (const FcChar8 *)s, (int)strlen(s));
}

static int base_y(XftFont *f, int y, int h) {
    if (!f) return y + h / 2;
    return y + (h + f->ascent - f->descent) / 2;
}

static const char *row_text(int i, char *out, size_t n) {
    const CtlOpt *o = page_opt(sbcs_page, i);
    if (!o) { if (n) out[0] = 0; return out; }
    if (i == edit_row) { snprintf(out, n, "%s", edit_buf); return out; }
    ctl_opt_get(o, out, n);
    if (!out[0]) snprintf(out, n, "(unset)");
    return out;
}

static int list_cols(void) { return current_kind() == PAGE_KEYS ? 2 : 3; }

static char *field_ptr(int row, int col) {
    if (current_kind() == PAGE_KEYS)
        return col == 0 ? sbcs_keyrows[row].key : sbcs_keyrows[row].cmd;
    if (current_kind() == PAGE_CTX) {
        if (col == 0) return sbcs_ctxrows[row].label;
        if (col == 1) return sbcs_ctxrows[row].func;
        return sbcs_ctxrows[row].arg;
    }
    if (col == 0) return sbcs_iconrows[row].name;
    if (col == 1) return sbcs_iconrows[row].image;
    return sbcs_iconrows[row].cmd;
}

static size_t field_cap(int col) {
    if (current_kind() == PAGE_KEYS)
        return col == 0 ? sizeof sbcs_keyrows[0].key : sizeof sbcs_keyrows[0].cmd;
    if (current_kind() == PAGE_CTX) {
        if (col == 0) return sizeof sbcs_ctxrows[0].label;
        if (col == 1) return sizeof sbcs_ctxrows[0].func;
        return sizeof sbcs_ctxrows[0].arg;
    }
    if (col == 0) return sizeof sbcs_iconrows[0].name;
    if (col == 1) return sizeof sbcs_iconrows[0].image;
    return sizeof sbcs_iconrows[0].cmd;
}

static int field_px(int row, int col) {
    if (!is_list_page(sbcs_page)) return field_x();
    if (current_kind() == PAGE_KEYS)
        return col == 0 ? key_field_x() : key_cmd_x();
    if (current_kind() == PAGE_CTX) {
        if (col == 0) return ctx_lbl_x();
        if (col == 1) return ctx_fn_x();
        return ctx_arg_x();
    }
    if (col == 0) return icon_name_x();
    if (col == 1) return icon_img_x();
    return icon_cmd_x();
}

static int field_pw(int col) {
    if (!is_list_page(sbcs_page)) return field_w();
    if (current_kind() == PAGE_KEYS)
        return col == 0 ? key_w() : key_cmd_w();
    if (current_kind() == PAGE_CTX) {
        if (col == 0) return ctx_lbl_w();
        if (col == 1) return ctx_fn_w();
        return ctx_arg_w();
    }
    if (col == 0) return icon_name_w();
    if (col == 1) return icon_img_w();
    return icon_cmd_w();
}

static const char *field_text(int row, int col, char *out, size_t n) {
    if (!is_list_page(sbcs_page)) return row_text(row, out, n);
    if (row == edit_row && col == edit_col) {
        snprintf(out, n, "%s", edit_buf);
        return out;
    }
    const char *p = field_ptr(row, col);
    snprintf(out, n, "%.*s", 255, p ? p : "");
    if (!out[0]) snprintf(out, n, "%s",
                          col == 1 && current_kind() == PAGE_ICONS
                              ? "(png file)" : "(unset)");
    return out;
}

static XftFont *editor_font(void) {
    if (!edit_font && cfg) edit_font = open_font(cfg->fonts);
    return edit_font;
}

static void edit_scroll(void) {
    XftFont *f = editor_font();
    int inner = (is_list_page(sbcs_page) ? field_pw(edit_col) : field_w()) - 12;

    while (edit_pos > edit_len) edit_pos--;
    while (edit_pos < 0) edit_pos++;
    if (!f || text_wn(f, edit_buf, edit_pos) <= inner) return;

    int start = 0;
    while (start < edit_pos && text_wn(f, edit_buf + start, edit_pos - start) > inner)
        start++;
    if (start > edit_len) start = edit_len;

    edit_pos -= start;
    memmove(edit_buf, edit_buf + start, (size_t)(edit_len - start));
    edit_len -= start;
    edit_buf[edit_len] = 0;
}

static Drawable target(void) {
    return (sbcs_buf && sbcs_blit != BLIT_DIRECT) ? (Drawable)sbcs_buf
                                                   : (Drawable)sbcs_win;
}

static void fill(GC gc, unsigned long col, int x, int y, int w, int h) {
    XSetForeground(dpy, gc, (unsigned long)col);
    XFillRectangle(dpy, target(), gc, x, y, (unsigned)w, (unsigned)h);
}

static void frame(GC gc, unsigned long col, int x, int y, int w, int h) {
    XSetForeground(dpy, gc, (unsigned long)col);
    XDrawRectangle(dpy, target(), gc, x, y, (unsigned)w, (unsigned)h);
}

static void blit_now(void);

static void draw(void) {
    if (sbcs_win == XCB_NONE || !cfg) return;

    ui_font_sync();
    XftFont *fr = ui_font;

    int ih = item_h(), rh = row_h(), fh = field_h();
    int pw = pad(), nw = nav_w(), fh_foot = foot_h();

    opts_label_w = 0;
    if (sbcs_page >= 0 && pages[sbcs_page].kind == PAGE_OPTS) {
        for (int i = 0; i < page_rows(sbcs_page); i++) {
            const CtlOpt *o = page_opt(sbcs_page, i);
            if (!o) continue;
            int w = text_w(fr, ctl_opt_label(o)) + 6;
            if (w > opts_label_w) opts_label_w = w;
        }
    }

    unsigned long bg  = panel_bg();
    unsigned long bd  = panel_bd();
    unsigned long nav = lighten(bg, bg_is_light() ? 92 : 130);
    unsigned long fld = lighten(bg, bg_is_light() ? 100 : 175);
    unsigned long edg = lighten(bg, bg_is_light() ? 70 : 220);

    GC gc = sbcs_gc ? sbcs_gc : XCreateGC(dpy, target(), 0, NULL);
    int own_gc = sbcs_gc == NULL;

    fill(gc, bg, 0, 0, sbcs_w, sbcs_h);
    fill(gc, nav, 0, 0, nw, sbcs_h - fh_foot);

    XftDraw *d = XftDrawCreate(dpy, target(), visual, cmap);

    if (d) {
        XftColor c_item, c_sel, c_name, c_value, c_foot, c_dim, c_mark;
        text_shade(80,  &c_item);
        text_shade(100, &c_sel);
        text_shade(78,  &c_name);
        text_shade(100, &c_value);
        text_shade(48,  &c_foot);
        text_shade(60,  &c_dim);
        text_shade(100, &c_mark);

        char buf[SBCS_VAL_LEN], val[SBCS_VAL_LEN];

        int nav_top = content_top();
        int vis_nav = nav_vis();
        for (int p = nav_off; p < n_pages() && p < nav_off + vis_nav; p++) {
            int y = nav_top + (p - nav_off) * ih;
            int sel = (p == sbcs_page);
            if (sel) fill(gc, bd, 1, y + 1, nw - 2, ih - 2);
            fit_text(fr, pages[p].title, nw - 2 * pw, buf, sizeof buf);
            draw_text(d, sel ? &c_sel : &c_item, fr, pw,
                      base_y(fr, y, ih), buf);
        }
        if (nav_max_off() > 0) {
            int track = sbcs_h - fh_foot - 2 * nav_top;
            int span = track * vis_nav / n_pages();
            span = MAX(span, 12);
            int top = nav_top + (track - span) * nav_off / nav_max_off();
            fill(gc, edg, nw - 4, top, 2, span);
        }

        if (is_list_page(sbcs_page)) {
            int keys = current_kind() == PAGE_KEYS;
            int cts  = current_kind() == PAGE_CTX;
            int n = page_rows(sbcs_page);

            int ay = add_row_y();
            fill(gc, lighten(bg, bg_is_light() ? 96 : 150), name_x(), ay,
                 sbcs_w - name_x() - pw, rh - 4);
            frame(gc, edg, name_x(), ay, sbcs_w - name_x() - pw, rh - 4);
            draw_text(d, &c_value, fr, name_x() + 8, base_y(fr, ay, rh - 4),
                      keys ? "+ Add new keybind" :
                      cts  ? "+ Add new menu entry" : "+ Add new icon");

            for (int i = 0; i < n; i++) {
                int y = item_row_y(i);
                if (y < content_top() || y >= content_bot()) continue;
                if (y + rh > content_bot()) break;

                for (int c = 0; c < list_cols(); c++) {
                    int fx = field_px(i, c), fw = field_pw(c);
                    int fy = y + (rh - fh) / 2;
                    int on = (i == edit_row && c == edit_col);

                    fill(gc, on ? lighten(fld, 108) : fld, fx, fy, fw, fh);
                    frame(gc, on ? bd : edg, fx, fy, fw, fh);

                    field_text(i, c, val, sizeof val);
                    fit_text(fr, val, fw - 12, buf, sizeof buf);
                    draw_text(d, &c_value, fr, fx + 6, base_y(fr, fy, fh), buf);
                }

                if (keys) {

                    unsigned mods = sbcs_keyrows[i].mods;
                    static const char *labels[4] = { "MOD", "ALT", "CTRL", "SHIFT" };
                    static const unsigned bits[4] = { SBCS_MOD_MOD, SBCS_MOD_ALT,
                                                       SBCS_MOD_CTRL, SBCS_MOD_SHIFT };
                    draw_text(d, &c_dim, fr, key_field_x() + key_w() + 3,
                              base_y(fr, y, rh), "+");
                    for (int c = 0; c < 4; c++) {
                        int cx = chip_x(c);
                        int ch = chip_h();
                        int cy = y + (rh - ch) / 2;
                        int on = mods & bits[c];
                        fill(gc, on ? bd : lighten(bg, bg_is_light() ? 98 : 145), cx,
                             cy, chip_w(), ch);
                        frame(gc, on ? bd : edg, cx, cy, chip_w(), ch);
                        fit_text(fr, labels[c], chip_w() - 8, buf, sizeof buf);
                        draw_text(d, on ? &c_mark : &c_dim, fr,
                                  cx + (chip_w() - text_w(fr, buf)) / 2,
                                  base_y(fr, cy, ch), buf);
                    }
                    draw_text(d, &c_dim, fr, key_cmd_x() - 10,
                              base_y(fr, y, rh), "=");
                } else if (cts) {
                    draw_text(d, &c_dim, fr, ctx_fn_x() - 16,
                              base_y(fr, y, rh), ",");
                    draw_text(d, &c_dim, fr, ctx_arg_x() - 12,
                              base_y(fr, y, rh), "=");
                } else {
                    int ts = thumb();
                    int tx = icon_img_x() - ts - 8;
                    int ty = y + (rh - ts) / 2;
                    draw_text(d, &c_dim, fr, icon_img_x() - 16,
                              base_y(fr, y, rh), ",");
                    if (sbcs_iconrows[i].image[0])
                        icons_draw_thumb(target(), sbcs_iconrows[i].image, tx, ty,
                                         ts, ts, fld);
                    draw_text(d, &c_dim, fr, icon_cmd_x() - 12,
                              base_y(fr, y, rh), "=");
                }

                int db = del_box();
                int dx = del_x() + (del_w() - db) / 2;
                int dy = y + (rh - db) / 2;
                frame(gc, edg, dx, dy, db, db);
                draw_text(d, &c_dim, fr, dx + db / 2 - text_w(fr, "x") / 2,
                          base_y(fr, dy, db), "x");
            }
        } else {

            for (int i = 0; i < page_rows(sbcs_page); i++) {
                const CtlOpt *o = page_opt(sbcs_page, i);
                if (!o) continue;
                int y = opt_row_y(i);
                if (y < content_top() || y >= content_bot()) continue;

                fit_text(fr, ctl_opt_label(o), name_maxw(), buf, sizeof buf);
                draw_text(d, &c_name, fr, name_x(), base_y(fr, y, rh), buf);

                if (o->type == CTL_OPT_BOOL) {
                    char s[SBCS_VAL_LEN];
                    ctl_opt_get(o, s, sizeof s);
                    int on = atoi(s) != 0;
                    int bs = box_sz();
                    int bx = box_x(), by = y + (rh - bs) / 2;

                    frame(gc, edg, bx, by, bs, bs);
                    if (on) {
                        fill(gc, bd, bx + 3, by + 3, bs - 6, bs - 6);
                        draw_text(d, &c_mark, fr,
                                  bx + bs / 2 - text_w(fr, "x") / 2,
                                  base_y(fr, y, rh), "x");
                    }
                    const char *word = on ? "on" : "off";
                    draw_text(d, &c_dim, fr, bx - 6 - text_w(fr, word),
                              base_y(fr, y, rh), word);
                    continue;
                }

                int fw = field_w();
                int fx = field_x(), fy = y + (rh - fh) / 2;
                fill(gc, fld, fx, fy, fw, fh);
                frame(gc, i == edit_row ? bd : edg, fx, fy, fw, fh);

                row_text(i, val, sizeof val);
                fit_text(fr, val, fw - 12, buf, sizeof buf);
                draw_text(d, &c_value, fr, fx + 6, base_y(fr, fy, fh), buf);
            }
        }

        if (edit_row >= 0) {
            int i = edit_row;
            int fw = is_list_page(sbcs_page) ? field_pw(edit_col) : field_w();
            int caret = text_wn(fr, edit_buf, edit_pos);
            if (caret < fw - 12) {
                int fx = is_list_page(sbcs_page) ? field_px(i, edit_col) : field_x();
                int fy = is_list_page(sbcs_page) ? item_row_y(i) : opt_row_y(i);
                fy += (rh - fh) / 2;
                XSetForeground(dpy, gc, (unsigned long)lighten(bg, bg_is_light() ? 30 : 240));
                XFillRectangle(dpy, target(), gc, fx + 6 + caret, fy + 3,
                               2, (unsigned)(fh - 6));
            }
        }

        char range[64] = "";
        if (row_off > 0 || page_max_off() > 0) {
            int first = row_off + 1;
            int last = row_off + page_visible();
            int total = page_rows(sbcs_page);
            snprintf(range, sizeof range, "    showing %d-%d of %d", first,
                     last < total ? last : total, total);
        }

        const char *hint =
            edit_row >= 0 ? "Enter apply    Esc cancel    Tab next field"
            : current_kind() == PAGE_KEYS
                ? "command: a wm function (win_fs, win_kill, run ...) or a shell command"
            : current_kind() == PAGE_CTX
                ? "func: a wm function (run, win_kill, toggle_icons ...)    arg: its arguments"
            : is_list_page(sbcs_page)
                ? "command to spawn: a shell command, or a wm function name"
                : "changes are written to ~/.config/sbcwm/config.lua";
        char line[SBCS_VAL_LEN];
        snprintf(line, sizeof line, "%s%s", hint, range);
        fit_text(fr, line, sbcs_w - name_x() - pw - 8, buf, sizeof buf);
        draw_text(d, &c_foot, fr, name_x(),
                  base_y(fr, sbcs_h - fh_foot, fh_foot), buf);

        XftColorFree(dpy, visual, cmap, &c_item);
        XftColorFree(dpy, visual, cmap, &c_sel);
        XftColorFree(dpy, visual, cmap, &c_name);
        XftColorFree(dpy, visual, cmap, &c_value);
        XftColorFree(dpy, visual, cmap, &c_foot);
        XftColorFree(dpy, visual, cmap, &c_dim);
        XftColorFree(dpy, visual, cmap, &c_mark);
        XftDrawDestroy(d);
    }

    if (own_gc) XFreeGC(dpy, gc);

    blit_now();
    XFlush(dpy);
}

static unsigned long pixel_of(XImage *im) {
    if (!im) return 0;
    unsigned long p = (unsigned long)XGetPixel(im, 0, 0);
    XDestroyImage(im);

    return p & (visual->red_mask | visual->green_mask | visual->blue_mask);
}

static void conn_sync(void) {
    free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
}

static void blit_now(void) {
    if (sbcs_blit == BLIT_COPY) {
        XSync(dpy, False);
        xcb_copy_area(conn, sbcs_buf, sbcs_win, sbcs_cgx, 0, 0, 0, 0,
                      (unsigned)sbcs_w, (unsigned)sbcs_h);
        xcb_flush(conn);
        return;
    }
    if (sbcs_blit == BLIT_PUT) {
        XImage *img = XGetImage(dpy, sbcs_buf, 0, 0, (unsigned)sbcs_w,
                                (unsigned)sbcs_h, AllPlanes, ZPixmap);
        if (img) {
            XPutImage(dpy, sbcs_win, sbcs_gc, img, 0, 0, 0, 0,
                      (unsigned)sbcs_w, (unsigned)sbcs_h);
            XDestroyImage(img);
            return;
        }
        sbcs_blit = BLIT_DIRECT;
        draw();
    }
}

static void blit_probe(void) {
    sbcs_blit = BLIT_DIRECT;
    if (!sbcs_buf || !sbcs_gc) return;

    unsigned long want = ((unsigned long)0xff00ff & (visual->red_mask |
                             visual->green_mask | visual->blue_mask));
    XSetForeground(dpy, sbcs_gc, want);
    XFillRectangle(dpy, sbcs_buf, sbcs_gc, 0, 0, 1, 1);

    xcb_copy_area(conn, sbcs_buf, sbcs_win, sbcs_cgx, 0, 0, 0, 0, 1, 1);
    xcb_flush(conn);
    XSync(dpy, False);
    conn_sync();
    if (pixel_of(XGetImage(dpy, sbcs_win, 0, 0, 1, 1, AllPlanes, ZPixmap)) == want) {
        sbcs_blit = BLIT_COPY;
        return;
    }

    XImage *img = XGetImage(dpy, sbcs_buf, 0, 0, 1, 1, AllPlanes, ZPixmap);
    if (!img) return;
    XPutImage(dpy, sbcs_win, sbcs_gc, img, 0, 0, 0, 0, 1, 1);
    XDestroyImage(img);
    XSync(dpy, False);
    if (pixel_of(XGetImage(dpy, sbcs_win, 0, 0, 1, 1, AllPlanes, ZPixmap)) == want)
        sbcs_blit = BLIT_PUT;
}

static void buffer_free(void);

static void buffer_create(void) {
    if (sbcs_buf) return;
    sbcs_buf = XCreatePixmap(dpy, (Drawable)sbcs_win, (unsigned)sbcs_w,
                             (unsigned)sbcs_h, (unsigned)depth);
    sbcs_gc  = sbcs_buf ? XCreateGC(dpy, sbcs_buf, 0, NULL) : NULL;
    if (sbcs_buf && sbcs_gc) {
        sbcs_cgx = xcb_generate_id(conn);
        xcb_create_gc(conn, sbcs_cgx, sbcs_buf, 0, NULL);
        xcb_flush(conn);
        blit_probe();
    }
    XFlush(dpy);
}

static void buffer_resize(void) {
    if (!sbcs_buf) { buffer_create(); return; }
    buffer_free();
    buffer_create();
}

static void buffer_free(void) {
    if (sbcs_cgx) { xcb_free_gc(conn, sbcs_cgx); sbcs_cgx = 0; }
    if (sbcs_gc) { XFreeGC(dpy, sbcs_gc); sbcs_gc = NULL; }
    if (sbcs_buf) { XFreePixmap(dpy, sbcs_buf); sbcs_buf = 0; }
    sbcs_blit = BLIT_COPY;
    XFlush(dpy);
}

static void edit_take_keyboard(void) {
    if (edit_grab || sbcs_win == XCB_NONE) return;
    xcb_grab_keyboard_reply_t *g =
        xcb_grab_keyboard_reply(conn,
                                xcb_grab_keyboard(conn, 0, sbcs_win, XCB_CURRENT_TIME,
                                                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
                                NULL);
    if (g) {
        edit_grab = g->status == XCB_GRAB_STATUS_SUCCESS;
        free(g);
    }
}

static void edit_drop_keyboard(void) {
    if (!edit_grab) return;
    edit_grab = 0;
    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);
}

static void edit_begin(int row, int col) {
    edit_col = col;
    if (is_list_page(sbcs_page)) {
        if (row < 0 || row >= page_rows(sbcs_page) || col >= list_cols()) return;
        const char *p = field_ptr(row, col);
        snprintf(edit_buf, sizeof edit_buf, "%s", p ? p : "");
        edit_len = (int)strlen(edit_buf);
        edit_pos = edit_len;
        edit_row = row;
        edit_take_keyboard();
        edit_scroll();
        draw();
        return;
    }

    const CtlOpt *o = page_opt(sbcs_page, row);
    if (!o || o->type == CTL_OPT_BOOL) return;

    char val[SBCS_VAL_LEN];
    ctl_opt_get(o, val, sizeof val);
    snprintf(edit_buf, sizeof edit_buf, "%s", val);
    edit_len = (int)strlen(edit_buf);
    edit_pos = edit_len;
    edit_row = row;
    edit_take_keyboard();
    edit_scroll();
    draw();
}

static int lists_commit(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/.config/sbcwm/config.lua", get_home());
    if (sbcs_lists_save(path) < 0) {
        notify_show("config.lua not written", 0x602020);
        return -1;
    }

    sbcs_lists_load(path);
    page_clamp();
    reload_config_quiet(0);
    return 0;
}

static void edit_end(int apply) {
    if (edit_row < 0) return;
    int row = edit_row, col = edit_col;
    char buf[SBCS_VAL_LEN];

    edit_drop_keyboard();
    snprintf(buf, sizeof buf, "%s", edit_buf);
    edit_row = -1;
    edit_len = edit_pos = 0;
    edit_buf[0] = 0;

    if (is_list_page(sbcs_page)) {
        if (!apply) { draw(); return; }
        if (row < 0 || row >= page_rows(sbcs_page) || col >= list_cols()) return;
        size_t n = field_cap(col);
        snprintf(field_ptr(row, col), n, "%.*s", (int)n - 1, buf);
        lists_commit();
        draw();
        return;
    }

    const CtlOpt *o = page_opt(sbcs_page, row);
    if (!o) return;

    if (apply) {
        if (ctl_opt_set(o, buf) < 0)
            notify_show("invalid value", 0x602020);
        else if (sbcs_config_save() < 0)
            notify_show("config.lua not written", 0x602020);
    }
    draw();
}

static void toggle_bool(const CtlOpt *o) {
    char val[SBCS_VAL_LEN];
    ctl_opt_get(o, val, sizeof val);
    if (ctl_opt_set(o, atoi(val) ? "0" : "1") < 0) return;
    if (sbcs_config_save() < 0) notify_show("config.lua not written", 0x602020);
    draw();
}

void sbcs_open(void) {
    ctx_close();

    if (sbcs_win != XCB_NONE) {
        focus_win_id(sbcs_win);
        draw();
        return;
    }

    if (!cfg) return;

    char cfgdir[512];
    snprintf(cfgdir, sizeof cfgdir, "%s/.config/sbcwm/config.lua", get_home());
    sbcs_lists_load(cfgdir);

    int mon = mon_at_ptr();
    int mx = 0, my = 0, mw = 0, mh = 0;
    if (mon >= 0 && mon < n_mons) {
        mx = mons[mon].x;
        my = mons[mon].y;
        mw = mons[mon].w;
        mh = mons[mon].h;
    }
    if (mw < SBCS_MIN_W) mw = SBCS_MIN_W;
    if (mh < SBCS_MIN_H) mh = SBCS_MIN_H;

    sbcs_w = CLAMP(mw * 86 / 100, SBCS_MIN_W, MIN(1000, mw));
    sbcs_h = CLAMP(mh * 82 / 100, SBCS_MIN_H, MIN(720, mh));
    sbcs_x = CLAMP(mx + (mw - sbcs_w) / 2, mx, mx + MAX(0, mw - sbcs_w));
    sbcs_y = CLAMP(my + (mh - sbcs_h) / 3, my, my + MAX(0, mh - sbcs_h));

    unsigned long bg = panel_bg();
    unsigned long bd = panel_bd();

    sbcs_win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                    XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t em = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                  XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
                  XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS;
    uint32_t values[] = { (uint32_t)bg, (uint32_t)bd, 0, em };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, sbcs_win, root,
                      (int16_t)sbcs_x, (int16_t)sbcs_y, (uint16_t)sbcs_w,
                      (uint16_t)sbcs_h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);
    static const char panel_title[] = "Settings";
    xcb_atom_t utf8 = get_atom("UTF8_STRING");
    xcb_atom_t nwn = get_atom("_NET_WM_NAME");
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sbcs_win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, sizeof panel_title - 1, panel_title);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sbcs_win, nwn, utf8, 8,
                        sizeof panel_title - 1, panel_title);

    xcb_atom_t wm_protocols_atom = get_atom("WM_PROTOCOLS");
    xcb_atom_t take_focus = get_atom("WM_TAKE_FOCUS");
    xcb_atom_t proto[2] = { get_atom("WM_DELETE_WINDOW"), take_focus };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sbcs_win, wm_protocols_atom,
                        XCB_ATOM_ATOM, 32, 2, proto);

    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof hints);
    hints.flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE |
                  XCB_ICCCM_SIZE_HINT_P_RESIZE_INC;
    hints.min_width = (uint32_t)SBCS_MIN_W;
    hints.min_height = (uint32_t)SBCS_MIN_H;
    hints.max_width = (uint32_t)mw;
    hints.max_height = (uint32_t)mh;
    hints.width_inc = 1;
    hints.height_inc = 1;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sbcs_win,
                        XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32,
                        (uint32_t)(sizeof hints / sizeof(uint32_t)),
                        (const uint8_t *)&hints);

    xcb_map_window(conn, sbcs_win);
    xcb_flush(conn);
    sbcs_manage(sbcs_win);

    if (cur_normal == None) {
        cur_normal = XCreateFontCursor(dpy, XC_left_ptr);
        cur_text   = XCreateFontCursor(dpy, XC_xterm);
        cur_hand   = XCreateFontCursor(dpy, XC_hand2);
    }

    buffer_create();

    edit_row = -1;
    edit_len = edit_pos = 0;
    edit_buf[0] = 0;
    row_off = 0;
    nav_clamp();
    ui_font_sync();
    page_clamp();
    draw();
}

void sbcs_close(void) {
    if (sbcs_win == XCB_NONE) return;
    edit_drop_keyboard();
    buffer_free();
    xcb_destroy_window(conn, sbcs_win);
    sbcs_win = XCB_NONE;
    edit_row = -1;
    edit_len = edit_pos = 0;
    edit_buf[0] = 0;
    if (edit_font) { XftFontClose(dpy, edit_font); edit_font = NULL; }
    if (ui_font) { XftFontClose(dpy, ui_font); ui_font = NULL; }
    free(ui_font_name);
    ui_font_name = NULL;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
    xcb_flush(conn);
}

void sbcs_refresh(void) {
    if (sbcs_win == XCB_NONE) return;
    ui_font_sync();
    page_clamp();
    draw();
}

int sbcs_handle_configure(xcb_configure_notify_event_t *e) {
    if (sbcs_win == XCB_NONE || e->window != sbcs_win) return 0;

    sbcs_x = e->x;
    sbcs_y = e->y;
    int w = e->width > 0 ? e->width : sbcs_w;
    int h = e->height > 0 ? e->height : sbcs_h;
    if (w == sbcs_w && h == sbcs_h) return 1;

    sbcs_w = w;
    sbcs_h = h;
    buffer_resize();
    page_clamp();
    if (edit_row >= 0) edit_scroll();
    draw();
    return 1;
}

void sbcs_cleanup(void) {
    if (sbcs_win == XCB_NONE) return;
    buffer_free();
    if (last_cursor != None) { XUndefineCursor(dpy, sbcs_win); last_cursor = None; }
    xcb_destroy_window(conn, sbcs_win);
    sbcs_win = XCB_NONE;
    if (edit_font) { XftFontClose(dpy, edit_font); edit_font = NULL; }
    if (ui_font) { XftFontClose(dpy, ui_font); ui_font = NULL; }
    free(ui_font_name);
    ui_font_name = NULL;
    xcb_flush(conn);
}

static int del_hit(int x) {
    int db = del_box();
    int dx = del_x() + (del_w() - db) / 2;
    return x >= dx && x < dx + db;
}

static int chip_hit(int x, int y, int ry) {
    if (current_kind() != PAGE_KEYS) return 0;
    int ch = chip_h();
    if (y < ry + (row_h() - ch) / 2 || y >= ry + (row_h() + ch) / 2) return 0;
    for (int c = 0; c < 4; c++)
        if (x >= chip_x(c) && x < chip_x(c) + chip_w()) return 1;
    return 0;
}

static Cursor cursor_at(int x, int y) {
    if (x < nav_w()) return cur_hand;

    if (is_list_page(sbcs_page)) {
        if (y < add_row_y() + row_h()) return cur_hand;
        int i = list_item_at(y);
        if (i < 0) return cur_normal;
        if (del_hit(x)) return cur_hand;
        if (chip_hit(x, y, item_row_y(i))) return cur_hand;
        for (int c = 0; c < list_cols(); c++)
            if (x >= field_px(i, c) && x < field_px(i, c) + field_pw(c))
                return cur_text;
        return cur_normal;
    }

    int i = row_at(y);
    if (i < 0) return cur_normal;
    const CtlOpt *o = page_opt(sbcs_page, i);
    if (!o) return cur_normal;
    if (o->type == CTL_OPT_BOOL) return cur_hand;
    if (x >= field_x()) return cur_text;
    return cur_normal;
}

static int list_new(void) {
    switch (current_kind()) {
    case PAGE_KEYS:
        if (sbcs_nkeys >= SBCS_MAX_ROWS) return -1;
        memset(&sbcs_keyrows[sbcs_nkeys], 0, sizeof sbcs_keyrows[0]);
        return sbcs_nkeys++;
    case PAGE_CTX:
        if (sbcs_nctx >= SBCS_MAX_ROWS) return -1;
        memset(&sbcs_ctxrows[sbcs_nctx], 0, sizeof sbcs_ctxrows[0]);
        return sbcs_nctx++;
    default:
        if (sbcs_nicons >= SBCS_MAX_ROWS) return -1;
        memset(&sbcs_iconrows[sbcs_nicons], 0, sizeof sbcs_iconrows[0]);
        return sbcs_nicons++;
    }
}

static void list_add(void) {
    int row = list_new();
    if (row < 0) { notify_show("list is full", 0x602020); return; }

    page_scroll_to(row);
    lists_commit();
    edit_begin(row, 0);
}

static void list_delete(int i) {
    switch (current_kind()) {
    case PAGE_KEYS:
        if (i < 0 || i >= sbcs_nkeys) return;
        memmove(&sbcs_keyrows[i], &sbcs_keyrows[i + 1],
                (size_t)(sbcs_nkeys - i - 1) * sizeof sbcs_keyrows[0]);
        sbcs_nkeys--;
        break;
    case PAGE_CTX:
        if (i < 0 || i >= sbcs_nctx) return;
        memmove(&sbcs_ctxrows[i], &sbcs_ctxrows[i + 1],
                (size_t)(sbcs_nctx - i - 1) * sizeof sbcs_ctxrows[0]);
        sbcs_nctx--;
        break;
    default:
        if (i < 0 || i >= sbcs_nicons) return;
        memmove(&sbcs_iconrows[i], &sbcs_iconrows[i + 1],
                (size_t)(sbcs_nicons - i - 1) * sizeof sbcs_iconrows[0]);
        sbcs_nicons--;
        break;
    }
    if (edit_row > i) edit_row--;
    lists_commit();
    draw();
}

int sbcs_handle_press(xcb_button_press_event_t *e) {
    if (sbcs_win == XCB_NONE || e->event != sbcs_win) return 0;

    int ex = e->event_x, ey = e->event_y;

    if (e->detail == XCB_BUTTON_INDEX_4 || e->detail == XCB_BUTTON_INDEX_5) {
        int up = e->detail == XCB_BUTTON_INDEX_4 ? -1 : 1;
        if (ex < nav_w() && nav_max_off() > 0) {
            nav_off += up;
            nav_clamp();
            draw();
            return 1;
        }
        if (page_max_off() > 0) {
            row_off += up;
            page_clamp();
            draw();
        }
        return 1;
    }
    if (e->detail != XCB_BUTTON_INDEX_1) return 1;

    int p = item_at(ey);
    if (p >= 0 && ex < nav_w()) {
        edit_end(1);
        sbcs_page = p;
        row_off = 0;
        draw();
        return 1;
    }

    if (ex < nav_w()) { edit_end(1); return 1; }

    if (is_list_page(sbcs_page)) {
        if (ey < add_row_y() + row_h()) {
            edit_end(1);
            list_add();
            return 1;
        }

        int i = list_item_at(ey);
        if (i < 0) { edit_end(1); return 1; }
        int ry = item_row_y(i);

        if (del_hit(ex)) {
            edit_end(1);
            list_delete(i);
            return 1;
        }

        if (current_kind() == PAGE_KEYS) {
            static const unsigned bits[4] = { SBCS_MOD_MOD, SBCS_MOD_ALT,
                                               SBCS_MOD_CTRL, SBCS_MOD_SHIFT };
            for (int c = 0; c < 4; c++) {
                if (ex < chip_x(c) || ex >= chip_x(c) + chip_w()) continue;
                if (ey < ry + (row_h() - chip_h()) / 2 ||
                    ey >= ry + (row_h() + chip_h()) / 2) continue;
                edit_end(1);
                sbcs_keyrows[i].mods ^= bits[c];
                lists_commit();
                draw();
                return 1;
            }
        }

        for (int c = 0; c < list_cols(); c++) {
            if (ex < field_px(i, c) || ex >= field_px(i, c) + field_pw(c)) continue;
            if (i == edit_row && c == edit_col) { edit_end(1); return 1; }
            edit_begin(i, c);
            return 1;
        }

        edit_end(1);
        return 1;
    }

    int i = row_at(ey);
    if (i < 0) { edit_end(1); return 1; }

    const CtlOpt *o = page_opt(sbcs_page, i);
    if (!o) return 1;

    if (o->type == CTL_OPT_BOOL) {
        edit_end(1);
        toggle_bool(o);
        return 1;
    }

    if (ex >= field_x()) {
        if (i == edit_row) { edit_end(1); return 1; }
        edit_begin(i, 0);
        return 1;
    }

    edit_end(1);
    return 1;
}

int sbcs_handle_motion(xcb_motion_notify_event_t *e) {
    if (sbcs_win == XCB_NONE) return 0;

    Cursor c = cursor_at(e->event_x, e->event_y);
    if (c != last_cursor) {
        last_cursor = c;
        XDefineCursor(dpy, (Drawable)sbcs_win, c);
        XFlush(dpy);
    }
    return 0;
}

int sbcs_handle_key(xcb_key_press_event_t *e) {
    if (sbcs_win == XCB_NONE || e->event != sbcs_win) return 0;

    xcb_keysym_t ks = event_keysym(e);

    if (edit_row < 0) {
        if (ks == XK_Escape) sbcs_close();
        else if (ks == XK_Up || ks == XK_Down) {
            int p = CLAMP(sbcs_page + (ks == XK_Down ? 1 : -1), 0, n_pages() - 1);
            if (p != sbcs_page) {
                sbcs_page = p;
                row_off = 0;
                nav_scroll_to(p);
                draw();
            }
        }
        return 1;
    }

    switch (ks) {
        case XK_Escape:
            edit_end(0);
            return 1;
        case XK_Return:
        case XK_KP_Enter:
            edit_end(1);
            return 1;
        case XK_Tab: {
            if (is_list_page(sbcs_page)) {
                int cols = list_cols();
                int row = edit_row, col = (edit_col + 1) % cols;
                edit_end(1);
                edit_begin(row, col);
                return 1;
            }
            int rows = page_rows(sbcs_page);
            int start = (edit_row + 1) % rows;
            edit_end(1);
            for (int k = 0; k < rows; k++) {
                int j = (start + k) % rows;
                const CtlOpt *o = page_opt(sbcs_page, j);
                if (o && o->type != CTL_OPT_BOOL) { edit_begin(j, 0); break; }
            }
            return 1;
        }
        case XK_BackSpace:
            if (edit_pos > 0) {
                int start = utf8_trim(edit_buf, edit_pos - 1);
                memmove(edit_buf + start, edit_buf + edit_pos,
                        (size_t)(edit_len - edit_pos));
                edit_len -= edit_pos - start;
                edit_pos = start;
                edit_buf[edit_len] = 0;
            }
            edit_scroll();
            draw();
            return 1;
        case XK_Delete:
            if (edit_pos < edit_len) {
                memmove(edit_buf + edit_pos, edit_buf + edit_pos + 1,
                        (size_t)(edit_len - edit_pos - 1));
                edit_len--;
                edit_buf[edit_len] = 0;
            }
            draw();
            return 1;
        case XK_Left:
            edit_pos = utf8_trim(edit_buf, edit_pos);
            if (edit_pos > 0) edit_pos--;
            edit_scroll();
            draw();
            return 1;
        case XK_Right:
            edit_pos = utf8_trim(edit_buf, edit_pos);
            if (edit_pos < edit_len) edit_pos++;
            edit_scroll();
            draw();
            return 1;
        case XK_Home:
            edit_pos = 0;
            edit_scroll();
            draw();
            return 1;
        case XK_End:
            edit_pos = edit_len;
            edit_scroll();
            draw();
            return 1;
        default:
            break;
    }

    char txt[8];
    int n = event_text(e, txt, sizeof txt);
    if (n > 0 && edit_len + n < (int)sizeof edit_buf) {
        memmove(edit_buf + edit_pos + n, edit_buf + edit_pos,
                (size_t)(edit_len - edit_pos));
        memcpy(edit_buf + edit_pos, txt, (size_t)n);
        edit_pos += n;
        edit_len += n;
        edit_buf[edit_len] = 0;
        edit_scroll();
        draw();
    }
    return 1;
}

int sbcs_redraw(xcb_expose_event_t *e) {
    if (sbcs_win == XCB_NONE || e->window != sbcs_win) return 0;
    draw();
    return 1;
}
