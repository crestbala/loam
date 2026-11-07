/**
 * pkgresolve.c — package roots for `import "name"` and `import "name:module"`.
 *
 * Discovery order (first name wins):
 *   1. `vendor/<name>/` next to a `loam.lock` that lists it, walking up from
 *      the entry file. The lock's checksum column must match the tree.
 *   2. The nearest `loam.toml` with `[workspace] members`, plus any `[package]`
 *      manifest met on the way up.
 *   3. `LOAM_PATH` roots, as an override for names still missing. `std:` does
 *      not consult this list; it is the language library only.
 *
 * There is no `export` keyword. A package's entry file publishes every
 * top-level declaration, and the importer decides which names to pull in.
 *
 * The lock checksum is a 31-bit FNV-style hash of the sorted relative paths
 * and file bytes. `zeli pkg sync` writes the same decimal. Keep the two
 * in step: offset 18651461, prime 16777619, mask 0x7fffffff, and a trailing
 * 0 byte after each path and after each file.
 */
#include "compile.h"
#include "ext.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LOAM_STD_DIR
#define LOAM_STD_DIR "std"
#endif

#ifndef LOAM_PATH
#define LOAM_PATH ""
#endif

#define PKG_MAX 64

typedef struct {
    char name[96];
    char dir[1024];
    char entry[256];
    int from_vendor;
    char sum[32]; /* expected decimal from the lock; empty if absent */
} Pkg;

static Pkg g_pkgs[PKG_MAX];
static int g_npkgs;
static char g_anchor[1024];
static char g_err[512];

const char *loam_package_error(void) { return g_err; }

static void clear_err(void) { g_err[0] = '\0'; }

static int file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_under(const char *path, const char *dir) {
    size_t d = dir ? strlen(dir) : 0;
    if (!path || !dir || d == 0) return 0;
    return strncmp(path, dir, d) == 0 && (path[d] == '/' || path[d] == '\0');
}

static const char *search_path(char *buf, size_t sz) {
    const char *env = getenv("LOAM_PATH");
    if (env && env[0] && strlen(LOAM_PATH) > 0) snprintf(buf, sz, "%s:%s", env, LOAM_PATH);
    else if (env && env[0]) snprintf(buf, sz, "%s", env);
    else snprintf(buf, sz, "%s", LOAM_PATH);
    return buf;
}

static void basename_of(const char *path, char *out, size_t sz) {
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    if (!base) base = "";
    snprintf(out, sz, "%s", base);
}

static int join2(char *out, size_t sz, const char *a, const char *b) {
    int n;
    if (!b || !b[0]) return 0;
    if (b[0] == '/') n = snprintf(out, sz, "%s", b);
    else if (!a || !a[0] || strcmp(a, ".") == 0) n = snprintf(out, sz, "%s", b);
    else n = snprintf(out, sz, "%s/%s", a, b);
    return n > 0 && (size_t)n < sz;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

static int copy_quoted(const char **ps, char *out, size_t sz) {
    const char *s = *ps;
    size_t n = 0;
    if (*s != '"') return 0;
    s++;
    while (*s && *s != '"' && *s != '\n') {
        if (n + 1 >= sz) return 0;
        out[n++] = *s++;
    }
    if (*s != '"') return 0;
    out[n] = '\0';
    *ps = s + 1;
    return 1;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) sz = 0;
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz && ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/** `[section]` name, or 0 when `line` is not a header. */
static int section_name(const char *line, char *out, size_t sz) {
    const char *s = skip_ws(line);
    size_t n;
    const char *end;
    if (*s != '[') return 0;
    s++;
    end = strchr(s, ']');
    if (!end || end == s) return 0;
    n = (size_t)(end - s);
    if (n + 1 > sz) n = sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return 1;
}

static Pkg *find_pkg(const char *name) {
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_npkgs; i++)
        if (strcmp(g_pkgs[i].name, name) == 0) return &g_pkgs[i];
    return NULL;
}

int loam_package_known(const char *name) { return find_pkg(name) != NULL; }

int loam_package_count(void) { return g_npkgs; }

const char *loam_package_name(int i) {
    if (i < 0 || i >= g_npkgs) return NULL;
    return g_pkgs[i].name;
}

const char *loam_package_dir(int i) {
    if (i < 0 || i >= g_npkgs) return NULL;
    return g_pkgs[i].dir;
}

