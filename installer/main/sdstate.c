/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdstate.h"
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool sdstate_load(const char *state_path, stockrec_t *s)
{
    memset(s, 0, sizeof(*s));
    FILE *f = fopen(state_path, "r");
    if (!f) return false;
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "stock=", 6)) { copy_str(s->path, sizeof(s->path), line + 6); }
        else if (!strncmp(line, "sha=", 4)) { copy_str(s->sha, sizeof(s->sha), line + 4); }
    }
    fclose(f);
    s->have = s->path[0] && strlen(s->sha) == 64;
    return s->have;
}

bool sdstate_save(const char *state_path, const stockrec_t *s)
{
    FILE *f = fopen(state_path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "stock=%s\nsha=%s\n", s->path, s->sha) > 0;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    return ok;
}
