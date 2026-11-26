// Lane Rush - a small MOBA-style lane game for Nintendo 3DS (libctru + citro2d)
// Break the red tower, then the red nexus, before the red team breaks yours.
// Controls: Circle Pad / D-Pad = move (your hero attacks nearby enemies by itself)
//           A (or touch DASH) = dash strike | B (or touch BURST) = shockwave | START = quit
// Top screen: the lane. Bottom screen: minimap, nexus health and the two skill buttons.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define RGB(r, g, b)     RGBA((r), (g), (b), 255)

#define SCR_W 400
#define SCR_H 240
#define BOT_W 320
#define WORLD_W 1600           // the lane is 1600 pixels long, the camera follows your hero
#define LANE_Y 120             // centre line of the lane
#define LANE_HALF 30           // half the lane's width
#define MAX_ENT 40
#define MAX_FX 40
#define MAX_SPAWN 16
#define WAVE_EVERY 15.0f       // seconds between minion waves
#define DT (1.0f / 60.0f)

enum { K_MINION, K_HERO, K_TOWER, K_NEXUS };
enum { ST_MENU, ST_PLAY, ST_OVER };

typedef struct {
	int kind, team;            // team 0 = blue (you), 1 = red
	float x, y, r;
	float hp, max, atk, rng, spd, acd, cd, face;
	int lvl;
	float xp, s1, s2, rt;      // xp, skill cooldowns, respawn timer
	bool low;                  // the AI hero is retreating to heal
} Ent;

typedef struct { int kind; float x1, y1, x2, y2, t, r; int team, val; } Fx; // 0 line, 1 ring, 2 number
typedef struct { float at; int team; } Spawn;

// entity slots: 0..3 buildings (team * 2 + isNexus), 4 = your hero, 5 = red hero, 6.. = minions
#define HERO0 4
#define FIRST_MINION 6

static Ent E[MAX_ENT];
static int nE;
static Fx fxs[MAX_FX];
static int nFx;
static Spawn spawnQ[MAX_SPAWN];
static int nQ;
static float T, nextWave, camx;
static int kills, state = ST_MENU;
static C2D_TextBuf textBuf;

static const u32 TEAM_COL[2] = { RGB(58, 160, 255), RGB(255, 90, 106) };

// ---------------------------------------------------------------- small helpers
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float dist(const Ent *a, const Ent *b) { return hypotf(a->x - b->x, a->y - b->y); }
static bool towerUp(int team) { return E[team * 2].hp > 0; }

static void addFx(int kind, float x1, float y1, float x2, float y2, float t, float r, int team, int val) {
	if (nFx >= MAX_FX) nFx = MAX_FX - 1;          // overwrite the newest one when full
	fxs[nFx++] = (Fx){ kind, x1, y1, x2, y2, t, r, team, val };
}

static void setEnt(Ent *e, int kind, int team) {
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->team = team;
	e->face = team ? 3.14159f : 0.0f;
	switch (kind) {
	case K_MINION: e->r = 5;  e->hp = e->max = 260;  e->atk = 14; e->rng = 14; e->spd = 26; e->acd = 0.9f;  break;
	case K_HERO:   e->r = 8;  e->hp = e->max = 900;  e->atk = 46; e->rng = 30; e->spd = 56; e->acd = 0.65f; e->lvl = 1; break;
	case K_TOWER:  e->r = 12; e->hp = e->max = 1800; e->atk = 70; e->rng = 84; e->acd = 1.2f; break;
	default:       e->r = 16; e->hp = e->max = 3000; break;           // nexus
	}
}

static void resetGame(void) {
	nE = FIRST_MINION;
	nFx = 0;
	nQ = 0;
	T = 0;
	nextWave = 2;
	kills = 0;
	camx = 0;
	for (int t = 0; t < 2; t++) {
		setEnt(&E[t * 2], K_TOWER, t);
		E[t * 2].x = t ? WORLD_W - 260 : 260;
		setEnt(&E[t * 2 + 1], K_NEXUS, t);
		E[t * 2 + 1].x = t ? WORLD_W - 60 : 60;
		setEnt(&E[HERO0 + t], K_HERO, t);
		E[HERO0 + t].x = t ? WORLD_W - 90 : 90;
		E[t * 2].y = E[t * 2 + 1].y = E[HERO0 + t].y = LANE_Y;
	}
}

