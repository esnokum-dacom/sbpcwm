#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "sbcct.h"
#include "sbpcwm.h"
#include "ctl.h"
#include "sbcs.h"

#define SBCS_MAX_TRACKED 64
#define LINE_MAX_LEN      2048

static const char *top_level[] = { "defaultsh", "fonts", "fontb", NULL };

static int is_top_level(const char *name) {
    for (int i = 0; top_level[i]; i++)
        if (!strcmp(top_level[i], name)) return 1;
    return 0;
}

static void lua_str(char *out, size_t n, const char *s) {
    size_t i = 0;
    if (n == 0) return;
    if (i + 1 < n) out[i++] = '"';
    for (const char *p = s ? s : ""; *p && i + 2 < n; p++) {
        if (*p == '"' || *p == '\\') out[i++] = '\\';
        if (i + 1 < n) out[i++] = *p;
    }
    if (i + 1 < n) out[i++] = '"';
    out[i] = 0;
}

static void opt_line(const CtlOpt *o, int indent, int comma, char *out, size_t n) {
    char val[SBCS_VAL_LEN];
    ctl_opt_get(o, val, sizeof val);

    if (o->type == CTL_OPT_STR) {
        char q[SBCS_VAL_LEN * 2];
        lua_str(q, sizeof q, val);
        snprintf(out, n, "%*s%s = %s%s", indent, "", o->name, q, comma ? "," : "");
    } else {
        snprintf(out, n, "%*s%s = %s%s", indent, "", o->name, val, comma ? "," : "");
    }
}

static int line_assign(const char *line, char *name, size_t n) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    size_t i = 0;
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && i + 1 < n)
        name[i++] = *p++;
    name[i] = 0;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return 0;
    return i > 0;
}