void loam_package_clear_error(void) { clear_err(); }

static void add_pkg(const char *name, const char *dir, const char *entry, int from_vendor,
                    const char *sum) {
    Pkg *pk;
    if (!name || !name[0] || !dir || !dir[0] || find_pkg(name)) return;
    if (g_npkgs >= PKG_MAX) return;
    pk = &g_pkgs[g_npkgs++];
    memset(pk, 0, sizeof *pk);
    snprintf(pk->name, sizeof pk->name, "%s", name);
    snprintf(pk->dir, sizeof pk->dir, "%s", dir);
    if (entry) snprintf(pk->entry, sizeof pk->entry, "%s", entry);
    pk->from_vendor = from_vendor;
    if (sum) snprintf(pk->sum, sizeof pk->sum, "%s", sum);
}

static void read_package_fields(const char *toml_path, char *name, size_t namesz,
                                char *entry, size_t entrysz) {
    char *text = read_all(toml_path);
    char *line;
    char section[64];
    int in_pkg = 0;
    if (!text) return;
    section[0] = '\0';
    line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        const char *s;
        if (nl) *nl = '\0';
        s = skip_ws(line);
        if (section_name(s, section, sizeof section)) in_pkg = strcmp(section, "package") == 0;
        else if (in_pkg && strncmp(s, "name", 4) == 0 && (s[4] == ' ' || s[4] == '=' || s[4] == '\t')) {
            const char *q = strchr(s, '"');
            if (q) copy_quoted(&q, name, namesz);
        } else if (in_pkg && strncmp(s, "entry", 5) == 0 &&
                   (s[5] == ' ' || s[5] == '=' || s[5] == '\t')) {
            const char *q = strchr(s, '"');
            if (q) copy_quoted(&q, entry, entrysz);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(text);
}

/** 1 when `path` contains a `[workspace]` section. Members are filled, up to
 *  `max`, as strings the caller must free. */
static int read_workspace(const char *path, char **members, int max, int *nmem) {
    char *text = read_all(path);
    char *line;
    char section[64];
    int in_ws = 0;
    int in_list = 0;
    int saw = 0;
    if (!text) return 0;
    *nmem = 0;
    section[0] = '\0';
    line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        const char *s;
        if (nl) *nl = '\0';
        s = skip_ws(line);
        if (section_name(s, section, sizeof section)) {
            in_ws = strcmp(section, "workspace") == 0;
            if (in_ws) saw = 1;
            in_list = 0;
        } else if (in_ws && !in_list && strncmp(s, "members", 7) == 0) {
            const char *br = strchr(s, '[');
            if (br) {
                in_list = strchr(br, ']') == NULL;
                s = br + 1;
            }
        }
        if (in_ws && (in_list || (s && strchr(s, '"')))) {
            while (s && *s && *s != ']') {
                char mem[512];
                s = skip_ws(s);
                if (*s == '"') {
                    if (copy_quoted(&s, mem, sizeof mem) && *nmem < max)
                        members[(*nmem)++] = strdup(mem);
                    s = skip_ws(s);
                    if (*s == ',') s++;
                    continue;
                }
                break;
            }
            if (s && strchr(s, ']')) in_list = 0;
            else if (strchr(line, ']')) in_list = 0;
            else if (in_list == 0 && strchr(skip_ws(line), '"') == NULL) { /* not a list line */ }
            else in_list = strchr(line, ']') == NULL && in_ws;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(text);
    return saw;
}

/* --- checksum (must match `zeli pkg sync`) ----------------------------- */

static int64_t hash_step(int64_t h, unsigned char b) {
    h ^= (int64_t)b;
    h *= (int64_t)16777619;
    h &= (int64_t)2147483647;
    return h;
}

static int64_t hash_bytes(int64_t h, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) h = hash_step(h, (unsigned char)s[i]);
    return hash_step(h, 0);
}

static int64_t hash_file(int64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    int c;
    if (!f) return hash_step(h, 0);
    while ((c = fgetc(f)) != EOF) h = hash_step(h, (unsigned char)c);
    fclose(f);
    return hash_step(h, 0);
}

typedef struct {
    char **items;
    int n, cap;
} StrList;

static void sl_add(StrList *s, const char *rel) {
    if (s->n >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->items = (char **)realloc(s->items, (size_t)s->cap * sizeof(char *));
        if (!s->items) return;
    }
    s->items[s->n++] = strdup(rel);
}

static void collect_files(const char *root, const char *rel, StrList *out) {
    char dir[1200];
    DIR *d;
    struct dirent *ent;
    if (rel && rel[0]) snprintf(dir, sizeof dir, "%s/%s", root, rel);
    else snprintf(dir, sizeof dir, "%s", root);
    d = opendir(dir);
    if (!d) return;
    while ((ent = readdir(d))) {
        char child[1024];
        char full[1200];
        struct stat st;
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        if (strcmp(ent->d_name, ".git") == 0) continue;
        if (rel && rel[0]) snprintf(child, sizeof child, "%s/%s", rel, ent->d_name);
        else snprintf(child, sizeof child, "%s", ent->d_name);
        snprintf(full, sizeof full, "%s/%s", root, child);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) collect_files(root, child, out);
        else if (S_ISREG(st.st_mode)) sl_add(out, child);
    }
    closedir(d);
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int64_t tree_hash(const char *root) {
    StrList files;
    int64_t h = 18651461;
    int i;
    memset(&files, 0, sizeof files);
    collect_files(root, "", &files);
    if (files.n > 1) qsort(files.items, (size_t)files.n, sizeof(char *), cmpstr);
    for (i = 0; i < files.n; i++) {
        char full[1200];
        h = hash_bytes(h, files.items[i], strlen(files.items[i]));
        snprintf(full, sizeof full, "%s/%s", root, files.items[i]);
        h = hash_file(h, full);
        free(files.items[i]);
    }
    free(files.items);
    return h;
}

static int lock_sum(const char *appdir, const char *name, char *sum, size_t sumsz, int *listed) {
    char path[1200];
    char *text;
    char *line;
    *listed = 0;
    if (sumsz) sum[0] = '\0';
    snprintf(path, sizeof path, "%s/loam.lock", appdir);
    text = read_all(path);
    if (!text) return 0;
    line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        char *tab;
        char *fields[6];
        int nf = 0;
        if (nl) *nl = '\0';
        if (line[0] != '#' && line[0] != '\0') {
            fields[nf++] = line;
            for (tab = line; *tab && nf < 6; tab++) {
                if (*tab == '\t') {
                    *tab = '\0';
                    fields[nf++] = tab + 1;
                }
            }
            if (nf >= 1 && strcmp(fields[0], name) == 0) {
                *listed = 1;
                if (nf >= 4 && sumsz) snprintf(sum, sumsz, "%s", fields[3]);
                break;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(text);
    return 1;
}

static int checksum_matches(const Pkg *pk) {
    char got[32];
    int64_t h;
    if (!pk->sum[0]) return 1;
    h = tree_hash(pk->dir);
    snprintf(got, sizeof got, "%lld", (long long)h);
    if (strcmp(got, pk->sum) == 0) return 1;
    snprintf(g_err, sizeof g_err,
             "package '%s' does not match loam.lock (run `zeli pkg sync`)", pk->name);
    return 0;
}

int loam_vendor_check(const char *pkgdir) {
    char dir[1024];
    char app[1024];
    char base[96];
    char sum[32];
    char *slash;
    int listed = 0;
    int64_t h;
    char got[32];
    clear_err();
    if (!pkgdir || !pkgdir[0]) return 1;
    snprintf(dir, sizeof dir, "%s", pkgdir);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) return 1;
    *slash = '\0';
    basename_of(dir, base, sizeof base);
    if (strcmp(base, "vendor") != 0) return 1;
    basename_of(pkgdir, base, sizeof base);
    slash = strrchr(dir, '/');
    if (!slash) snprintf(app, sizeof app, ".");
    else if (slash == dir) snprintf(app, sizeof app, "/");
    else {
        *slash = '\0';
        snprintf(app, sizeof app, "%s", dir);
    }
    if (!lock_sum(app, base, sum, sizeof sum, &listed)) {
        snprintf(g_err, sizeof g_err,
                 "cannot find package '%s' (vendor/%s has no loam.lock; run `zeli pkg sync`)",
                 base, base);
        return 0;
    }
    if (!listed) {
        snprintf(g_err, sizeof g_err,
                 "cannot find package '%s' (vendor/%s is not in loam.lock; run `zeli pkg sync`)",
                 base, base);
        return 0;
    }
    if (!sum[0]) return 1;
    h = tree_hash(pkgdir);
    snprintf(got, sizeof got, "%lld", (long long)h);
    if (strcmp(got, sum) == 0) return 1;
    snprintf(g_err, sizeof g_err,
             "package '%s' does not match loam.lock (run `zeli pkg sync`)", base);
    return 0;
}

static int try_rel(const char *dir, const char *rel, char *out, size_t outsz) {
    size_t e;
    int n;
    if (!rel || !rel[0]) return 0;
    n = snprintf(out, outsz, "%s/%s", dir, rel);
    if (n > 0 && (size_t)n < outsz && file_exists(out)) return 1;
    /* A manifest entry already has an extension. Defaults try both. */
    if (strchr(rel, '.')) return 0;
    for (e = 0; e < LOAM_EXT_COUNT; e++) {
        n = snprintf(out, outsz, "%s/%s%s", dir, rel, loam_ext_name(e));
        if (n > 0 && (size_t)n < outsz && file_exists(out)) return 1;
    }
    return 0;
}

static int entry_file(const Pkg *pk, char *out, size_t outsz) {
    char rel[256];
    if (pk->entry[0] && try_rel(pk->dir, pk->entry, out, outsz)) return 1;
    snprintf(rel, sizeof rel, "src/%s", pk->name);
    if (try_rel(pk->dir, rel, out, outsz)) return 1;
    snprintf(rel, sizeof rel, "std/%s", pk->name);
    if (try_rel(pk->dir, rel, out, outsz)) return 1;
    if (try_rel(pk->dir, pk->name, out, outsz)) return 1;
    if (try_rel(pk->dir, "src/lib", out, outsz)) return 1;
    if (try_rel(pk->dir, "main", out, outsz)) return 1;
    return 0;
}

static int module_file(const Pkg *pk, const char *mod, char *out, size_t outsz) {
    char rel[512];
    if (!mod || !mod[0]) return 0;
    snprintf(rel, sizeof rel, "src/%s", mod);
    if (try_rel(pk->dir, rel, out, outsz)) return 1;
    snprintf(rel, sizeof rel, "std/%s", mod);
    if (try_rel(pk->dir, rel, out, outsz)) return 1;
    if (try_rel(pk->dir, mod, out, outsz)) return 1;
    return 0;
}

/** Collapse "." / ".." so a workspace member `../shakelib` matches the loaded path. */
static int normalize_dir(const char *in, char *out, size_t sz) {
    char parts[64][256];
    int n = 0;
    int abs;
    const char *p;
    size_t o;
    if (!in || !in[0] || !out || sz < 2) return 0;
    abs = in[0] == '/';
    p = in;
    while (*p) {
        const char *start;
        size_t len;
        while (*p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '/') p++;
        len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0 && strcmp(parts[n - 1], "..") != 0) n--;
            else if (!abs && n < 64) {
                memcpy(parts[n], "..", 3);
                n++;
            }
            continue;
        }
        if (n >= 64 || len >= 255) {
            snprintf(out, sz, "%s", in);
            return 1;
        }
        memcpy(parts[n], start, len);
        parts[n][len] = '\0';
        n++;
    }
    o = 0;
    if (abs) out[o++] = '/';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(parts[i]);
        if (o && out[o - 1] != '/') {
            if (o + 1 >= sz) return 0;
            out[o++] = '/';
        }
        if (o + len >= sz) return 0;
        memcpy(out + o, parts[i], len);
        o += len;
    }
    if (o == 0) out[o++] = abs ? '/' : '.';
    out[o] = '\0';
    return 1;
}

