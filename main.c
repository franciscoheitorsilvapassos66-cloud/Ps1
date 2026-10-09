// Snake 3D (estilo Nokia) para PS1 - PSn00bSDK
// Otimizações: GTE (RTPS) p/ toda a matemática 3D, só inteiros, OT de 32 slots,
// só faces visíveis (3 quads por cubo), polígonos flat (sem textura),
// projeções do cenário pré-calculadas, zero malloc, double buffer.
#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxapi.h>

#define OT_LEN   32
#define PKT_LEN  32768
#define COLS     20
#define ROWS     12
#define CELL     64
#define HALF     28
#define DIST     2200
#define TILT_C   3138   /* cos(40 graus) em 4096 */
#define TILT_S   2633   /* sin(40 graus) em 4096 */
#define MAXLEN   (COLS * ROWS)
#define SLOT_FAR (OT_LEN - 1)

typedef struct {
    DISPENV disp; DRAWENV draw;
    uint32_t ot[OT_LEN]; uint8_t pkt[PKT_LEN];
} Buf;

typedef struct { uint8_t x, y; } Pt;

static Buf bufs[2]; static Buf *cur; static uint8_t *nextp; static int bi;
static uint8_t padbuf[2][34];
static MATRIX cam;
static int32_t ax[COLS], ay[ROWS], bz[ROWS];
static int16_t frm[4][2], flr[4][2];

/* paletas: topo, frente (sul), lado */
static const uint8_t C_BODY[3][3] = {{67,82,61},{40,50,37},{54,66,49}};
static const uint8_t C_HEAD[3][3] = {{30,40,28},{16,22,15},{23,31,21}};
static const uint8_t C_FOOD[3][3] = {{67,82,61},{40,50,37},{54,66,49}};
static const uint8_t C_DARK[3]  = {67,82,61};
static const uint8_t C_FLOOR[3] = {190,230,206};

/* ---------- jogo ---------- */
enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER };
static Pt snake[MAXLEN]; static int head, len;
static uint8_t occ[ROWS][COLS];
static int dirx, diry, ndx, ndy, score, hi, eaten, delay, tickc, state, frame;
static Pt food;
static uint32_t seed = 0x1234567;

static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void place_food(void) {
    int n = COLS * ROWS, s = rnd() % n;
    for (int k = 0; k < n; k++) {
        int i = (s + k) % n, x = i % COLS, y = i / COLS;
        if (!occ[y][x]) { food.x = x; food.y = y; return; }
    }
}

static void reset(void) {
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < COLS; x++) occ[y][x] = 0;
    len = 3; head = 2;
    for (int i = 0; i < 3; i++) { snake[i].x = 8 + i; snake[i].y = 6; occ[6][8 + i] = 1; }
    dirx = ndx = 1; diry = ndy = 0; score = 0; eaten = 0; delay = 10; tickc = 0;
    place_food();
}

static void die(void) { state = ST_OVER; if (score > hi) hi = score; }

static void step(void) {
    dirx = ndx; diry = ndy;
    Pt h = snake[head];
    int nx = h.x + dirx, ny = h.y + diry;
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { die(); return; }
    int eat = (nx == food.x && ny == food.y);
    if (!eat) { Pt t = snake[(head - len + 1 + MAXLEN) % MAXLEN]; occ[t.y][t.x] = 0; }
    if (occ[ny][nx]) { die(); return; }
    head = (head + 1) % MAXLEN;
    snake[head].x = nx; snake[head].y = ny; occ[ny][nx] = 1;
    if (eat) {
        len++; score += 7; eaten++;
        delay = 10 - eaten / 5; if (delay < 4) delay = 4;
        if (len >= MAXLEN) die(); else place_food();
    }
}