// ---------------------------------------------------------------- combat
// who can this unit hit right now? returns an index into E, or -1
static int pick(const Ent *u, float range) {
	int best = -1;
	float bs = 1e9f;
	for (int i = 0; i < nE; i++) {
		const Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0) continue;
		if (e->kind == K_NEXUS && towerUp(e->team)) continue;     // the nexus is shielded by its tower
		float d = dist(u, e) - e->r;
		if (d > range) continue;
		float s = d + ((u->kind == K_TOWER && e->kind == K_HERO) ? 40.0f : 0.0f); // towers prefer minions
		if (s < bs) { bs = s; best = i; }
	}
	return best;
}

static void gainXp(Ent *h, float xp) {
	h->xp += xp;
	while (h->xp >= 90.0f * h->lvl) {
		h->xp -= 90.0f * h->lvl;
		h->lvl++;
		h->max = floorf(h->max * 1.12f);
		h->hp = fminf(h->max, h->hp + h->max * 0.3f);
		h->atk = floorf(h->atk * 1.1f);
		addFx(1, h->x, h->y, 0, 0, 0.6f, 20, h->team, 0);
	}
}

static void endGame(bool win);

static void die(int i) {
	Ent *t = &E[i];
	t->hp = 0;
	float xp = t->kind == K_NEXUS ? 0 : t->kind == K_TOWER ? 90 : t->kind == K_HERO ? 110 : 18;
	if (t->kind == K_HERO) {
		t->rt = 5.0f + t->lvl * 1.5f;
		if (t->team == 1) kills++;
	}
	for (int h = HERO0; h < HERO0 + 2; h++)
		if (E[h].team != t->team && E[h].hp > 0 && dist(&E[h], t) < 150) gainXp(&E[h], xp);
	addFx(1, t->x, t->y, 0, 0, 0.45f, t->r * 2, t->team, 0);
	if (t->kind == K_NEXUS) endGame(t->team == 1);
}

static void hit(int i, float amount) {
	Ent *t = &E[i];
	if (t->kind == K_NEXUS && towerUp(t->team)) return;
	t->hp -= amount;
	if (t->kind == K_HERO) addFx(2, t->x, t->y - t->r - 4, 0, 0, 0.7f, 0, t->team, (int)amount);
	if (t->hp <= 0 && t->max > 0) die(i);
}

static float segDist(float x1, float y1, float x2, float y2, float px, float py) {
	float dx = x2 - x1, dy = y2 - y1, l = dx * dx + dy * dy;
	if (l < 0.001f) l = 0.001f;
	float t = clampf(((px - x1) * dx + (py - y1) * dy) / l, 0, 1);
	return hypotf(px - (x1 + t * dx), py - (y1 + t * dy));
}

static void dash(Ent *u) {                       // a quick strike forward that hurts everything it passes
	if (u->s1 > 0 || u->hp <= 0) return;
	u->s1 = 5;
	float x1 = u->x, y1 = u->y;
	u->x = clampf(u->x + cosf(u->face) * 60, 10, WORLD_W - 10);
	u->y = clampf(u->y + sinf(u->face) * 60, LANE_Y - LANE_HALF - 20, LANE_Y + LANE_HALF + 20);
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || e->kind >= K_TOWER) continue;
		if (segDist(x1, y1, u->x, u->y, e->x, e->y) < e->r + 10) hit(i, 70 + u->lvl * 14);
	}
	addFx(0, x1, y1, u->x, u->y, 0.25f, 4, u->team, 0);
}

