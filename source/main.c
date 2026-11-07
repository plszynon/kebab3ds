// Kebab Klikacz 2 - Nintendo 3DS port (core game)
// Data (upgrades / buildings) is generated from the original index2.html.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "data.h"

#define MAXB 160
#define MAXU 160
#define SAVE_DIR  "sdmc:/3ds/kebab-klikacz"
#define SAVE_FILE SAVE_DIR "/save.bin"
#define Z 0.5f

// ---------------------------------------------------------------- state
typedef struct {
    uint32_t magic, version;
    double kebabs, totalMade, clickPower, prestige, totalClicks;
    double owned[MAXB];
    uint8_t bought[MAXU];
} Save;

static Save G;
static int buyAmt = 1;            // 1, 10, 100, 0 = max
static int tab = 0;               // 0 click, 1 buildings, 2 upgrades, 3 prestige
static int cur[4];
static float scr[4];
static double prestigeArm = 0;    // seconds left to confirm prestige
static double saveTimer = 0;
static double clickPop = 0;

typedef struct { float x, y, life; char s[32]; } Floaty;
static Floaty fl[16];

static C2D_TextBuf tbuf;

// ---------------------------------------------------------------- auto-update (GitHub Releases)
#ifndef BUILD_NUM
#define BUILD_NUM 0
#endif
#ifndef UPDATE_REPO
#define UPDATE_REPO "plszynon/kebab3ds"
#endif
#define DEFAULT_SELF "sdmc:/3ds/kebab-klikacz.3dsx"

// 0 idle, 1 checking, 2 up to date, 3 update available, 4 downloading, 5 installed, 6 error
static volatile int updState = 0;
static volatile int remoteBuild = 0;
static volatile int updBusy = 0;
static char selfPath[256] = DEFAULT_SELF;

static int httpGet(const char *url0, u8 **outBuf, u32 *outSize) {
    char *newurl = malloc(0x1000);
    char cur[1024]; snprintf(cur, sizeof cur, "%s", url0);
    httpcContext ctx; u32 status = 0; int redirects = 0; Result ret;
    if (!newurl) return -1;
    for (;;) {
        ret = httpcOpenContext(&ctx, HTTPC_METHOD_GET, cur, 1);
        if (R_FAILED(ret)) { free(newurl); return -2; }
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
        httpcAddRequestHeaderField(&ctx, "User-Agent", "kebab-klikacz-3ds");
        ret = httpcBeginRequest(&ctx);
        if (R_FAILED(ret)) { httpcCloseContext(&ctx); free(newurl); return -3; }
        ret = httpcGetResponseStatusCode(&ctx, &status);
        if (R_FAILED(ret)) { httpcCloseContext(&ctx); free(newurl); return -4; }
        if ((status >= 301 && status <= 303) || status == 307 || status == 308) {
            if (++redirects > 6) { httpcCloseContext(&ctx); free(newurl); return -5; }
            ret = httpcGetResponseHeader(&ctx, "Location", newurl, 0x1000);
            httpcCloseContext(&ctx);
            if (R_FAILED(ret)) { free(newurl); return -6; }
            snprintf(cur, sizeof cur, "%s", newurl);
            continue;
        }
        break;
    }
    free(newurl);
    if (status != 200) { httpcCloseContext(&ctx); return -7; }
    u32 size = 0, rd = 0;
    u8 *buf = malloc(0x4000);
    if (!buf) { httpcCloseContext(&ctx); return -8; }
    u32 cap = 0x4000;
    do {
        if (cap - size < 0x2000) {
            u8 *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); httpcCloseContext(&ctx); return -9; }
            buf = nb; cap *= 2;
        }
        ret = httpcDownloadData(&ctx, buf + size, cap - size - 1, &rd);
        size += rd;
        if (size > 8 * 1024 * 1024) { free(buf); httpcCloseContext(&ctx); return -10; }
    } while (ret == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
    httpcCloseContext(&ctx);
    if (R_FAILED(ret)) { free(buf); return -11; }
    buf[size] = 0;
    *outBuf = buf; *outSize = size;
    return 0;
}

