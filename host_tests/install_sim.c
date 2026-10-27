/* SPDX-License-Identifier: GPL-3.0-only
 * Host test for components/fm1install: a simulated FM-1 (running firmware -> loader -> new firmware) on a virtual clock.
 * The simulation follows what Felucca's fm1_install.py expects of a real device. It checks that the installer serves
 * exactly the package, in every step, and that it fails safely when the device misbehaves. */
#include "fm1install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG 100003u    /* logical image size used in the tests (odd on purpose) */

typedef enum { S_RUN, S_CHECK, S_GONE1, S_LOADER, S_WRITE, S_GONE2, S_NEW, S_DEAD } sst_t;
typedef enum { M_NORMAL, M_NOASK, M_DIE_WRITE, M_NOLOADER, M_OTHERID, M_BADREQ, M_STALL_WRITE } mode_t_;

typedef struct {
    sst_t st; mode_t_ mode; uint32_t T, until;
    uint32_t next; int outstanding; uint32_t o_addr, o_len; bool o_finish;
    uint8_t image[IMG], flash[IMG]; uint8_t flash_set[IMG];
    char run_id[28], new_id[28];
    uint8_t pend[128]; int pend_n;
    int upgrades, check_chunks, write_chunks, bad;
    int read_fail_at;
} sim_t;
static sim_t S;
static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) fails++; } while (0)

static size_t make_identity(const char *txt, uint8_t *out)
{
    uint8_t d[34] = {0};
    d[0] = 0x00; d[1] = 0x59; d[2] = 0x11;
    memcpy(d + 6, txt, strlen(txt));
    out[0] = 0xF0;
    size_t p = fm1_pack7(d, sizeof d, out + 1);
    out[1 + p] = 0xF7;
    return p + 2;
}

static size_t make_request(uint32_t addr, uint32_t len, uint8_t *out)
{
    uint8_t u[15] = {0x00, 0x59, 0x30, 0, 0, 0, 0};
    u[6] = 0;
    u[7] = (uint8_t)addr; u[8] = (uint8_t)(addr >> 8); u[9] = (uint8_t)(addr >> 16); u[10] = (uint8_t)(addr >> 24);
    u[11] = (uint8_t)len; u[12] = (uint8_t)(len >> 8); u[13] = 0;
    uint8_t sum = 0; for (int i = 6; i < 14; i++) sum = (uint8_t)(sum + u[i]);
    u[14] = (uint8_t)~sum;
    out[0] = 0xF0;
    size_t p = fm1_pack7(u, sizeof u, out + 1);
    out[1 + p] = 0xF7;
    return p + 2;
}

static void tick(void)
{
    if (S.st == S_GONE1 && S.T >= S.until) S.st = (S.mode == M_NOLOADER) ? S_DEAD : S_LOADER;
    if (S.st == S_GONE2 && S.T >= S.until) S.st = S_NEW;
}

static bool is_ready(void *c) { (void)c; tick(); return S.st != S_GONE1 && S.st != S_GONE2 && S.st != S_DEAD; }
static void flush(void *c) { (void)c; S.pend_n = 0; }
static void sleepms(void *c, uint32_t ms) { (void)c; S.T += ms; }
static uint32_t nowms(void *c) { (void)c; return S.T; }
static void prog(void *c, const char *st, int v) { (void)c; (void)st; (void)v; }

static bool read_image(void *c, uint32_t addr, size_t len, uint8_t *out)
{
    (void)c;
    if (S.read_fail_at >= 0 && (int)addr >= S.read_fail_at) return false;
    memcpy(out, S.image + addr, len);
    return true;
}

static const char *current_id(void)
{
    switch (S.st) {
    case S_LOADER: case S_WRITE: return "ota-FM-1_900";
    case S_NEW: return S.mode == M_OTHERID ? "FM-1_777" : S.new_id;
    default: return S.run_id;
    }
}