/** One absolute spelling. A relative entry (`packages/.../app.loam`) otherwise
 *  stores `packages/zeus`, while `LOAM_PATH` is absolute, and `loam_is_std_path`
 *  then misses `std/` seams. Bodyless host hooks get a second prototype
 *  (`int` is i32) that clashes with `zeus_rt.h`. */
static void canon_dir(char *norm, size_t sz) {
    char *rp = realpath(norm, NULL);
    if (!rp) return;
    if (strlen(rp) + 1 <= sz) memcpy(norm, rp, strlen(rp) + 1);
    free(rp);
}

static void add_dir(const char *dir, int from_vendor, int allow_unnamed, const char *sum) {
    char name[96], entry[256], base[96], toml[1100], norm[1024];
    name[0] = entry[0] = '\0';
    if (!dir || !dir[0]) return;
    if (!normalize_dir(dir, norm, sizeof norm)) return;
    canon_dir(norm, sizeof norm);
    dir = norm;
    if (!is_dir(dir)) return;
    snprintf(toml, sizeof toml, "%s/loam.toml", dir);
    if (file_exists(toml)) read_package_fields(toml, name, sizeof name, entry, sizeof entry);
    basename_of(dir, base, sizeof base);
    if (name[0]) {
        add_pkg(name, dir, entry, from_vendor, sum);
        if (base[0] && strcmp(base, name) != 0)
            add_pkg(base, dir, entry, from_vendor, sum);
    } else if (allow_unnamed && base[0]) {
        add_pkg(base, dir, entry, from_vendor, sum);
    }
}