static void burst(Ent *u) {                      // a shockwave around the hero
	if (u->s2 > 0 || u->hp <= 0) return;
	u->s2 = 9;
	const float R = 54;
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0) continue;
		if (dist(u, e) < R + e->r) hit(i, e->kind >= K_TOWER ? 60 : 110 + u->lvl * 20);
	}
	addFx(1, u->x, u->y, 0, 0, 0.5f, R, u->team, 0);
}

// ---------------------------------------------------------------- AI
static void toward(const Ent *u, float x, float y, float *mx, float *my) {
	float dx = x - u->x, dy = y - u->y, l = hypotf(dx, dy);
	if (l < 4) { *mx = *my = 0; return; }
	*mx = dx / l;
	*my = dy / l;
}

static void aiMinion(Ent *u, float *mx, float *my) {
	int tg = pick(u, u->rng);
	if (tg >= 0) {                               // in range: stand and fight
		u->face = atan2f(E[tg].y - u->y, E[tg].x - u->x);
		*mx = *my = 0;
		return;
	}
	int n = pick(u, 68);
	if (n >= 0) { toward(u, E[n].x, E[n].y, mx, my); return; }
	*mx = u->team ? -1.0f : 1.0f;                // otherwise march down the lane
	*my = (LANE_Y - u->y) * 0.02f;
}

static void aiHero(Ent *u, float *mx, float *my) {
	float homeX = u->team ? WORLD_W - 90 : 90;
	*mx = *my = 0;
	if (u->hp < u->max * 0.3f) u->low = true;
	if (u->hp >= u->max * 0.9f) u->low = false;
	if (u->low) { toward(u, homeX, LANE_Y, mx, my); return; }
	int tg = pick(u, 144);
	if (tg >= 0) {
		Ent *t = &E[tg];
		bool escorted = false;                   // never push a tower without your minions
		for (int i = FIRST_MINION; i < nE; i++)
			if (E[i].team == u->team && E[i].hp > 0 && dist(&E[i], t) < 92) escorted = true;
		if (t->kind == K_TOWER && !escorted) {
			if (dist(u, t) < t->rng + 28) *mx = u->team ? 1.0f : -1.0f;
			return;
		}
		float d = dist(u, t) - t->r;
		u->face = atan2f(t->y - u->y, t->x - u->x);
		if (d < 60 && t->kind == K_HERO && u->s1 <= 0 && rand() % 33 == 0) dash(u);
		if (d < 50 && u->s2 <= 0) burst(u);
		if (d > u->rng * 0.85f) toward(u, t->x, t->y, mx, my);
		return;
	}
	float front = -1;                            // follow the furthest minion of its own team
	for (int i = FIRST_MINION; i < nE; i++) {
		if (E[i].team != u->team || E[i].hp <= 0) continue;
		if (front < 0 || (u->team ? E[i].x < front : E[i].x > front)) front = E[i].x;
	}
	toward(u, front < 0 ? homeX : front + (u->team ? 20 : -20), LANE_Y, mx, my);
}

// ---------------------------------------------------------------- game loop
static void endGame(bool win) {
	(void)win;
	state = ST_OVER;
}

static bool blueWon(void) { return E[3].hp <= 0; }  // E[3] is the red nexus: blue wins when it falls

static void addMinion(int team) {
	if (nE >= MAX_ENT) return;
	Ent *m = &E[nE++];
	setEnt(m, K_MINION, team);
	m->x = team ? WORLD_W - 70 : 70;
	m->y = LANE_Y + (rand() % 21) - 10;
}