/* ---------- gráficos ---------- */
static void init_gfx(void) {
    ResetGraph(0);
    SetDefDispEnv(&bufs[0].disp, 0, 0, 320, 240);
    SetDefDrawEnv(&bufs[0].draw, 0, 240, 320, 240);
    SetDefDispEnv(&bufs[1].disp, 0, 240, 320, 240);
    SetDefDrawEnv(&bufs[1].draw, 0, 0, 320, 240);
    for (int i = 0; i < 2; i++) { bufs[i].draw.isbg = 1; setRGB0(&bufs[i].draw, 199, 240, 216); }
    cur = &bufs[0]; bi = 0;
    ClearOTagR(cur->ot, OT_LEN); nextp = cur->pkt;

    InitGeom();
    gte_SetGeomOffset(160, 138);
    gte_SetGeomScreen(400);
    cam.m[0][0] = 4096; cam.m[0][1] = 0;      cam.m[0][2] = 0;
    cam.m[1][0] = 0;    cam.m[1][1] = TILT_C; cam.m[1][2] = TILT_S;
    cam.m[2][0] = 0;    cam.m[2][1] = -TILT_S; cam.m[2][2] = TILT_C;
    gte_SetRotMatrix(&cam);

    for (int c = 0; c < COLS; c++) ax[c] = c * CELL - COLS * CELL / 2 + CELL / 2;
    for (int r = 0; r < ROWS; r++) {
        int wy = r * CELL - ROWS * CELL / 2 + CELL / 2;
        ay[r] = (TILT_C * wy) >> 12;
        bz[r] = DIST + ((-TILT_S * wy) >> 12);
    }

    /* cenário (moldura + chão): projetado UMA vez */
    static const int fx[4] = {-1, 1, -1, 1}, fy[4] = {-1, -1, 1, 1};
    SVECTOR z = {0, 0, 0, 0};
    for (int k = 0; k < 2; k++) {
        int hw = COLS * CELL / 2 + (k == 0 ? 20 : 0), hh = ROWS * CELL / 2 + (k == 0 ? 20 : 0);
        for (int i = 0; i < 4; i++) {
            MATRIX tm; int32_t s; int wy = fy[i] * hh;
            tm.t[0] = fx[i] * hw; tm.t[1] = (TILT_C * wy) >> 12; tm.t[2] = DIST + ((-TILT_S * wy) >> 12);
            gte_SetTransMatrix(&tm); gte_ldv0(&z); gte_rtps(); gte_stsxy(&s);
            int16_t (*dst)[2] = (k == 0) ? frm : flr;
            dst[i][0] = (int16_t)s; dst[i][1] = (int16_t)(s >> 16);
        }
    }
    FntLoad(960, 0);
    InitPAD(padbuf[0], 34, padbuf[1], 34); StartPAD(); ChangeClearPAD(0);
    SetDispMask(1);
}

static int room(int n) { return nextp + n * (int)sizeof(POLY_F4) < cur->pkt + PKT_LEN - 256; }

static void quad_xy(int x0,int y0,int x1,int y1,int x2,int y2,int x3,int y3,const uint8_t *c,int slot) {
    POLY_F4 *p = (POLY_F4 *)nextp; nextp += sizeof(POLY_F4);
    setPolyF4(p); setRGB0(p, c[0], c[1], c[2]);
    setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
    addPrim(&cur->ot[slot], p);
}
static void rect(int x0,int y0,int x1,int y1,const uint8_t *c,int slot) { quad_xy(x0,y0,x1,y0,x0,y1,x1,y1,c,slot); }
static void flat4(int16_t p[4][2], const uint8_t *c, int slot) {
    quad_xy(p[0][0],p[0][1],p[1][0],p[1][1],p[2][0],p[2][1],p[3][0],p[3][1],c,slot);
}

#define SX(i) ((int16_t)s[i])
#define SY(i) ((int16_t)(s[i] >> 16))
static void face(const int32_t *s,int a,int b,int c,int d,const uint8_t *col,int slot) {
    quad_xy(SX(a),SY(a),SX(b),SY(b),SX(c),SY(c),SX(d),SY(d),col,slot);
}