static int parent_of(char *dir, size_t sz) {
    size_t n;
    char *slash;
    if (!dir[0] || strcmp(dir, "/") == 0) return 0;
    if (strcmp(dir, ".") == 0) {
        snprintf(dir, sz, "..");
        return 1;
    }
    n = strlen(dir);
    if (n >= 2 && dir[n - 1] == '.' && dir[n - 2] == '.') {
        if (n + 4 >= sz) return 0;
        memcpy(dir + n, "/..", 4);
        return 1;
    }
    slash = strrchr(dir, '/');
    if (!slash) {
        snprintf(dir, sz, ".");
        return strcmp(dir, ".") == 0;
    }
    if (slash == dir) {
        dir[1] = '\0';
        return 1;
    }
    *slash = '\0';
    return 1;
}

static void add_vendor(const char *appdir) {
    char vendor[1100];
    DIR *d;
    struct dirent *ent;
    snprintf(vendor, sizeof vendor, "%s/vendor", appdir);
    if (!is_dir(vendor)) return;
    d = opendir(vendor);
    if (!d) return;
    while ((ent = readdir(d))) {
        char pkgdir[1200];
        char sum[32];
        int listed = 0;
        if (ent->d_name[0] == '.') continue;
        snprintf(pkgdir, sizeof pkgdir, "%s/%s", vendor, ent->d_name);
        if (!is_dir(pkgdir)) continue;
        sum[0] = '\0';
        if (!lock_sum(appdir, ent->d_name, sum, sizeof sum, &listed) || !listed) continue;
        add_dir(pkgdir, 1, 1, sum);
    }
    closedir(d);
}