static void update(u32 kDown, u32 kHeld, touchPosition tp) {
	Ent *me = &E[HERO0];
	T += DT;
	if (T >= nextWave) {
		nextWave += WAVE_EVERY;
		for (int i = 0; i < 3 && nQ + 2 <= MAX_SPAWN; i++) {
			spawnQ[nQ++] = (Spawn){ T + i * 0.7f, 0 };
			spawnQ[nQ++] = (Spawn){ T + i * 0.7f, 1 };
		}
	}
	for (int i = nQ - 1; i >= 0; i--)
		if (spawnQ[i].at <= T) { addMinion(spawnQ[i].team); spawnQ[i] = spawnQ[--nQ]; }

	// your hero: skills first, then movement
	if (kDown & KEY_A) dash(me);
	if (kDown & KEY_B) burst(me);
	if (kDown & KEY_TOUCH) {
		if (tp.py >= 150 && tp.py < 220) {
			if (tp.px >= 20 && tp.px < 150) dash(me);
			else if (tp.px >= 170 && tp.px < 300) burst(me);
		}
	}
	circlePosition cp;
	hidCircleRead(&cp);

	for (int i = HERO0; i < nE; i++) {
		Ent *u = &E[i];
		if (u->hp <= 0) {                        // a fallen hero waits, then returns to base
			if (u->kind == K_HERO) {
				u->rt -= DT;
				if (u->rt <= 0) {
					u->hp = u->max;
					u->x = u->team ? WORLD_W - 90 : 90;
					u->y = LANE_Y;
				}
			}
			continue;
		}
		u->cd -= DT; u->s1 -= DT; u->s2 -= DT;
		float mx = 0, my = 0;
		if (u == me) {
			mx = (abs(cp.dx) > 20 ? cp.dx / 156.0f : 0) + ((kHeld & KEY_DRIGHT) ? 1 : 0) - ((kHeld & KEY_DLEFT) ? 1 : 0);
			my = -(abs(cp.dy) > 20 ? cp.dy / 156.0f : 0) + ((kHeld & KEY_DDOWN) ? 1 : 0) - ((kHeld & KEY_DUP) ? 1 : 0);
			float l = hypotf(mx, my);
			if (l > 1) { mx /= l; my /= l; }
		} else if (u->kind == K_HERO) aiHero(u, &mx, &my);
		else aiMinion(u, &mx, &my);
		if (mx != 0 || my != 0) {
			u->x = clampf(u->x + mx * u->spd * DT, 10, WORLD_W - 10);
			u->y = clampf(u->y + my * u->spd * DT, LANE_Y - LANE_HALF - 20, LANE_Y + LANE_HALF + 20);
			u->face = atan2f(my, mx);
		}
		int tg = pick(u, u->rng);                // auto-attack the nearest enemy in range
		if (tg >= 0) {
			if (mx == 0 && my == 0) u->face = atan2f(E[tg].y - u->y, E[tg].x - u->x);
			if (u->cd <= 0) {
				u->cd = u->acd;
				addFx(0, u->x, u->y, E[tg].x, E[tg].y, 0.14f, 1.5f, u->team, 0);
				hit(tg, u->atk);
			}
		}
		if (u->kind == K_HERO && dist(u, &E[u->team * 2 + 1]) < 90)   // heal next to your own nexus
			u->hp = fminf(u->max, u->hp + 90 * DT);
	}

	for (int i = HERO0; i < nE; i++)             // units push each other apart a little
		for (int j = i + 1; j < nE; j++) {
			Ent *a = &E[i], *b = &E[j];
			if (a->hp <= 0 || b->hp <= 0) continue;
			float d = dist(a, b), m = a->r + b->r;
			if (d < m && d > 0.01f) {
				float p = (m - d) * 0.25f / d;
				a->x -= (b->x - a->x) * p; a->y -= (b->y - a->y) * p;
				b->x += (b->x - a->x) * p; b->y += (b->y - a->y) * p;
			}
		}

	for (int t = 0; t < 2; t++) {                // towers shoot whatever is in range
		Ent *b = &E[t * 2];
		if (b->hp <= 0) continue;
		b->cd -= DT;
		int tg = pick(b, b->rng);
		if (tg >= 0 && b->cd <= 0) {
			b->cd = b->acd;
			addFx(0, b->x, b->y - 8, E[tg].x, E[tg].y, 0.18f, 2.5f, b->team, 0);
			hit(tg, b->atk);
		}
	}

	for (int i = nE - 1; i >= FIRST_MINION; i--) // remove fallen minions
		if (E[i].hp <= 0) E[i] = E[--nE];
	for (int i = nFx - 1; i >= 0; i--) {
		fxs[i].t -= DT;
		if (fxs[i].t <= 0) fxs[i] = fxs[--nFx];
	}
	camx += ((me->x - SCR_W / 2) - camx) * 0.12f;
	camx = clampf(camx, 0, WORLD_W - SCR_W);
}