static int recvf(void *c, uint8_t *buf, size_t cap, uint32_t ms)
{
    (void)c; tick();
    if (S.st == S_GONE1 || S.st == S_GONE2 || S.st == S_DEAD) { S.T += ms; return -1; }
    if (S.pend_n) { int n = S.pend_n; memcpy(buf, S.pend, (size_t)n); S.pend_n = 0; return n; }
    if ((S.st == S_CHECK || S.st == S_WRITE) && !S.outstanding) {
        if (S.mode == M_NOASK && S.st == S_CHECK) { S.T += ms; return 0; }
        if (S.mode == M_STALL_WRITE && S.st == S_WRITE && S.next > IMG / 2) { S.T += ms; return 0; }
        uint32_t a, l; bool fin = false;
        if (S.mode == M_BADREQ && S.st == S_CHECK && S.next > 5000) { a = IMG - 100; l = 512; }
        else if (S.next < IMG) { a = S.next; l = IMG - a < 512 ? IMG - a : 512; }
        else { a = S.st == S_CHECK ? FM1_FINISH_CHECK : FM1_FINISH_WRITE; l = 0; fin = true; }
        S.outstanding = 1; S.o_addr = a; S.o_len = l; S.o_finish = fin;
        size_t n = make_request(a, l, buf); (void)cap;
        return (int)n;
    }
    S.T += ms;
    return 0;
}

static bool sendf(void *c, const uint8_t *p, size_t n)
{
    (void)c; tick();
    if (S.st == S_GONE1 || S.st == S_GONE2 || S.st == S_DEAD) return false;
    if (n == sizeof(FM1_HS_QUERY) && !memcmp(p, FM1_HS_QUERY, n)) { S.pend_n = (int)make_identity(current_id(), S.pend); return true; }
    if (n == sizeof(FM1_UPGRADE) && !memcmp(p, FM1_UPGRADE, n)) {
        S.upgrades++;
        if (S.st == S_RUN) { S.st = S_CHECK; S.next = 0; S.outstanding = 0; }
        else if (S.st == S_LOADER) { S.st = S_WRITE; S.next = 0; S.outstanding = 0; }
        return true;
    }
    /* a response to our outstanding request */
    uint8_t u[700];
    if (p[0] != 0xF0 || p[n - 1] != 0xF7) { S.bad++; return true; }
    size_t k = fm1_unpack7(p + 1, n - 2, u);
    if (!S.outstanding || k < 15 || u[0] != 0 || u[1] != 0x59 || u[2] != 0x30) { S.bad++; return true; }
    uint8_t sum = 0; for (size_t i = 6; i < k - 1; i++) sum = (uint8_t)(sum + u[i]);
    if ((uint8_t)~sum != u[k - 1]) { S.bad++; return true; }
    uint32_t a = u[7] | (u[8] << 8) | (u[9] << 16) | ((uint32_t)u[10] << 24);
    uint32_t l = u[11] | (u[12] << 8);
    if (a != S.o_addr) { S.bad++; return true; }
    S.outstanding = 0;
    if (S.o_finish) {
        if (l != 8 || memcmp(u + 14, "success", 7)) S.bad++;
        if (S.st == S_CHECK) { S.st = S_GONE1; S.until = S.T + 1500; }
        else { S.st = (S.mode == M_DIE_WRITE) ? S_DEAD : S_GONE2; S.until = S.T + 4000; }
        return true;
    }
    if (l != S.o_len || memcmp(u + 14, S.image + a, l)) { S.bad++; return true; }
    if (S.st == S_CHECK) S.check_chunks++;
    else {
        S.write_chunks++; memcpy(S.flash + a, u + 14, l); memset(S.flash_set + a, 1, l);
        if (S.mode == M_DIE_WRITE && a > IMG / 3) { S.st = S_DEAD; return true; }
    }
    S.next = a + l;
    return true;
}

static void reset(mode_t_ m, const char *run_id, const char *new_id, sst_t start)
{
    memset(&S, 0, sizeof S);
    for (uint32_t i = 0; i < IMG; i++) S.image[i] = (uint8_t)(i * 2654435761u >> 13);
    S.mode = m; S.st = start; S.read_fail_at = -1;
    strcpy(S.run_id, run_id); strcpy(S.new_id, new_id);
}