static void add_workspace_file(const char *toml, const char *dir) {
    char *members[64];
    int n = 0;
    int i;
    if (!read_workspace(toml, members, 64, &n)) {
        for (i = 0; i < n; i++) free(members[i]);
        return;
    }
    for (i = 0; i < n; i++) {
        char mem[1100];
        if (join2(mem, sizeof mem, dir, members[i])) add_dir(mem, 0, 0, NULL);
        free(members[i]);
    }
}

static int toml_has_workspace(const char *path) {
    char *members[4];
    int n = 0;
    int saw;
    int i;
    saw = read_workspace(path, members, 4, &n);
    for (i = 0; i < n; i++) free(members[i]);
    return saw;
}

static int toml_has_package(const char *path) {
    char name[96], entry[256];
    name[0] = '\0';
    read_package_fields(path, name, sizeof name, entry, sizeof entry);
    return name[0] != '\0';
}

static void add_loam_path(void) {
    char buf[1024];
    const char *roots = search_path(buf, sizeof buf);
    const char *p = roots;
    while (*p) {
        char dir[1024];
        size_t len;
        const char *end;
        while (*p == ':') p++;
        if (!*p) break;
        end = strchr(p, ':');
        len = end ? (size_t)(end - p) : strlen(p);
        if (len > 0 && len < sizeof dir) {
            memcpy(dir, p, len);
            dir[len] = '\0';
            add_dir(dir, 0, 1, NULL);
        }
        if (!end) break;
        p = end + 1;
    }
}