/* cubo de meia-aresta h na célula (cx,cy); desenha só 3 faces visíveis */
static void cube(int cx, int cy, int h, const uint8_t col[3][3]) {
    if (!room(3)) return;
    MATRIX tm; SVECTOR v = {0, 0, 0, 0}; int32_t s[8];
    tm.t[0] = ax[cx];
    tm.t[1] = ay[cy] - ((TILT_S * h) >> 12);
    tm.t[2] = bz[cy] - ((TILT_C * h) >> 12);
    gte_SetTransMatrix(&tm);
    for (int i = 0; i < 8; i++) {
        v.vx = (i & 1) ? h : -h; v.vy = (i & 2) ? h : -h; v.vz = (i & 4) ? h : -h;
        gte_ldv0(&v); gte_rtps(); gte_stsxy(&s[i]);
    }
    int slot = 2 + (ROWS - 1 - cy);
    face(s, 2, 3, 6, 7, col[1], slot);                       /* frente */
    if (cx < COLS / 2) face(s, 1, 3, 5, 7, col[2], slot);    /* lado leste */
    else               face(s, 0, 2, 4, 6, col[2], slot);    /* lado oeste */
    face(s, 0, 1, 2, 3, col[0], slot);                       /* topo */
}

static void text(int x, int y, const char *t) {
    nextp = (uint8_t *)FntSort(&cur->ot[0], nextp, x, y, t);
}

static void render(void) {
    char b[40];
    gte_SetRotMatrix(&cam);
    flat4(frm, C_DARK, SLOT_FAR);
    flat4(flr, C_FLOOR, SLOT_FAR - 1);

    for (int i = len - 1; i >= 0; i--) {
        Pt p = snake[(head - i + MAXLEN) % MAXLEN];
        cube(p.x, p.y, HALF, i == 0 ? C_HEAD : C_BODY);
    }
    if (state != ST_TITLE && ((frame >> 3) & 1)) cube(food.x, food.y, HALF / 2 + 3, C_FOOD); /* pisca como no Nokia */

    rect(0, 0, 320, 22, C_DARK, 1);
    sprintf(b, "SCORE %04d", score); text(10, 8, b);
    sprintf(b, "HI %04d", hi);       text(236, 8, b);

    if (state != ST_PLAY) {
        rect(64, 92, 256, 150, C_DARK, 1);
        if (state == ST_TITLE) { text(124, 100, "SNAKE 3D"); text(108, 126, "PRESS START"); }
        if (state == ST_PAUSE) { text(132, 100, "PAUSE");    text(108, 126, "PRESS START"); }
        if (state == ST_OVER)  { text(116, 100, "GAME OVER"); text(108, 126, "PRESS START"); }
    }
}

static void flip(void) {
    DrawSync(0); VSync(0);
    PutDispEnv(&cur->disp);
    DrawOTagEnv(&cur->ot[OT_LEN - 1], &cur->draw);
    bi ^= 1; cur = &bufs[bi];
    ClearOTagR(cur->ot, OT_LEN); nextp = cur->pkt;
}

int main(void) {
    init_gfx();
    reset();
    state = ST_TITLE;
    uint16_t prev = 0;
    for (;;) {
        frame++;
        PADTYPE *pad = (PADTYPE *)padbuf[0];
        uint16_t btn = (pad->stat == 0) ? (uint16_t)~pad->btn : 0;
        uint16_t press = btn & ~prev; prev = btn;

        if (press & PAD_START) {
            seed ^= (uint32_t)frame * 2654435761u;
            if (state == ST_TITLE) { reset(); state = ST_PLAY; }
            else if (state == ST_PLAY) state = ST_PAUSE;
            else if (state == ST_PAUSE) state = ST_PLAY;
            else if (state == ST_OVER) { reset(); state = ST_PLAY; }
        }
        if (state == ST_PLAY) {
            if ((btn & PAD_UP)    && diry == 0) { ndx = 0;  ndy = -1; }
            if ((btn & PAD_DOWN)  && diry == 0) { ndx = 0;  ndy = 1;  }
            if ((btn & PAD_LEFT)  && dirx == 0) { ndx = -1; ndy = 0;  }
            if ((btn & PAD_RIGHT) && dirx == 0) { ndx = 1;  ndy = 0;  }
            if (++tickc >= delay) { tickc = 0; step(); }
        }
        render();
        flip();
    }
    return 0;
}