// ---------------------------------------------------------------- drawing
static void fillr(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0, w, h, c); }
static void disc(float x, float y, float r, u32 c) { C2D_DrawCircleSolid(x, y, 0, r, c); }

static void text(float x, float y, float s, u32 c, const char *str) {
	C2D_Text t;
	C2D_TextParse(&t, textBuf, str);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, s, s, c);
}

static void textC(float cx, float y, float s, u32 c, const char *str) {
	C2D_Text t;
	float w, h;
	C2D_TextParse(&t, textBuf, str);
	C2D_TextOptimize(&t);
	C2D_TextGetDimensions(&t, s, s, &w, &h);
	C2D_DrawText(&t, C2D_WithColor, cx - w / 2, y, 0.5f, s, s, c);
}

static void bar(float cx, float y, float w, float f, u32 c) {
	f = clampf(f, 0, 1);
	fillr(cx - w / 2 - 1, y - 1, w + 2, 5, RGBA(0, 0, 0, 170));
	fillr(cx - w / 2, y, w * f, 3, c);
}

static void drawEnt(const Ent *e, float ox) {
	float x = e->x - ox, y = e->y;
	if (x < -30 || x > SCR_W + 30) return;
	if (e->kind == K_TOWER || e->kind == K_NEXUS) {
		if (e->hp <= 0) { disc(x, y, e->r * 0.8f, RGBA(0, 0, 0, 130)); return; }
		disc(x, y + e->r * 0.5f, e->r * 1.05f, RGBA(0, 0, 0, 90));
		if (e->kind == K_NEXUS) {                // a diamond
			C2D_DrawTriangle(x, y - e->r * 1.3f, TEAM_COL[e->team], x + e->r, y, TEAM_COL[e->team], x - e->r, y, TEAM_COL[e->team], 0);
			C2D_DrawTriangle(x, y + e->r * 1.3f, TEAM_COL[e->team], x + e->r, y, TEAM_COL[e->team], x - e->r, y, TEAM_COL[e->team], 0);
			if (towerUp(e->team)) disc(x, y, 4, RGBA(255, 255, 255, 140));   // shielded
		} else {
			disc(x, y, e->r, RGB(11, 30, 43));
			disc(x, y, e->r - 2, TEAM_COL[e->team]);
			disc(x, y, 4, RGB(255, 255, 255));
		}
		bar(x, y - e->r * 1.5f - 6, e->r * 2, e->hp / e->max, TEAM_COL[e->team]);
		return;
	}
	if (e->hp <= 0) return;
	bool hero = e->kind == K_HERO;
	disc(x, y + e->r * 0.8f, e->r * 0.9f, RGBA(0, 0, 0, 90));
	disc(x, y, e->r + (hero ? 1.5f : 1.0f), hero ? RGB(255, 255, 255) : RGB(11, 30, 43));
	disc(x, y, e->r, TEAM_COL[e->team]);
	disc(x + cosf(e->face) * e->r * 0.5f, y + sinf(e->face) * e->r * 0.5f, e->r * 0.28f, RGB(11, 30, 43));
	if (e == &E[HERO0]) C2D_DrawTriangle(x, y - e->r - 6, RGB(255, 200, 87), x - 3, y - e->r - 12, RGB(255, 200, 87), x + 3, y - e->r - 12, RGB(255, 200, 87), 0);
	bar(x, y - e->r - 5, hero ? 22 : 12, e->hp / e->max, e->team ? RGB(255, 138, 150) : RGB(124, 196, 255));
}