static void checkThread(void *arg) {
    (void)arg;
    char url[256]; u8 *buf = NULL; u32 sz = 0;
    updState = 1;
    snprintf(url, sizeof url, "https://github.com/%s/releases/download/latest/version.txt", UPDATE_REPO);
    if (httpGet(url, &buf, &sz) == 0 && sz > 0) {
        remoteBuild = atoi((char *)buf);
        free(buf);
        updState = remoteBuild > BUILD_NUM ? 3 : 2;
    } else updState = 6;
    updBusy = 0;
}

static void downloadThread(void *arg) {
    (void)arg;
    char url[256], tmp[300]; u8 *buf = NULL; u32 sz = 0;
    updState = 4;
    snprintf(url, sizeof url, "https://github.com/%s/releases/download/latest/kebab-klikacz.3dsx", UPDATE_REPO);
    if (httpGet(url, &buf, &sz) != 0 || sz < 4096 || memcmp(buf, "3DSX", 4) != 0) {
        if (buf) free(buf);
        updState = 6; updBusy = 0; return;
    }
    snprintf(tmp, sizeof tmp, "%s.new", selfPath);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(buf, 1, sz, f) == sz;
    if (f) fclose(f);
    free(buf);
    if (ok) {
        remove(selfPath);
        ok = rename(tmp, selfPath) == 0;
    }
    updState = ok ? 5 : 6;
    updBusy = 0;
}

static void startUpdateThread(void (*fn)(void *)) {
    if (updBusy) return;
    updBusy = 1;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(fn, NULL, 32 * 1024, prio - 1, -2, true);   // detached
    if (!t) { updBusy = 0; updState = 6; }
}

// ---------------------------------------------------------------- number formatting
typedef struct { double v; char n[16]; } Tier;
static Tier tiers[90];
static int ntiers = 0;

static void initTiers(void) {
    const char *base[10] = {"M","B","T","Qa","Qi","Sx","Sp","Oc","No","Dc"};
    const char *U[10]    = {"","U","D","T","Qa","Qi","Sx","Sp","Oc","No"};
    const char *TEN[10]  = {"","Dc","Vg","Tg","Qg","Qig","Sxg","Spg","Ocg","Nog"};
    for (int i = 0; i <= 81; i++) {
        int n = i + 1;
        tiers[ntiers].v = pow(10.0, 6 + i * 3);
        if (n <= 10) snprintf(tiers[ntiers].n, 16, "%s", base[i]);
        else snprintf(tiers[ntiers].n, 16, "%s%s", U[n % 10], TEN[n / 10]);
        ntiers++;
    }
    tiers[ntiers].v = 1e100; snprintf(tiers[ntiers].n, 16, "Googol"); ntiers++;
    tiers[ntiers].v = 1e250; snprintf(tiers[ntiers].n, 16, "Kebabilion"); ntiers++;
    for (int i = 1; i < ntiers; i++) {          // insertion sort by value
        Tier t = tiers[i]; int j = i - 1;
        while (j >= 0 && tiers[j].v > t.v) { tiers[j + 1] = tiers[j]; j--; }
        tiers[j + 1] = t;
    }
}

static void fmt(double n, char *out, size_t sz) {
    if (n < 0) n = 0;
    if (!isfinite(n)) { snprintf(out, sz, "inf"); return; }
    if (n < 1000) { snprintf(out, sz, "%d", (int)n); return; }
    if (n < 1e6)  { int v = (int)n; snprintf(out, sz, "%d %03d", v / 1000, v % 1000); return; }
    int t = 0;
    for (int i = ntiers - 1; i >= 0; i--) if (n >= tiers[i].v) { t = i; break; }
    double sc = n / tiers[t].v;
    if (t == ntiers - 1 && sc >= 1000) {
        char b[32]; snprintf(b, sizeof b, "%.2e", n);
        char *p = strchr(b, '+'); if (p) memmove(p, p + 1, strlen(p));
        snprintf(out, sz, "%s", b);
    } else snprintf(out, sz, "%.2f%s", sc, tiers[t].n);
}