static const char *line_value(const char *line) {
    const char *eq = strchr(line, '=');
    if (!eq) return NULL;
    const char *p = eq + 1;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int same_value(const char *p, const char *want) {
    size_t n = strlen(want);
    for (size_t i = 0; i < n; i++)
        if (p[i] != want[i]) return 0;

    const char *q = p + n;
    while (*q == ' ' || *q == '\t') q++;
    return *q == ',' || *q == '\n' || *q == '\r' || *q == 0 ||
           (q[0] == '-' && q[1] == '-');
}

static void flush_missing(FILE *out, unsigned char *seen, int top, int indent, int comma) {
    size_t n = 0;
    const CtlOpt *list = ctl_opt_list(&n);
    for (size_t i = 0; i < n; i++) {
        if (seen[i]) continue;
        if (is_top_level(list[i].name) != top) continue;
        char line[LINE_MAX_LEN];
        opt_line(&list[i], indent, comma, line, sizeof line);
        fprintf(out, "%s\n", line);
        seen[i] = 1;
    }
}

int sbcs_config_save(void) {
    if (!cfg) return -1;
    const char *home = get_home();
    if (!home) return -1;

    char path[512], tmp[600];
    snprintf(path, sizeof path, "%s/.config/sbcwm/config.lua", home);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    size_t nopt = 0;
    (void)ctl_opt_list(&nopt);
    if (nopt == 0 || nopt > SBCS_MAX_TRACKED) return -1;
    unsigned char seen[SBCS_MAX_TRACKED];
    memset(seen, 0, sizeof seen);

    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        return -1;
    }

    if (!in) {
        fprintf(out, "-- sbcwm configuration (options are managed by sbcs)\n");
        flush_missing(out, seen, 1, 0, 0);
        fprintf(out, "\nopts = {\n");
        flush_missing(out, seen, 0, 2, 1);
        fprintf(out, "}\n");
        fclose(out);
        if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
        return 0;
    }

    char line[LINE_MAX_LEN];
    int in_opts = 0, wrote_block = 0;

    while (fgets(line, sizeof line, in)) {
        char name[128];
        if (!in_opts && !strncmp(line, "opts", 4)) {
            const char *p = line + 4;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') { in_opts = 1; wrote_block = 1; fputs(line, out); continue; }
        }
        if (in_opts && (line[0] == '}' ||
                        (line[0] == ' ' && line[1] == '}'))) {
            flush_missing(out, seen, 0, 2, 1);
            in_opts = 0;
            fputs(line, out);
            continue;
        }
        if (line_assign(line, name, sizeof name)) {
            const CtlOpt *o = ctl_find_opt(name);
            if (o) {
                size_t idx = (size_t)(o - ctl_opt_list(NULL));
                if (idx < nopt) seen[idx] = 1;

                char want[SBCS_VAL_LEN * 2];
                if (o->type == CTL_OPT_STR) {
                    char val[SBCS_VAL_LEN];
                    ctl_opt_get(o, val, sizeof val);
                    lua_str(want, sizeof want, val);
                } else {
                    ctl_opt_get(o, want, sizeof want);
                }

                const char *val = line_value(line);
                if (val && same_value(val, want)) {
                    fputs(line, out);
                } else {
                    char repl[LINE_MAX_LEN];
                    opt_line(o, in_opts ? 2 : 0, in_opts, repl, sizeof repl);

                    const char *cmt = val ? strstr(val, "--") : NULL;
                    if (cmt) {
                        char cmtbuf[LINE_MAX_LEN / 2];
                        size_t cl = 0;
                        while (cmt[cl] && cmt[cl] != '\n' && cmt[cl] != '\r' &&
                               cl + 1 < sizeof cmtbuf) {
                            cmtbuf[cl] = cmt[cl];
                            cl++;
                        }
                        cmtbuf[cl] = 0;
                        while (cl && (cmtbuf[cl - 1] == ' ' || cmtbuf[cl - 1] == '\t'))
                            cmtbuf[--cl] = 0;
                        size_t rl = strlen(repl);
                        if (cl && rl + cl + 3 < sizeof repl)
                            snprintf(repl + rl, sizeof repl - rl, "  %s", cmtbuf);
                    }
                    fprintf(out, "%s\n", repl);
                }
                continue;
            }
        }
        fputs(line, out);
    }

    fclose(in);
    flush_missing(out, seen, 1, 0, 0);
    if (!wrote_block) {
        fprintf(out, "\nopts = {\n");
        flush_missing(out, seen, 0, 2, 1);
        fprintf(out, "}\n");
    }
    fclose(out);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

SbcsKeyRow  sbcs_keyrows[SBCS_MAX_ROWS];
int         sbcs_nkeys;
SbcsIconRow sbcs_iconrows[SBCS_MAX_ROWS];
int         sbcs_nicons;
SbcsCtxRow  sbcs_ctxrows[SBCS_MAX_ROWS];
int         sbcs_nctx;

static const char *known_funcs[] = {
    "win_kill", "win_center", "win_fs", "run", "quit", "canvas_pan_key",
    "canvas_reset", "move_nextmon", "ws_focusnext", "toggle_icons",
    "reload_config", NULL,
};

static int is_known_func(const char *s) {
    for (int i = 0; known_funcs[i]; i++)
        if (!strcmp(known_funcs[i], s)) return 1;
    return 0;
}

static unsigned mod_bit_for(const char *name) {
    if (!strcmp(name, "super") || !strcmp(name, "mod")) return SBCS_MOD_MOD;
    if (!strcmp(name, "alt"))  return SBCS_MOD_ALT;
    if (!strcmp(name, "ctrl")) return SBCS_MOD_CTRL;
    if (!strcmp(name, "shift")) return SBCS_MOD_SHIFT;
    return 0;
}

static const char *mod_name_for(unsigned bit) {
    switch (bit) {
    case SBCS_MOD_MOD:   return "super";
    case SBCS_MOD_ALT:   return "alt";
    case SBCS_MOD_CTRL:  return "ctrl";
    default:             return "shift";
    }
}

static const char *parse_value(const char *p, char *out, size_t n) {
    size_t i = 0;
    out[0] = 0;
    for (;;) {
        const char *before = p;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "os.getenv", 9)) {
            const char *q = strchr(p, '"');
            if (!q) break;
            const char *e = strchr(++q, '"');
            if (!e) break;
            char var[32];
            size_t vl = (size_t)(e - q);
            if (vl >= sizeof var) vl = sizeof var - 1;
            memcpy(var, q, vl);
            var[vl] = 0;
            const char *val = !strcmp(var, "HOME") ? get_home() : "";
            size_t l = strlen(val);
            if (i + l < n) { memcpy(out + i, val, l); i += l; }
            out[i] = 0;
            p = e + 1;
            if (*p == ')') p++;
        } else if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) p++;
                if (i + 1 < n) out[i++] = *p;
                p++;
            }
            if (*p == q) p++;
            out[i] = 0;
        } else if (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.' ||
                   *p == '/') {
            while (*p && *p != ',' && *p != '}' && *p != '\n' &&
                   strchr(" \t", *p) == NULL) {
                if (i + 1 < n) out[i++] = *p;
                p++;
            }
            out[i] = 0;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '.' && p[1] == '.') {
            p += 2;
            if (p == before) break;
            continue;
        }
        break;
    }
    return p;
}