static fm1i_err_t run(const char *dev_text, const char *product, fm1_identity_t *fin)
{
    fm1_io_t io = { .ctx = NULL, .send = sendf, .recv = recvf, .device_ready = is_ready, .flush_rx = flush,
                    .read_image = read_image, .sleep_ms = sleepms, .now_ms = nowms, .progress = prog };
    fm1_identity_t dev, back;
    uint8_t pkt[128];
    size_t n = make_identity(dev_text, pkt);
    if (!fm1_parse_identity(pkt, n, &dev)) { printf("bad test identity\n"); exit(2); }
    return fm1i_install(&io, &FM1_DELAYS_REAL, &dev, product, IMG, fin ? fin : &back);
}

static bool flash_equals_image(void)
{
    for (uint32_t i = 0; i < IMG; i++) if (!S.flash_set[i] || S.flash[i] != S.image[i]) return false;
    return true;
}
static bool flash_untouched(void) { for (uint32_t i = 0; i < IMG; i++) if (S.flash_set[i]) return false; return true; }

int main(void)
{
    fm1_identity_t fin;
    uint32_t chunks = (IMG + 511) / 512;

    reset(M_NORMAL, "FM-1_900", "FM-1_015", S_RUN);
    fm1i_err_t e = run("FM-1_900", "FM-1_015", &fin);
    CHECK("custom -> stock: OK", e == FM1I_OK);
    CHECK("final identity is the package's", !strcmp(fin.text, "FM-1_015"));
    CHECK("flash holds exactly the package", flash_equals_image());
    CHECK("check step served every chunk once", S.check_chunks == (int)chunks);
    CHECK("write step served every chunk once", S.write_chunks == (int)chunks);
    CHECK("upgrade sent exactly twice", S.upgrades == 2);
    CHECK("every answer was well-formed and matched", S.bad == 0);
    CHECK("it took a plausible virtual time (> 10 s)", S.T > 10000);

    reset(M_NORMAL, "FM-1_900", "FM-1_016", S_LOADER);
    e = run("ota-FM-1_900", "FM-1_016", &fin);
    CHECK("device already in the loader: only the write is done", e == FM1I_OK && S.upgrades == 1 && S.check_chunks == 0);
    CHECK("... and the flash is right", flash_equals_image() && !strcmp(fin.text, "FM-1_016"));

    reset(M_NORMAL, "FM-2_900", "FM-1_015", S_RUN);
    e = run("FM-2_900", "FM-1_015", NULL);
    CHECK("wrong model is refused before anything is sent", e == FM1I_MODEL && S.upgrades == 0);

    reset(M_NOASK, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("FM-1 never asks in the check step -> STOPPED, nothing written", e == FM1I_STOPPED && flash_untouched() && S.upgrades == 1);

    reset(M_BADREQ, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("request outside the package -> BADREQ, nothing written", e == FM1I_BADREQ && flash_untouched());

    reset(M_NOLOADER, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("loader never appears -> NOLOADER, nothing written", e == FM1I_NOLOADER && flash_untouched());

    reset(M_DIE_WRITE, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("unplugged during the write -> LOST", e == FM1I_LOST);

    reset(M_STALL_WRITE, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("loader stalls during the write -> STOPPED (after the idle time)", e == FM1I_STOPPED && S.T > 180000);

    reset(M_OTHERID, "FM-1_900", "FM-1_015", S_RUN);
    e = run("FM-1_900", "FM-1_015", &fin);
    CHECK("comes back as another firmware -> MISMATCH", e == FM1I_MISMATCH && !strcmp(fin.text, "FM-1_777"));

    reset(M_NORMAL, "FM-1_900", "FM-1_015", S_RUN);
    S.read_fail_at = 20000;
    e = run("FM-1_900", "FM-1_015", NULL);
    CHECK("card read error during the check step -> READ, nothing written", e == FM1I_READ && flash_untouched());

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