// ---------------------------------------------------------------- game logic
static double prestigeMult(void) { return 1.0 + G.prestige * 0.1; }
static double effClick(void) { return G.clickPower * prestigeMult(); }
static double effCps(void) {
    double s = 0;
    for (int i = 0; i < NBLD; i++) s += BLDS[i].cps * G.owned[i];
    return s * prestigeMult();
}
static double bCost1(int i) { return ceil(BLDS[i].base * pow(1.15, G.owned[i])); }
static double bulkCost(int i, int n) {
    if (n <= 1) return bCost1(i);
    return ceil(BLDS[i].base * pow(1.15, G.owned[i]) * (pow(1.15, n) - 1) / 0.15);
}
static int bulkCount(int i) {
    if (buyAmt != 0) return buyAmt;
    double c0 = BLDS[i].base * pow(1.15, G.owned[i]);
    if (!(c0 > 0) || G.kebabs < c0) return 1;
    int n = (int)floor(log(G.kebabs * 0.15 / c0 + 1) / log(1.15));
    if (n < 1) n = 1;
    if (n > 100000) n = 100000;
    while (n > 1 && bulkCost(i, n) > G.kebabs) n--;
    return n;
}
static void buyBuilding(int i) {
    int n = bulkCount(i); double c = bulkCost(i, n);
    if (G.kebabs < c) return;
    G.kebabs -= c; G.owned[i] += n;
}
static void buyUpgrade(int i) {
    if (G.bought[i] || G.kebabs < UPGS[i].cost) return;
    G.kebabs -= UPGS[i].cost; G.bought[i] = 1;
    if (UPGS[i].mult) G.clickPower *= UPGS[i].val; else G.clickPower += UPGS[i].val;
}
static double prestigePotential(void) {
    return G.totalMade >= 1e6 ? floor(sqrt(G.totalMade / 1e6)) : 0;
}
static void doPrestige(void) {
    double g = prestigePotential();
    if (g < 1) return;
    G.prestige += g;
    G.kebabs = 0; G.totalMade = 0; G.clickPower = 1;
    memset(G.owned, 0, sizeof G.owned);
    memset(G.bought, 0, sizeof G.bought);
}
static int unbought[MAXU];
static int buildUnbought(void) {
    int n = 0;
    for (int i = 0; i < NUPG; i++) if (!G.bought[i]) unbought[n++] = i;
    return n;
}

// ---------------------------------------------------------------- save / load
static void newGame(void) {
    memset(&G, 0, sizeof G);
    G.magic = 0x4B4B3344; G.version = 1; G.clickPower = 1;
}
static void saveGame(void) {
    mkdir("sdmc:/3ds", 0777); mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return;
    fwrite(&G, sizeof G, 1, f); fclose(f);
}
static void loadGame(void) {
    newGame();
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) return;
    Save t;
    if (fread(&t, sizeof t, 1, f) == 1 && t.magic == 0x4B4B3344) G = t;
    fclose(f);
}

// ---------------------------------------------------------------- drawing helpers
static void txt(float x, float y, float s, u32 col, int al, const char *str) {
    C2D_Text t; C2D_TextParse(&t, tbuf, str); C2D_TextOptimize(&t);
    float w, h; C2D_TextGetDimensions(&t, s, s, &w, &h);
    if (al == 1) x -= w / 2; else if (al == 2) x -= w;
    C2D_DrawText(&t, C2D_WithColor, x, y, Z, s, s, col);
}
static void txtfit(float x, float y, float s, u32 col, int al, const char *str, float maxw) {
    C2D_Text t; C2D_TextParse(&t, tbuf, str); C2D_TextOptimize(&t);
    float w, h; C2D_TextGetDimensions(&t, s, s, &w, &h);
    if (w > maxw) { s *= maxw / w; w = maxw; }
    if (al == 1) x -= w / 2; else if (al == 2) x -= w;
    C2D_DrawText(&t, C2D_WithColor, x, y, Z, s, s, col);
}
static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, Z, w, h, c); }