static const char *field_value(const char *body, const char *field) {
    size_t fl = strlen(field);
    for (const char *p = body; *p; p++) {
        if (p != body && !strchr("{, \t", p[-1])) continue;
        if (strncmp(p, field, fl)) continue;
        const char *q = p + fl;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        for (q++; *q == ' ' || *q == '\t'; q++) ;
        return q;
    }
    return NULL;
}

static void parse_list(const char *p, char *out, size_t n) {
    size_t i = 0;
    out[0] = 0;
    if (!p) return;
    while (*p && *p != '{') p++;
    if (!*p) return;
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '}' || !*p) break;
        char item[256];
        const char *before = p;
        p = parse_value(p, item, sizeof item);

        if (p == before) p++;
        if (i && i + 1 < n) out[i++] = ' ';
        size_t l = strlen(item);
        if (i + l < n) { memcpy(out + i, item, l); i += l; }
        out[i] = 0;
    }
}

typedef void (*EntryFn)(const char *body, void *data);

static void walk_table(const char *body, size_t len, EntryFn fn, void *data) {
    int depth = 0;
    const char *start = NULL;
    for (size_t i = 0; i < len; i++) {
        char c = body[i];
        if (c == '{') {
            if (++depth == 2) start = body + i + 1;
        } else if (c == '}') {
            if (depth == 2 && start) {
                char entry[1024];
                size_t l = (size_t)((body + i) - start);
                if (l >= sizeof entry) l = sizeof entry - 1;
                memcpy(entry, start, l);
                entry[l] = 0;
                fn(entry, data);
                start = NULL;
            }
            depth--;
        } else if (c == '"' || c == '\'') {
            char q = c;
            while (++i < len && body[i] != q)
                if (body[i] == '\\') i++;
        }
    }
}

static void take_key(const char *body, void *data) {
    (void)data;
    if (sbcs_nkeys >= SBCS_MAX_ROWS) return;
    SbcsKeyRow *r = &sbcs_keyrows[sbcs_nkeys++];
    memset(r, 0, sizeof *r);
    const char *v;
    if ((v = field_value(body, "key"))) parse_value(v, r->key, sizeof r->key);
    char mods[128] = "";
    parse_list(field_value(body, "mod"), mods, sizeof mods);
    for (char *m = strtok(mods, " "); m; m = strtok(NULL, " "))
        r->mods |= mod_bit_for(m);
    char func[64] = "";
    if ((v = field_value(body, "func"))) parse_value(v, func, sizeof func);
    r->quit = !strcmp(func, "quit");
    const char *av = field_value(body, "arg");
    char arg[256] = "";

    if (av && strchr(av, '{')) parse_list(av, arg, sizeof arg);
    else                     parse_value(av ? av : "", r->arg, sizeof r->arg);

    snprintf(r->cmd, sizeof r->cmd, "%s",
             (!strcmp(func, "run") || !strcmp(func, "quit"))
                 ? (arg[0] ? arg : r->arg) : func);
}

static void take_icon(const char *body, void *data) {
    (void)data;
    if (sbcs_nicons >= SBCS_MAX_ROWS) return;
    SbcsIconRow *r = &sbcs_iconrows[sbcs_nicons++];
    memset(r, 0, sizeof *r);
    const char *v;
    char num[32];
    if ((v = field_value(body, "name")))  parse_value(v, r->name,  sizeof r->name);
    if ((v = field_value(body, "image"))) {
        parse_value(v, r->image, sizeof r->image);

        const char *e = v;
        while (*e && *e != ',' && *e != '}') e++;
        size_t l = (size_t)(e - v);
        while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
        if (l >= sizeof r->image_src) l = sizeof r->image_src - 1;
        memcpy(r->image_src, v, l);
        r->image_src[l] = 0;
    }
    const char *cv = field_value(body, "cmd");
    if (cv && strchr(cv, '{')) parse_list(cv, r->cmd, sizeof r->cmd);
    else                      parse_value(cv ? cv : "", r->cmd, sizeof r->cmd);
    if ((v = field_value(body, "x"))) {
        parse_value(v, num, sizeof num);
        r->x = atoi(num);
        if ((v = field_value(body, "y"))) {
            parse_value(v, num, sizeof num);
            r->y = atoi(num);
        }
    }
    if ((v = field_value(body, "mon"))) {
        parse_value(v, num, sizeof num);
        r->mon = atoi(num);
    }
}