static void drawTop(void) {
	float ox = camx;
	fillr(0, 0, SCR_W, SCR_H, RGB(15, 42, 51));
	fillr(-ox, 0, 210, SCR_H, RGBA(58, 160, 255, 40));                    // the two bases
	fillr(WORLD_W - 210 - ox, 0, 210, SCR_H, RGBA(255, 90, 106, 40));
	fillr(WORLD_W / 2 - 12 - ox, 0, 24, SCR_H, RGBA(255, 255, 255, 14));   // the middle of the map
	fillr(0, LANE_Y - LANE_HALF, SCR_W, LANE_HALF * 2, RGB(23, 64, 75));   // the lane
	for (int x = ((int)(ox / 40)) * 40; x < ox + SCR_W; x += 40) fillr(x - ox, LANE_Y - 1, 16, 2, RGB(42, 98, 114));

	for (int i = 0; i < 4; i++) drawEnt(&E[i], ox);
	for (int i = FIRST_MINION; i < nE; i++) drawEnt(&E[i], ox);
	for (int h = HERO0; h < HERO0 + 2; h++) drawEnt(&E[h], ox);

	char buf[48];
	for (int i = 0; i < nFx; i++) {
		const Fx *f = &fxs[i];
		u32 c = RGBA(255, 255, 255, (int)(255 * clampf(f->t * 3, 0, 1)));
		if (f->kind == 0) C2D_DrawLine(f->x1 - ox, f->y1, c, f->x2 - ox, f->y2, c, f->r, 0);
		else if (f->kind == 1) {
			float rr = f->r * (1.2f - f->t);
			u32 rc = (TEAM_COL[f->team] & 0x00FFFFFF) | ((u32)(110 * clampf(f->t * 3, 0, 1)) << 24);
			disc(f->x1 - ox, f->y1, rr, rc);        // a soft expanding wave
		} else {
			snprintf(buf, sizeof(buf), "%d", f->val);
			text(f->x1 - ox - 6, f->y1 - (0.7f - f->t) * 20, 0.4f, RGBA(255, 200, 87, (int)(255 * clampf(f->t * 3, 0, 1))), buf);
		}
	}

	// HUD
	const Ent *me = &E[HERO0];
	snprintf(buf, sizeof(buf), "Level %d%s", me->lvl, me->hp <= 0 ? "  - back in a moment" : "");
	text(6, 3, 0.45f, RGB(255, 255, 255), buf);
	fillr(6, 20, 90, 6, RGBA(0, 0, 0, 170));
	fillr(6, 20, 90 * clampf(me->hp / me->max, 0, 1), 6, RGB(124, 196, 255));
	fillr(6, 28, 90 * clampf(me->xp / (90.0f * me->lvl), 0, 1), 2, RGB(255, 200, 87));
	snprintf(buf, sizeof(buf), "%d:%02d   kills %d", (int)T / 60, (int)T % 60, kills);
	textC(SCR_W / 2, 3, 0.45f, RGB(255, 255, 255), buf);

	if (state != ST_PLAY) {
		fillr(0, 0, SCR_W, SCR_H, RGBA(11, 30, 43, 190));
		if (state == ST_MENU) {
			textC(SCR_W / 2, 70, 1.1f, RGB(255, 255, 255), "Lane Rush");
			textC(SCR_W / 2, 118, 0.5f, RGB(214, 232, 245), "Break the red tower, then the red nexus.");
			textC(SCR_W / 2, 136, 0.5f, RGB(214, 232, 245), "Circle Pad moves, A dashes, B bursts.");
			textC(SCR_W / 2, 176, 0.6f, RGB(255, 200, 87), "Press A to start");
		} else {
			bool win = blueWon();
			textC(SCR_W / 2, 66, 1.1f, win ? RGB(124, 220, 160) : RGB(255, 138, 150), win ? "Victory" : "Defeat");
			snprintf(buf, sizeof(buf), "Time %d:%02d   kills %d   level %d", (int)T / 60, (int)T % 60, kills, me->lvl);
			textC(SCR_W / 2, 122, 0.5f, RGB(214, 232, 245), buf);
			textC(SCR_W / 2, 164, 0.6f, RGB(255, 200, 87), "Press A to play again, START to quit");
		}
	}
}