#define COL(r,g,b) C2D_Color32(r, g, b, 255)
static u32 cBg, cPanel, cAccent, cWhite, cMuted, cOk, cBad, cDark;

static void addFloaty(float x, float y, const char *s) {
    for (int i = 0; i < 16; i++) if (fl[i].life <= 0) {
        fl[i].x = x; fl[i].y = y; fl[i].life = 1.0f;
        snprintf(fl[i].s, sizeof fl[i].s, "%s", s); return;
    }
}
static void doClick(float x, float y) {
    double p = effClick();
    G.kebabs += p; G.totalMade += p; G.totalClicks++;
    clickPop = 1.0;
    char b[24], s[32]; fmt(p, b, sizeof b); snprintf(s, sizeof s, "+%s", b);
    addFloaty(x, y, s);
}

// ---------------------------------------------------------------- list geometry
#define ROWH 40
#define LTOP 28
#define LBOT 212
#define VIEWH (LBOT - LTOP)

static int listCount(void) { return tab == 1 ? NBLD : (tab == 2 ? buildUnbought() : 0); }
static void clampScroll(int t, int n) {
    float mx = n * ROWH - VIEWH; if (mx < 0) mx = 0;
    if (scr[t] > mx) scr[t] = mx;
    if (scr[t] < 0) scr[t] = 0;
}
static void ensureVisible(int t) {
    float top = cur[t] * ROWH, bot = (cur[t] + 1) * ROWH;
    if (top < scr[t]) scr[t] = top;
    if (bot > scr[t] + VIEWH) scr[t] = bot - VIEWH;
}
static void activate(int row) {
    if (tab == 1 && row >= 0 && row < NBLD) buyBuilding(row);
    else if (tab == 2) { int n = buildUnbought(); if (row >= 0 && row < n) buyUpgrade(unbought[row]); }
}

// ---------------------------------------------------------------- screens
static void drawTop(void) {
    char a[40], b[96];
    txt(200, 6, 0.8f, cAccent, 1, "Kebab Klikacz 2");
    txt(200, 38, 0.5f, cMuted, 1, "Kebaby");
    fmt(G.kebabs, a, sizeof a);
    txtfit(200, 52, 1.2f, cWhite, 1, a, 380);
    fmt(effCps(), a, sizeof a); snprintf(b, sizeof b, "%s / s", a);
    txtfit(200, 98, 0.65f, cOk, 1, b, 380);

    rect(20, 130, 360, 84, cPanel);
    fmt(effClick(), a, sizeof a); snprintf(b, sizeof b, "Siła kliknięcia: %s", a);
    txtfit(30, 136, 0.5f, cWhite, 0, b, 340);
    fmt(G.totalMade, a, sizeof a); snprintf(b, sizeof b, "Wyprodukowano łącznie: %s", a);
    txtfit(30, 156, 0.5f, cWhite, 0, b, 340);
    fmt(G.totalClicks, a, sizeof a); snprintf(b, sizeof b, "Kliknięcia: %s", a);
    txtfit(30, 176, 0.5f, cWhite, 0, b, 340);
    { char p[24]; fmt(G.prestige, a, sizeof a); fmt(G.prestige * 10, p, sizeof p); snprintf(b, sizeof b, "Prestiż: %s  (bonus +%s%%)", a, p); }
    txtfit(30, 196, 0.5f, cAccent, 0, b, 340);

    {
        char m[96]; u32 col = cMuted;
        switch (updState) {
            case 1: snprintf(m, sizeof m, "Szukam aktualizacji..."); break;
            case 2: snprintf(m, sizeof m, "Masz najnowszą wersję (build %d)", BUILD_NUM); col = cOk; break;
            case 3: snprintf(m, sizeof m, "Dostępna aktualizacja (build %d)! Naciśnij X", (int)remoteBuild); col = cAccent; break;
            case 4: snprintf(m, sizeof m, "Pobieranie aktualizacji..."); col = cAccent; break;
            case 5: snprintf(m, sizeof m, "Zaktualizowano! Wyjdź (START) i uruchom grę ponownie"); col = cOk; break;
            case 6: snprintf(m, sizeof m, "Brak połączenia lub błąd (SELECT = ponów)"); col = cBad; break;
            default: snprintf(m, sizeof m, "Wersja: build %d", BUILD_NUM); break;
        }
        txtfit(200, 112, 0.42f, col, 1, m, 380);
    }
    txtfit(200, 220, 0.4f, cMuted, 1, "L/R zakładki  Y ilość  X aktualizuj  SELECT sprawdź  START wyjście", 390);
}