static void take_ctx(const char *body, void *data) {
    (void)data;
    if (sbcs_nctx >= SBCS_MAX_ROWS) return;
    SbcsCtxRow *r = &sbcs_ctxrows[sbcs_nctx++];
    memset(r, 0, sizeof *r);
    const char *v;
    if ((v = field_value(body, "label"))) parse_value(v, r->label, sizeof r->label);
    if ((v = field_value(body, "func")))  parse_value(v, r->func,  sizeof r->func);
    const char *av = field_value(body, "arg");
    if (av && strchr(av, '{')) {
        char arg[256] = "";
        parse_list(av, arg, sizeof arg);
        snprintf(r->arg, sizeof r->arg, "%s", arg);
    } else if (av) {
        parse_value(av, r->arg, sizeof r->arg);
    }
    if (!r->func[0]) snprintf(r->func, sizeof r->func, "run");
}

typedef struct { char *p; size_t len, cap; } Text;

static int text_push(Text *t, char c) {
    if (t->len + 2 >= t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        char *p = realloc(t->p, cap);
        if (!p) return 0;
        t->p = p;
        t->cap = cap;
    }
    t->p[t->len++] = c;
    t->p[t->len] = 0;
    return 1;
}

void sbcs_lists_load(const char *path) {
    sbcs_nkeys = 0;
    sbcs_nicons = 0;
    sbcs_nctx = 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int in_keys = 0, in_icons = 0, in_ctx = 0, depth = 0;
    Text body = { NULL, 0, 0 };
    while ((len = getline(&line, &cap, f)) > 0) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!in_keys && !in_icons && !in_ctx) {
            if (!strncmp(p, "keys", 4) && (p[4] == ' ' || p[4] == '='))  in_keys = 1;
            else if (!strncmp(p, "icons", 5) && (p[5] == ' ' || p[5] == '=')) in_icons = 1;
            else if (!strncmp(p, "ctx", 3) && (p[3] == ' ' || p[3] == '=')) in_ctx = 1;
            else continue;
        }
        for (ssize_t i = 0; i < len; i++) {
            char c = line[i];
            if (c == '"' || c == '\'') {

                char q = c;
                if (depth >= 1 && !text_push(&body, c)) goto done;
                while (++i < len && line[i] != q) {

                    if (line[i] == '\\' && line[i + 1]) {
                        if (depth >= 1 && !text_push(&body, '\\')) goto done;
                        i++;
                    }
                    if (depth >= 1 && !text_push(&body, line[i])) goto done;
                }
                if (depth >= 1 && !text_push(&body, q)) goto done;
                continue;
            }
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                if (--depth == 0) {
                    if (in_keys)       walk_table(body.p, body.len, take_key, NULL);
                    else if (in_icons) walk_table(body.p, body.len, take_icon, NULL);
                    else if (in_ctx)   walk_table(body.p, body.len, take_ctx, NULL);
                    in_keys = in_icons = in_ctx = 0;
                    depth = 0;
                    body.len = 0;
                    if (body.p) body.p[0] = 0;
                    break;
                }
            }
            if (depth >= 1 && !text_push(&body, c)) goto done;
        }
    }
done:
    free(body.p);
    free(line);
    fclose(f);
}

static void emit_words(FILE *out, const char *cmd) {
    fputc('{', out);
    int n = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ') e++;
        char word[256], s[300];
        size_t l = (size_t)(e - p);
        if (l >= sizeof word) l = sizeof word - 1;
        memcpy(word, p, l);
        word[l] = 0;
        lua_str(s, sizeof s, word);
        fprintf(out, "%s%s", n++ ? ", " : " ", s);
        p = e;
    }
    if (!n) fputs(" \"\"", out);
    fputc('}', out);
}

static void emit_mods(FILE *out, unsigned mods) {
    fputs("mod = {", out);
    static const unsigned order[] = { SBCS_MOD_MOD, SBCS_MOD_ALT,
                                      SBCS_MOD_CTRL, SBCS_MOD_SHIFT };
    int n = 0;
    for (unsigned i = 0; i < sizeof order / sizeof *order; i++)
        if (mods & order[i]) fprintf(out, "%s\"%s\"", n++ ? ", " : " ",
                                     mod_name_for(order[i]));
    fputs("}, ", out);
}