void loam_packages_discover(const char *anchor) {
    char dir[1024];
    int depth;
    int vendor_done = 0;
    g_npkgs = 0;
    clear_err();
    snprintf(g_anchor, sizeof g_anchor, "%s", anchor ? anchor : "");
    if (anchor && anchor[0]) {
        const char *slash = strrchr(anchor, '/');
        if (!slash) snprintf(dir, sizeof dir, ".");
        else if (slash == anchor) snprintf(dir, sizeof dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)(slash - anchor), anchor);
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    for (depth = 0; depth < 64; depth++) {
        char vendor[1100];
        char toml[1100];
        snprintf(vendor, sizeof vendor, "%s/vendor", dir);
        if (!vendor_done && is_dir(vendor)) {
            add_vendor(dir);
            vendor_done = 1;
        }
        snprintf(toml, sizeof toml, "%s/loam.toml", dir);
        if (file_exists(toml)) {
            if (toml_has_package(toml)) add_dir(dir, 0, 0, NULL);
            if (toml_has_workspace(toml)) {
                add_workspace_file(toml, dir);
                break;
            }
        }
        if (strcmp(dir, "/") == 0) break;
        if (!parent_of(dir, sizeof dir)) break;
    }
    add_loam_path();
}

int loam_package_entry(const char *name, char *out, size_t outsz) {
    Pkg *pk;
    clear_err();
    pk = find_pkg(name);
    if (!pk) return 0;
    if (!entry_file(pk, out, outsz)) {
        snprintf(g_err, sizeof g_err, "package '%s' has no entry file", name);
        return 0;
    }
    if (pk->from_vendor && !checksum_matches(pk)) return 0;
    return 1;
}

int loam_package_module(const char *pkg, const char *mod, char *out, size_t outsz) {
    Pkg *pk;
    clear_err();
    pk = find_pkg(pkg);
    if (!pk) return 0;
    if (!module_file(pk, mod, out, outsz)) return 0;
    if (pk->from_vendor && !checksum_matches(pk)) return 0;
    return 1;
}

static int std_name_exists(const char *name) {
    size_t e;
    char path[1200];
    if (!name || !name[0] || strchr(name, '/') || strchr(name, ':')) return 0;
    for (e = 0; e < LOAM_EXT_COUNT; e++) {
        snprintf(path, sizeof path, "%s/%s%s", LOAM_STD_DIR, name, loam_ext_name(e));
        if (file_exists(path)) return 1;
    }
    return 0;
}

static int vendor_dir_unlisted(const char *name, char *found, size_t foundsz) {
    char dir[1024];
    int depth;
    if (!g_anchor[0]) return 0;
    {
        const char *slash = strrchr(g_anchor, '/');
        if (!slash) snprintf(dir, sizeof dir, ".");
        else if (slash == g_anchor) snprintf(dir, sizeof dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)(slash - g_anchor), g_anchor);
    }
    for (depth = 0; depth < 64; depth++) {
        char pkg[1200];
        snprintf(pkg, sizeof pkg, "%s/vendor/%s", dir, name);
        if (is_dir(pkg)) {
            snprintf(found, foundsz, "%s", dir);
            return 1;
        }
        if (strcmp(dir, "/") == 0) break;
        if (!parent_of(dir, sizeof dir)) break;
    }
    return 0;
}

void loam_package_missing(const char *name, char *buf, size_t sz) {
    char app[1024];
    if (std_name_exists(name)) {
        snprintf(buf, sz,
                 "cannot find package '%s' (language modules use std:, as in import \"std:%s\")",
                 name, name);
        return;
    }
    if (vendor_dir_unlisted(name, app, sizeof app)) {
        snprintf(buf, sz,
                 "cannot find package '%s' (vendor/%s is not in loam.lock; run `zeli pkg sync`)",
                 name, name);
        return;
    }
    snprintf(buf, sz,
             "cannot find package '%s' (add it to loam.toml and run `zeli pkg sync`)", name);
}

const char *loam_package_label(const char *path) {
    int i;
    const char *best = NULL;
    size_t bestlen = 0;
    if (!path) return NULL;
    if (path_under(path, LOAM_STD_DIR)) return "std";
    for (i = 0; i < g_npkgs; i++) {
        size_t n = strlen(g_pkgs[i].dir);
        if (n > bestlen && path_under(path, g_pkgs[i].dir)) {
            best = g_pkgs[i].name;
            bestlen = n;
        }
    }
    return best;
}