static void drawTabs(void) {
    const char *names[4] = {"Klik", "Budynki", "Ulepsz.", "Prestiż"};
    rect(0, 0, 320, 26, cDark);
    for (int i = 0; i < 4; i++) {
        float x = i * 80;
        rect(x + 2, 2, 76, 22, i == tab ? cAccent : cPanel);
        txt(x + 40, 5, 0.5f, i == tab ? cDark : cWhite, 1, names[i]);
    }
}

static void drawList(void) {
    int n = listCount();
    clampScroll(tab, n);
    for (int k = 0; k < n; k++) {
        float y = LTOP + k * ROWH - scr[tab];
        if (y + ROWH < LTOP || y > LBOT) continue;
        char a[40], b[80], c[80];
        int idx = k; double cost; int afford;
        const char *name;
        if (tab == 1) {
            int cnt = bulkCount(k); cost = bulkCost(k, cnt);
            name = BLDS[k].name; afford = G.kebabs >= cost;
            fmt(G.owned[k], a, sizeof a); snprintf(b, sizeof b, "Posiadasz: %s", a);
            fmt(cost, a, sizeof a);
            if (cnt > 1) snprintf(c, sizeof c, "%s (x%d)", a, cnt); else snprintf(c, sizeof c, "%s", a);
        } else {
            idx = unbought[k]; cost = UPGS[idx].cost;
            name = UPGS[idx].name; afford = G.kebabs >= cost;
            char v[40]; fmt(UPGS[idx].val, v, sizeof v);
            snprintf(b, sizeof b, UPGS[idx].mult ? "x%s za klik" : "+%s za klik", v);
            fmt(cost, c, sizeof c);
        }
        rect(4, y + 2, 312, ROWH - 4, k == cur[tab] ? COL(0x5a, 0x32, 0x10) : cPanel);
        rect(4, y + 2, 4, ROWH - 4, afford ? cOk : cBad);
        txtfit(14, y + 5, 0.48f, cWhite, 0, name, 190);
        txtfit(14, y + 22, 0.4f, cMuted, 0, b, 190);
        txtfit(310, y + 14, 0.48f, afford ? cOk : cBad, 2, c, 100);
    }
    if (tab == 2 && n == 0) txt(160, 100, 0.6f, cOk, 1, "Wszystkie ulepszenia kupione!");
    // header/footer cover overflow
    rect(0, 0, 320, LTOP, cDark);
    rect(0, LBOT, 320, 240 - LBOT, cDark);
    drawTabs();
    char s[64];
    if (tab == 1) {
        snprintf(s, sizeof s, "Kupno: %s [Y]", buyAmt == 0 ? "Max" : (buyAmt == 1 ? "x1" : (buyAmt == 10 ? "x10" : "x100")));
        txt(10, 220, 0.45f, cAccent, 0, s);
        txt(310, 220, 0.4f, cMuted, 2, "A kup  D-pad/przeciągnij");
    } else {
        snprintf(s, sizeof s, "Pozostało: %d / %d", n, NUPG);
        txt(10, 220, 0.45f, cAccent, 0, s);
        txt(310, 220, 0.4f, cMuted, 2, "A kup  D-pad/przeciągnij");
    }
}