static void drawButton(float x, const char *label, float cd, float maxcd, u32 col) {
	fillr(x, 150, 130, 70, RGB(11, 30, 51));
	fillr(x + 3, 153, 124, 64, col);
	if (cd > 0) {                                // darkens from the top while recharging
		fillr(x + 3, 153, 124, 64 * clampf(cd / maxcd, 0, 1), RGBA(0, 0, 0, 170));
		char b[8];
		snprintf(b, sizeof(b), "%d", (int)ceilf(cd));
		textC(x + 65, 175, 0.8f, RGB(255, 255, 255), b);
	} else textC(x + 65, 175, 0.8f, RGB(255, 255, 255), label);
}

static void drawBottom(void) {
	fillr(0, 0, BOT_W, SCR_H, RGB(14, 33, 51));
	text(10, 6, 0.5f, RGB(214, 232, 245), "Minimap");
	fillr(10, 28, 300, 36, RGB(23, 64, 75));
	for (int i = 0; i < nE; i++) {               // every living unit is a dot
		const Ent *e = &E[i];
		if (e->hp <= 0) continue;
		float x = 10 + e->x / WORLD_W * 300, y = 46 + (e->y - LANE_Y) * 0.3f;
		float s = e->kind >= K_TOWER ? 3.5f : e->kind == K_HERO ? 3.0f : 1.6f;
		disc(x, y, s, e->kind == K_HERO ? RGB(255, 255, 255) : TEAM_COL[e->team]);
		if (e->kind == K_HERO) disc(x, y, s - 1.2f, TEAM_COL[e->team]);
	}
	float vx = 10 + camx / WORLD_W * 300, vw = SCR_W / (float)WORLD_W * 300;   // what the top screen shows
	fillr(vx, 28, vw, 1, RGB(255, 200, 87)); fillr(vx, 63, vw, 1, RGB(255, 200, 87));
	fillr(vx, 28, 1, 36, RGB(255, 200, 87)); fillr(vx + vw, 28, 1, 36, RGB(255, 200, 87));

	text(10, 74, 0.45f, TEAM_COL[0], "Blue nexus");
	fillr(10, 92, 140, 8, RGBA(0, 0, 0, 170));
	fillr(10, 92, 140 * clampf(E[1].hp / E[1].max, 0, 1), 8, TEAM_COL[0]);
	text(170, 74, 0.45f, TEAM_COL[1], "Red nexus");
	fillr(170, 92, 140, 8, RGBA(0, 0, 0, 170));
	fillr(170, 92, 140 * clampf(E[3].hp / E[3].max, 0, 1), 8, TEAM_COL[1]);
	text(10, 108, 0.4f, RGB(150, 180, 200), towerUp(1) ? "Red tower still stands: its nexus is shielded." : "Red tower is down: attack the nexus!");
	text(10, 124, 0.4f, RGB(150, 180, 200), towerUp(0) ? "Your tower is holding." : "Your tower fell: protect your nexus!");

	drawButton(20, "DASH (A)", E[HERO0].s1, 5, RGB(58, 120, 200));
	drawButton(170, "BURST (B)", E[HERO0].s2, 9, RGB(200, 90, 80));
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(2048);
	C2D_Prepare();
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	textBuf = C2D_TextBufNew(2048);
	srand((unsigned)time(NULL));
	resetGame();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		touchPosition tp = { 0, 0 };
		if (kHeld & KEY_TOUCH) hidTouchRead(&tp);

		if (state == ST_PLAY) update(kDown, kHeld, tp);
		else if (kDown & KEY_A) { resetGame(); state = ST_PLAY; }

		C2D_TextBufClear(textBuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, RGB(15, 42, 51));
		C2D_SceneBegin(top);
		drawTop();
		C2D_TargetClear(bot, RGB(14, 33, 51));
		C2D_SceneBegin(bot);
		drawBottom();
		C3D_FrameEnd(0);
	}

	C2D_TextBufDelete(textBuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