static void emit_cmd(FILE *out, const char *cmd, const char *scalar) {
    if (is_known_func(cmd)) {
        char s[600];
        lua_str(s, sizeof s, cmd);
        fprintf(out, "func = %s", s);
        if (scalar && *scalar) fprintf(out, ", arg = %s", scalar);
        return;
    }
    fputs("func = \"run\", arg = ", out);
    emit_words(out, cmd);
}

static void emit_keys(FILE *out) {
    fputs("keys = {\n", out);
    for (int i = 0; i < sbcs_nkeys; i++) {
        SbcsKeyRow *r = &sbcs_keyrows[i];
        char s[600];
        fputs("  { ", out);
        emit_mods(out, r->mods);
        lua_str(s, sizeof s, r->key);
        fprintf(out, "key = %s, ", s);
        if (r->quit && !is_known_func(r->cmd)) {

            fputs("func = \"quit\", arg = ", out);
            emit_words(out, r->cmd);
        } else {
            emit_cmd(out, r->cmd, r->arg);
        }
        fputs(" },\n", out);
    }
    fputs("}\n", out);
}

static void emit_ctx(FILE *out) {
    fputs("ctx = {\n", out);
    for (int i = 0; i < sbcs_nctx; i++) {
        SbcsCtxRow *r = &sbcs_ctxrows[i];
        char s[600];
        fputs("  { ", out);
        lua_str(s, sizeof s, r->label);
        fprintf(out, "label = %s, ", s);
        if (!r->func[0] || !strcmp(r->func, "run")) {
            fputs("func = \"run\", arg = ", out);
            emit_words(out, r->arg);
        } else if (!is_known_func(r->func)) {
            fputs("func = \"run\", arg = ", out);
            emit_words(out, r->arg[0] ? r->arg : r->func);
        } else {
            lua_str(s, sizeof s, r->func);
            fprintf(out, "func = %s", s);
            if (r->arg[0]) fprintf(out, ", arg = %s", r->arg);
        }
        fputs(" },\n", out);
    }
    fputs("}\n", out);
}

static void emit_icons(FILE *out) {
    fputs("icons = {\n", out);
    for (int i = 0; i < sbcs_nicons; i++) {
        SbcsIconRow *r = &sbcs_iconrows[i];
        char s[600];
        fputs("  { ", out);
        lua_str(s, sizeof s, r->name);
        fprintf(out, "name = %s, ", s);
        if (r->image_src[0]) {
            char again[256];
            parse_value(r->image_src, again, sizeof again);
            if (!strcmp(again, r->image)) {
                fprintf(out, "image = %s, ", r->image_src);
            } else {
                lua_str(s, sizeof s, r->image);
                fprintf(out, "image = %s, ", s);
            }
        } else {
            lua_str(s, sizeof s, r->image);
            fprintf(out, "image = %s, ", s);
        }
        fprintf(out, "x = %d, y = %d", r->x, r->y);
        if (r->mon) fprintf(out, ", mon = %d", r->mon);
        fputs(", cmd = ", out);
        emit_words(out, r->cmd);
        fputs(" },\n", out);
    }
    fputs("}\n", out);
}

int sbcs_lists_save(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.sbcs.tmp", path);
    FILE *in = fopen(path, "r");
    if (!in) return -1;
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); return -1; }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int wrote_keys = 0, wrote_icons = 0, wrote_ctx = 0;
    while ((len = getline(&line, &cap, in)) > 0) {
        (void)len;
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int is_keys = !strncmp(p, "keys", 4) && (p[4] == ' ' || p[4] == '=');
        int is_icons = !strncmp(p, "icons", 5) && (p[5] == ' ' || p[5] == '=');
        int is_ctx = !strncmp(p, "ctx", 3) && (p[3] == ' ' || p[3] == '=');
        if (!is_keys && !is_icons && !is_ctx) { fputs(line, out); continue; }

        if (is_keys) emit_keys(out);
        else if (is_icons) emit_icons(out);
        else emit_ctx(out);
        if (is_keys) wrote_keys = 1;
        else if (is_icons) wrote_icons = 1;
        else wrote_ctx = 1;

        if (!strchr(line, '}')) {
            int done = 0;
            while (!done && getline(&line, &cap, in) > 0) {
                const char *q = line;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '}') done = 1;
            }
        }
    }
    if (!wrote_keys)  emit_keys(out);
    if (!wrote_icons) emit_icons(out);
    if (!wrote_ctx)   emit_ctx(out);
    free(line);
    fclose(in);
    if (fclose(out) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