static void drawClick(float dt) {
    float sc = 1.0f + (float)clickPop * 0.12f;
    C2D_DrawCircleSolid(160, 118, Z, 78 * sc, COL(0x6b, 0x34, 0x10));
    C2D_DrawCircleSolid(160, 118, Z, 66 * sc, COL(0xc8, 0x6a, 0x20));
    rect(156, 30, 8, 176, COL(0xb0, 0xb0, 0xb8));   // skewer
    C2D_DrawCircleSolid(160, 118, Z, 40 * sc, COL(0xe8, 0xa8, 0x50));
    txt(160, 108, 0.7f, cDark, 1, "KLIK!");
    clickPop -= dt * 6; if (clickPop < 0) clickPop = 0;
    for (int i = 0; i < 16; i++) if (fl[i].life > 0) {
        fl[i].life -= dt * 1.2f; fl[i].y -= dt * 50;
        u8 al = (u8)(255 * (fl[i].life > 0 ? fl[i].life : 0));
        txt(fl[i].x, fl[i].y, 0.55f, C2D_Color32(255, 255, 255, al), 1, fl[i].s);
    }
    rect(0, 0, 320, LTOP, cDark);
    rect(0, LBOT, 320, 240 - LBOT, cDark);
    drawTabs();
    char a[40], s[64]; fmt(effClick(), a, sizeof a); snprintf(s, sizeof s, "+%s na klik   (A lub dotknij)", a);
    txtfit(160, 220, 0.45f, cAccent, 1, s, 300);
}

static void drawPrestige(void) {
    rect(0, 0, 320, 240, cBg);
    drawTabs();
    char a[40], b[96];
    txt(160, 40, 0.7f, cAccent, 1, "Prestiż (Odrodzenie)");
    txtfit(160, 70, 0.45f, cWhite, 1, "Zresetuj postęp za stały bonus +10% za punkt.", 300);
    fmt(prestigePotential(), a, sizeof a);
    snprintf(b, sizeof b, "Zdobędziesz teraz: %s pkt", a);
    txtfit(160, 95, 0.55f, cOk, 1, b, 300);
    txtfit(160, 118, 0.42f, cMuted, 1, "Wymaga min. 1 000 000 wyprodukowanych kebabów.", 300);
    int can = prestigePotential() >= 1;
    float bx = 50, by = 150, bw = 220, bh = 48;
    rect(bx, by, bw, bh, can ? (prestigeArm > 0 ? cBad : cAccent) : cPanel);
    txt(160, by + 12, 0.65f, can ? cDark : cMuted, 1,
        can ? (prestigeArm > 0 ? "NA PEWNO? Dotknij ponownie" : "PRESTIŻ") : "Za mało kebabów");
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0 && strlen(argv[0]) < sizeof selfPath)
        snprintf(selfPath, sizeof selfPath, "%s", argv[0]);
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(16384);
    hidSetRepeatParameters(18, 4);

    cBg = COL(0x1a, 0x0a, 0x00); cPanel = COL(0x33, 0x1c, 0x0a); cAccent = COL(0xff, 0x9a, 0x2a);
    cWhite = COL(0xff, 0xf4, 0xe0); cMuted = COL(0xb0, 0x9a, 0x80); cOk = COL(0x6e, 0xe0, 0x6e);
    cBad = COL(0xe0, 0x5a, 0x4a); cDark = COL(0x10, 0x06, 0x00);

    initTiers();
    loadGame();
    int netOk = R_SUCCEEDED(httpcInit(0));
    if (netOk) startUpdateThread(checkThread); else updState = 6;

    u64 last = osGetTime();
    touchPosition tp, tstart; int touching = 0, dragged = 0; int lastY = 0;

    while (aptMainLoop()) {
        u64 now = osGetTime();
        float dt = (now - last) / 1000.0f; last = now;
        if (dt > 1.0f) dt = 1.0f;

        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld(), ku = hidKeysUp(), kr = hidKeysDownRepeat();
        if (kd & KEY_START) break;

        // passive income
        double inc = effCps() * dt;
        G.kebabs += inc; G.totalMade += inc;
        if (prestigeArm > 0) prestigeArm -= dt;
        saveTimer += dt;
        if (saveTimer > 30) { saveGame(); saveTimer = 0; }

        // buttons
        if (kd & KEY_L) tab = (tab + 3) % 4;
        if (kd & KEY_R) tab = (tab + 1) % 4;
        if ((kd & KEY_SELECT) && !updBusy && updState != 5) startUpdateThread(checkThread);
        if ((kd & KEY_X) && !updBusy && updState == 3) startUpdateThread(downloadThread);
        if (kd & KEY_Y) buyAmt = buyAmt == 1 ? 10 : (buyAmt == 10 ? 100 : (buyAmt == 100 ? 0 : 1));
        if (tab == 1 || tab == 2) {
            int n = listCount();
            if (kr & KEY_DOWN) { if (cur[tab] < n - 1) cur[tab]++; ensureVisible(tab); }
            if (kr & KEY_UP)   { if (cur[tab] > 0) cur[tab]--; ensureVisible(tab); }
            if (cur[tab] >= n) cur[tab] = n > 0 ? n - 1 : 0;
            if (kd & KEY_A) activate(cur[tab]);
        } else if (tab == 0) {
            if (kd & KEY_A) doClick(160, 100);
        } else if (tab == 3) {
            if (kd & KEY_A) { if (prestigeArm > 0) { doPrestige(); saveGame(); prestigeArm = 0; } else if (prestigePotential() >= 1) prestigeArm = 3; }
        }

        // touch
        if (kd & KEY_TOUCH) {
            hidTouchRead(&tp); tstart = tp; touching = 1; dragged = 0; lastY = tp.py;
            if (tp.py < 26) tab = tp.px / 80 > 3 ? 3 : tp.px / 80;
            else if (tab == 0 && tp.py < LBOT) doClick((float)tp.px, (float)tp.py - 20);
        } else if ((kh & KEY_TOUCH) && touching) {
            hidTouchRead(&tp);
            if (abs((int)tp.py - (int)tstart.py) > 8) dragged = 1;
            if (dragged && (tab == 1 || tab == 2)) { scr[tab] -= (float)((int)tp.py - lastY); clampScroll(tab, listCount()); }
            lastY = tp.py;
        }
        if ((ku & KEY_TOUCH) && touching) {
            touching = 0;
            if (!dragged && tstart.py >= 26) {
                if (tab == 1 || tab == 2) {
                    if (tstart.py >= LTOP && tstart.py < LBOT) {
                        int row = (int)((tstart.py - LTOP + scr[tab]) / ROWH);
                        if (row >= 0 && row < listCount()) { cur[tab] = row; activate(row); }
                    }
                } else if (tab == 3) {
                    if (tstart.px >= 50 && tstart.px <= 270 && tstart.py >= 150 && tstart.py <= 198) {
                        if (prestigeArm > 0) { doPrestige(); saveGame(); prestigeArm = 0; }
                        else if (prestigePotential() >= 1) prestigeArm = 3;
                    }
                }
            }
        }

        // draw
        C2D_TextBufClear(tbuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, cBg); C2D_TargetClear(bot, cBg);
        C2D_SceneBegin(top); drawTop();
        C2D_SceneBegin(bot);
        if (tab == 0) drawClick(dt); else if (tab == 3) drawPrestige(); else drawList();
        C3D_FrameEnd(0);
    }

    saveGame();
    if (netOk && !updBusy) httpcExit();
    C2D_TextBufDelete(tbuf);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}
