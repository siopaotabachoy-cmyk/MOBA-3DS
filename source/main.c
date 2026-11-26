// Lane Rush - a small MOBA game with three lanes and a jungle, for Nintendo 3DS (libctru + citro2d)
// Break any red tower, then the red nexus, before the red team breaks yours.
// Controls: Circle Pad / D-Pad = move | Y or R (or touch ATTACK, hold it) = attack | SELECT = auto-attack on/off
//           A (or touch DASH) = dash strike | B (or touch BURST) = shockwave
//           X (or touch ULT) = ultimate, unlocks at level 4 | START = quit
// Top screen: the map around your hero. Bottom screen: minimap, nexus health and the three skill buttons.
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
#define WORLD 1200             // the map is a 1200 x 1200 square, the camera follows your hero
#define LANE_W 52              // width of a lane
#define LANES 3                // top, middle, bottom
#define PATH_PTS 6
#define MAX_ENT 64
#define MAX_FX 40
#define MAX_SPAWN 16
#define WAVE_EVERY 20.0f       // seconds between minion waves
#define DT (1.0f / 60.0f)
#define ULT_LEVEL 4
#define ULT_CD 30.0f
#define ULT_RADIUS 90.0f

enum { K_MINION, K_HERO, K_CREEP, K_TOWER, K_NEXUS };
#define NEUTRAL 2              // jungle monsters belong to no team
enum { ST_LOBBY, ST_HELP, ST_PLAY, ST_OVER, ST_PICK };   // ST_PICK = choose your hero
enum { M_BATTLE, M_HELP, M_COUNT };   // lobby menu entries

typedef struct {
	int kind, team;            // team 0 = blue (you), 1 = red, 2 = jungle
	int lane, wp;              // minions: which lane, and the next point on it
	float hx, hy;              // jungle monsters: home position
	float x, y, r;
	float hp, max, atk, rng, spd, acd, cd, face;
	int lvl;
	int type;                  // heroes: which character (index into HEROES)
	float xp, s1, s2, s3, rt;  // xp, skill cooldowns, respawn timer
	bool low;                  // the AI hero is retreating to heal
	float walk;                // walking animation
} Ent;

// ---- heroes you can choose from ----
#define NHEROES 4
typedef struct {
	const char *name, *role, *d1, *d2;   // shown on the selection screen
	float hp, atk, rng, spd, acd;        // basic stats
	float dash, burst, ult;              // dash distance, burst and ultimate radius
	u32 acc, card;                       // accent colour and card colour
} HeroDef;

static const HeroDef HEROES[NHEROES] = {
	{ "Blue Knight", "Fighter", "Well rounded: good health and damage.", "A safe pick for your first match.",
	  900, 46, 30, 74, 0.65f, 60, 54, 90, RGB(58, 160, 255), RGB(56, 110, 220) },
	{ "Lyra", "Marksman", "Shoots from far away, but is fragile.", "Keep your distance and kite enemies.",
	  650, 40, 70, 78, 0.70f, 50, 48, 85, RGB(90, 220, 130), RGB(36, 130, 90) },
	{ "Grom", "Tank", "Huge health and a wide shockwave.", "Slow, but very hard to take down.",
	  1500, 38, 26, 62, 0.80f, 45, 70, 110, RGB(255, 160, 70), RGB(176, 84, 36) },
	{ "Vex", "Assassin", "Very fast with the hardest hits.", "Long dash, low health. Strike and run.",
	  620, 54, 24, 92, 0.55f, 85, 44, 80, RGB(190, 130, 255), RGB(100, 56, 190) },
};

typedef struct { int kind; float x1, y1, x2, y2, t, r; int team, val; } Fx; // 0 line, 1 ring, 2 number
typedef struct { float at; int lane; } Spawn;   // one minion for each team in this lane

// entity slots: 0..7 buildings (team * 4 + lane for towers, team * 4 + 3 for the nexus),
// 8 = your hero, 9 = red hero, 10..16 = jungle monsters, 17.. = minions
#define NEXUS(team) ((team) * 4 + 3)
#define BUILDINGS 8
#define HERO0 8
#define FIRST_CREEP 10
#define CREEPS 7
#define FIRST_MINION (FIRST_CREEP + CREEPS)

// each lane as a path from the blue base to the red base (red walks it backwards)
static const float PATH[LANES][PATH_PTS][2] = {
	{ {110, 1090}, {110, 250}, {160, 160}, {250, 110}, {700, 110}, {1090, 110} },      // top
	{ {110, 1090}, {306, 894}, {502, 698}, {698, 502}, {894, 306}, {1090, 110} },      // middle
	{ {110, 1090}, {500, 1090}, {950, 1090}, {1040, 1040}, {1090, 950}, {1090, 110} }, // bottom
};
static const float TOWER_POS[2][LANES][2] = {
	{ {110, 640}, {390, 810}, {560, 1090} },     // blue
	{ {640, 110}, {810, 390}, {1090, 560} },     // red
};
// jungle camps; the last one is the River Guardian
static const float CAMP[CREEPS][2] = {
	{300, 520}, {640, 320}, {680, 880}, {900, 680}, {520, 270}, {930, 840}, {420, 420},
};

static Ent E[MAX_ENT];
static int nE;
static Fx fxs[MAX_FX];
static int nFx;
static Spawn spawnQ[MAX_SPAWN];
static int nQ;
static float T, nextWave, camx, camy;
static int kills, state = ST_LOBBY;
static C2D_TextBuf textBuf;
static bool won;
static int menuSel = M_BATTLE;
static bool autoAtk;                       // SELECT turns your hero's automatic attacks on or off
static int pickSel, myHero, enemyHero = 1;   // hero selection: highlighted card, your hero, the red hero
static int played, victories;        // lobby record, kept until the game is closed
static int frame;                    // drives the lobby animation
static int knightClock;              // where the knight is in his animation loop

static const u32 TEAM_COL[3] = { RGB(58, 160, 255), RGB(255, 90, 106), RGB(190, 130, 255) };

// ---------------------------------------------------------------- small helpers
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float dist(const Ent *a, const Ent *b) { return hypotf(a->x - b->x, a->y - b->y); }
// the nexus is shielded until at least one of its team's towers has fallen
static bool shielded(int team) { return E[team * 4].hp > 0 && E[team * 4 + 1].hp > 0 && E[team * 4 + 2].hp > 0; }
static float homeX(int team) { return team ? WORLD - 150 : 150; }
static float homeY(int team) { return team ? 150 : WORLD - 150; }

static void addFx(int kind, float x1, float y1, float x2, float y2, float t, float r, int team, int val) {
	if (nFx >= MAX_FX) nFx = MAX_FX - 1;          // overwrite the newest one when full
	fxs[nFx++] = (Fx){ kind, x1, y1, x2, y2, t, r, team, val };
}

static void setEnt(Ent *e, int kind, int team) {
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->team = team;
	e->face = team == 1 ? 2.356f : -0.785f;
	switch (kind) {
	case K_MINION: e->r = 5;  e->hp = e->max = 260;  e->atk = 14; e->rng = 14; e->spd = 34; e->acd = 0.9f;  break;
	case K_HERO:   e->r = 8;  e->hp = e->max = 900;  e->atk = 46; e->rng = 30; e->spd = 74; e->acd = 0.65f; e->lvl = 1; break;
	case K_CREEP:  e->r = 9;  e->hp = e->max = 520;  e->atk = 24; e->rng = 22; e->spd = 40; e->acd = 1.1f;  break;
	case K_TOWER:  e->r = 12; e->hp = e->max = 1800; e->atk = 70; e->rng = 84; e->acd = 1.2f; break;
	default:       e->r = 16; e->hp = e->max = 3000; break;           // nexus
	}
}

static void applyHero(Ent *e, int type) {        // give a hero the stats of the chosen character
	const HeroDef *d = &HEROES[type];
	e->type = type;
	e->hp = e->max = d->hp;
	e->atk = d->atk;
	e->rng = d->rng;
	e->spd = d->spd;
	e->acd = d->acd;
}

static void resetGame(void) {
	nE = FIRST_MINION;
	nFx = 0;
	nQ = 0;
	T = 0;
	nextWave = 3;
	kills = 0;
	for (int t = 0; t < 2; t++) {
		for (int l = 0; l < LANES; l++) {
			Ent *tw = &E[t * 4 + l];
			setEnt(tw, K_TOWER, t);
			tw->x = TOWER_POS[t][l][0];
			tw->y = TOWER_POS[t][l][1];
		}
		Ent *nx = &E[NEXUS(t)];
		setEnt(nx, K_NEXUS, t);
		nx->x = PATH[1][t ? PATH_PTS - 1 : 0][0];
		nx->y = PATH[1][t ? PATH_PTS - 1 : 0][1];
		Ent *h = &E[HERO0 + t];
		setEnt(h, K_HERO, t);
		applyHero(h, t ? enemyHero : myHero);
		h->x = homeX(t);
		h->y = homeY(t);
	}
	for (int i = 0; i < CREEPS; i++) {
		Ent *c = &E[FIRST_CREEP + i];
		setEnt(c, K_CREEP, NEUTRAL);
		c->x = c->hx = CAMP[i][0];
		c->y = c->hy = CAMP[i][1];
		if (i == CREEPS - 1) {                   // the River Guardian is much tougher
			c->r = 15; c->hp = c->max = 2200; c->atk = 55; c->rng = 30; c->acd = 1.4f;
		}
	}
	camx = clampf(E[HERO0].x - SCR_W / 2, 0, WORLD - SCR_W);
	camy = clampf(E[HERO0].y - SCR_H / 2, 0, WORLD - SCR_H);
}

// ---------------------------------------------------------------- combat
// who can this unit hit right now? returns an index into E, or -1
static int pick(const Ent *u, float range) {
	int best = -1;
	float bs = 1e9f;
	for (int i = 0; i < nE; i++) {
		const Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0) continue;
		if (e->kind == K_NEXUS && shielded(e->team)) continue;
		if (e->kind == K_CREEP && !(u->kind == K_HERO && u->team == 0)) continue;  // only you hunt the jungle
		if (u->kind == K_CREEP && e->kind != K_HERO) continue;
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
	bool guardian = i == FIRST_CREEP + CREEPS - 1;
	float xp = t->kind == K_NEXUS ? 0 : t->kind == K_TOWER ? 90 : t->kind == K_HERO ? 110 : t->kind == K_CREEP ? (guardian ? 220 : 55) : 18;
	if (t->kind == K_CREEP) {
		t->rt = guardian ? 90.0f : 35.0f;        // monsters come back later
		Ent *me = &E[HERO0];
		if (me->hp > 0) me->hp = fminf(me->max, me->hp + me->max * (guardian ? 0.6f : 0.15f));
	}
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
	if (t->hp <= 0 || (t->kind == K_NEXUS && shielded(t->team))) return;
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
	u->x = clampf(u->x + cosf(u->face) * HEROES[u->type].dash, 30, WORLD - 30);
	u->y = clampf(u->y + sinf(u->face) * HEROES[u->type].dash, 30, WORLD - 30);
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || e->kind >= K_TOWER) continue;
		if (e->kind == K_CREEP && u->team == 1) continue;
		if (segDist(x1, y1, u->x, u->y, e->x, e->y) < e->r + 10) hit(i, 70 + u->lvl * 14);
	}
	addFx(0, x1, y1, u->x, u->y, 0.25f, 4, u->team, 0);
}

static void burst(Ent *u) {                      // a shockwave around the hero
	if (u->s2 > 0 || u->hp <= 0) return;
	u->s2 = 9;
	const float R = HEROES[u->type].burst;
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || (e->kind == K_CREEP && u->team == 1)) continue;
		if (dist(u, e) < R + e->r) hit(i, e->kind >= K_TOWER ? 60 : 110 + u->lvl * 20);
	}
	addFx(1, u->x, u->y, 0, 0, 0.5f, R, u->team, 0);
}

static void ultimate(Ent *u) {                   // a huge blast, unlocked at ULT_LEVEL
	if (u->lvl < ULT_LEVEL || u->s3 > 0 || u->hp <= 0) return;
	u->s3 = ULT_CD;
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || (e->kind == K_CREEP && u->team == 1)) continue;
		if (dist(u, e) < HEROES[u->type].ult + e->r) hit(i, e->kind >= K_TOWER ? 120 : 220 + u->lvl * 30);
	}
	addFx(1, u->x, u->y, 0, 0, 0.7f, HEROES[u->type].ult, u->team, 0);
	addFx(1, u->x, u->y, 0, 0, 0.45f, HEROES[u->type].ult * 0.6f, u->team, 0);
}

// ---------------------------------------------------------------- AI
static void toward(const Ent *u, float x, float y, float *mx, float *my) {
	float dx = x - u->x, dy = y - u->y, l = hypotf(dx, dy);
	if (l < 4) { *mx = *my = 0; return; }
	*mx = dx / l;
	*my = dy / l;
}

// point number "step" of a lane, counted from the team's own base
static const float *lanePoint(int team, int lane, int step) { return PATH[lane][team ? PATH_PTS - 1 - step : step]; }

static void aiMinion(Ent *u, float *mx, float *my) {
	int tg = pick(u, u->rng);
	if (tg >= 0) {                               // in range: stand and fight
		u->face = atan2f(E[tg].y - u->y, E[tg].x - u->x);
		*mx = *my = 0;
		return;
	}
	int n = pick(u, 68);
	if (n >= 0) { toward(u, E[n].x, E[n].y, mx, my); return; }
	const float *p = lanePoint(u->team, u->lane, u->wp);     // otherwise march along the lane
	if (u->wp < PATH_PTS - 1 && hypotf(p[0] - u->x, p[1] - u->y) < 34) p = lanePoint(u->team, u->lane, ++u->wp);
	toward(u, p[0], p[1], mx, my);
}

static void aiHero(Ent *u, float *mx, float *my) {
	*mx = *my = 0;
	if (u->hp < u->max * 0.3f) u->low = true;
	if (u->hp >= u->max * 0.9f) u->low = false;
	if (u->low) { toward(u, homeX(u->team), homeY(u->team), mx, my); return; }
	int tg = pick(u, 144);
	if (tg >= 0) {
		Ent *t = &E[tg];
		bool escorted = false;                   // never push a tower without your minions
		for (int i = FIRST_MINION; i < nE; i++)
			if (E[i].team == u->team && E[i].hp > 0 && dist(&E[i], t) < 92) escorted = true;
		if (t->kind == K_TOWER && !escorted) {
			if (dist(u, t) < t->rng + 28) toward(u, homeX(u->team), homeY(u->team), mx, my);
			return;
		}
		float d = dist(u, t) - t->r;
		u->face = atan2f(t->y - u->y, t->x - u->x);
		if (d < 60 && t->kind == K_HERO && u->s1 <= 0 && rand() % 33 == 0) dash(u);
		if (d < 50 && u->s2 <= 0) burst(u);
		if (d < 70 && t->kind == K_HERO && t->hp > 0 && rand() % 45 == 0) ultimate(u);
		if (d > u->rng * 0.85f) toward(u, t->x, t->y, mx, my);
		return;
	}
	int lane = (1 + (int)(T / 50)) % LANES;      // changes lane every 50 seconds
	int front = -1;                              // follow its furthest minion in that lane
	float best = -1e9f;
	for (int i = FIRST_MINION; i < nE; i++) {
		const Ent *m = &E[i];
		if (m->team != u->team || m->lane != lane || m->hp <= 0) continue;
		const float *p = lanePoint(m->team, lane, m->wp);
		float progress = m->wp * 10000.0f - hypotf(p[0] - m->x, p[1] - m->y);
		if (progress > best) { best = progress; front = i; }
	}
	// walk along the lane so it does not cut through the jungle
	if (u->lane != lane) { u->lane = lane; u->wp = 1; }
	const float *p = lanePoint(u->team, lane, u->wp);
	if (front >= 0 && E[front].wp <= u->wp) toward(u, E[front].x, E[front].y, mx, my);
	else {
		if (front >= 0 && u->wp < PATH_PTS - 1 && hypotf(p[0] - u->x, p[1] - u->y) < 40) u->wp++;
		if (front < 0) toward(u, TOWER_POS[u->team][lane][0], TOWER_POS[u->team][lane][1], mx, my);
		else toward(u, p[0], p[1], mx, my);
	}
}

// jungle monsters only fight back, and give up if you lead them too far away
static void aiCreep(Ent *u, float *mx, float *my) {
	Ent *me = &E[HERO0];
	*mx = *my = 0;
	bool angry = u->hp < u->max && me->hp > 0 && hypotf(me->x - u->hx, me->y - u->hy) < 120;
	if (angry) {
		u->face = atan2f(me->y - u->y, me->x - u->x);
		if (dist(u, me) - me->r > u->rng * 0.8f) toward(u, me->x, me->y, mx, my);
	} else {
		toward(u, u->hx, u->hy, mx, my);
		u->hp = fminf(u->max, u->hp + u->max * 0.25f * DT);
	}
}

// ---------------------------------------------------------------- game loop
static void endGame(bool win) {
	if (state != ST_PLAY) return;               // the first nexus to fall decides the match
	won = win;
	played++;
	if (win) victories++;
	state = ST_OVER;
}

static void addMinion(int team, int lane) {
	if (nE >= MAX_ENT) return;
	Ent *m = &E[nE++];
	setEnt(m, K_MINION, team);
	m->lane = lane;
	m->wp = 1;
	const float *a = lanePoint(team, lane, 0);
	m->x = a[0] + (rand() % 21) - 10;
	m->y = a[1] + (rand() % 21) - 10;
}

static void update(u32 kDown, u32 kHeld, touchPosition tp) {
	Ent *me = &E[HERO0];
	T += DT;
	if (T >= nextWave) {
		nextWave += WAVE_EVERY;
		for (int i = 0; i < 2; i++)
			for (int l = 0; l < LANES && nQ < MAX_SPAWN; l++) spawnQ[nQ++] = (Spawn){ T + i * 0.8f, l };
	}
	for (int i = nQ - 1; i >= 0; i--)
		if (spawnQ[i].at <= T) {
			if (nE + 2 <= MAX_ENT) { addMinion(0, spawnQ[i].lane); addMinion(1, spawnQ[i].lane); }   // both teams or neither
			spawnQ[i] = spawnQ[--nQ];
		}

	// your hero: skills first, then movement
	if (kDown & KEY_A) dash(me);
	if (kDown & KEY_B) burst(me);
	if (kDown & KEY_X) ultimate(me);
	if (kDown & KEY_TOUCH) {
		if (tp.py >= 150 && tp.py < 220) {
			if (tp.px >= 6 && tp.px < 80) dash(me);
			else if (tp.px >= 85 && tp.px < 159) burst(me);
			else if (tp.px >= 164 && tp.px < 238) ultimate(me);
		}
	}
	if (kDown & KEY_SELECT) autoAtk = !autoAtk;
	bool atkHeld = (kHeld & (KEY_Y | KEY_R)) ||                // attack button: Y, R or the touch button
	               ((kHeld & KEY_TOUCH) && tp.py >= 150 && tp.py < 220 && tp.px >= 243 && tp.px < 317);
	circlePosition cp;
	hidCircleRead(&cp);

	for (int i = HERO0; i < nE; i++) {
		Ent *u = &E[i];
		u->cd -= DT; u->s1 -= DT; u->s2 -= DT; u->s3 -= DT;   // cooldowns run even while dead
		if (u->hp <= 0) {                        // fallen heroes and monsters come back after a while
			if (u->kind == K_HERO || u->kind == K_CREEP) {
				u->rt -= DT;
				if (u->rt <= 0) {
					u->hp = u->max;
					u->x = u->kind == K_HERO ? homeX(u->team) : u->hx;
					u->y = u->kind == K_HERO ? homeY(u->team) : u->hy;
					u->wp = 1;
				}
			}
			continue;
		}
		float mx = 0, my = 0;
		if (u == me) {
			mx = (abs(cp.dx) > 20 ? cp.dx / 156.0f : 0) + ((kHeld & KEY_DRIGHT) ? 1 : 0) - ((kHeld & KEY_DLEFT) ? 1 : 0);
			my = -(abs(cp.dy) > 20 ? cp.dy / 156.0f : 0) + ((kHeld & KEY_DDOWN) ? 1 : 0) - ((kHeld & KEY_DUP) ? 1 : 0);
			float l = hypotf(mx, my);
			if (l > 1) { mx /= l; my /= l; }
		} else if (u->kind == K_HERO) aiHero(u, &mx, &my);
		else if (u->kind == K_CREEP) aiCreep(u, &mx, &my);
		else aiMinion(u, &mx, &my);
		if (mx * mx + my * my > 0.01f) u->walk += DT * 11;
		else u->walk = 0;
		if (mx != 0 || my != 0) {
			u->x = clampf(u->x + mx * u->spd * DT, 30, WORLD - 30);
			u->y = clampf(u->y + my * u->spd * DT, 30, WORLD - 30);
			u->face = atan2f(my, mx);
		}
		int tg = pick(u, u->rng);                // auto-attack the nearest enemy in range
		if (u->kind == K_CREEP && u->hp >= u->max) tg = -1;   // calm monsters leave you alone
		if (tg >= 0) {
			if (mx == 0 && my == 0) u->face = atan2f(E[tg].y - u->y, E[tg].x - u->x);
			if (u->cd <= 0 && (u != me || autoAtk || atkHeld)) {      // you hit only when you press attack (or auto is on)
				u->cd = u->acd;
				addFx(0, u->x, u->y, E[tg].x, E[tg].y, 0.14f, 1.5f, u->team, 0);
				hit(tg, u->atk);
			}
		}
		if (u->kind == K_HERO && dist(u, &E[NEXUS(u->team)]) < 100)   // heal next to your own nexus
			u->hp = fminf(u->max, u->hp + 90 * DT);
	}

	for (int i = HERO0; i < nE; i++)             // units push each other apart a little
		for (int j = i + 1; j < nE; j++) {
			Ent *a = &E[i], *b = &E[j];
			if (a->hp <= 0 || b->hp <= 0) continue;
			float d = dist(a, b), m = a->r + b->r;
			if (d < m) {
				float dx = b->x - a->x, dy = b->y - a->y;
				if (d <= 0.01f) { dx = 1; dy = 0; d = 1; }       // stacked units: pick a direction
				float p = (m - dist(a, b)) * 0.25f / d;
				float px = dx * p, py = dy * p;
				a->x -= px; a->y -= py;
				b->x += px; b->y += py;
			}
		}

	for (int i = 0; i < BUILDINGS; i++) {        // towers shoot whatever is in range
		Ent *b = &E[i];
		if (b->kind != K_TOWER || b->hp <= 0) continue;
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
	camy += ((me->y - SCR_H / 2) - camy) * 0.12f;
	camx = clampf(camx, 0, WORLD - SCR_W);
	camy = clampf(camy, 0, WORLD - SCR_H);
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

static void tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c);
static void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u32 c);
static void oval(float cx, float cy, float w, float h, u32 c);
static void glow(float x, float y, float r, int red, int green, int blue);
static void sparkle(float x, float y, float r, u32 c);

static void thick(float x1, float y1, float x2, float y2, float w, u32 c) { C2D_DrawLine(x1, y1, c, x2, y2, c, w, 0); }

static void drawTower(const Ent *e, float x, float y) {
	u32 col = TEAM_COL[e->team], stone = RGB(176, 184, 205), shade = RGB(106, 112, 140), dark = RGB(48, 50, 74);
	oval(x, y + 14, 38, 13, RGBA(0, 0, 0, 90));
	if (e->hp <= 0) {                                   // rubble
		tri(x - 16, y + 15, x - 5, y, x + 4, y + 15, shade);
		tri(x - 3, y + 16, x + 9, y + 4, x + 17, y + 15, dark);
		disc(x + 2, y + 13, 5, stone);
		return;
	}
	oval(x, y + 12, 32, 11, dark);
	quad(x - 11, y - 18, x, y - 18, x, y + 14, x - 14, y + 12, stone);
	quad(x, y - 18, x + 11, y - 18, x + 14, y + 12, x, y + 14, shade);
	fillr(x - 3, y - 2, 6, 12, dark);                   // door
	fillr(x - 15, y - 23, 30, 6, stone);
	for (int i = -1; i <= 1; i++) fillr(x + i * 11 - 4, y - 28, 8, 6, stone);
	fillr(x - 13, y - 17, 26, 3, col);
	float bob = sinf(frame * 0.06f + e->team * 3) * 2;
	glow(x, y - 39 + bob, 15, e->team ? 255 : 90, e->team ? 110 : 200, e->team ? 120 : 255);
	quad(x, y - 49 + bob, x + 7, y - 39 + bob, x, y - 29 + bob, x - 7, y - 39 + bob, col);
	tri(x, y - 49 + bob, x, y - 39 + bob, x - 7, y - 39 + bob, RGBA(255, 255, 255, 150));
	bar(x, y - 56, 34, e->hp / e->max, col);
}

static void drawNexus(const Ent *e, float x, float y) {
	u32 col = TEAM_COL[e->team];
	oval(x, y + 16, 52, 18, RGBA(0, 0, 0, 90));
	if (e->hp <= 0) {
		tri(x - 18, y + 18, x - 6, y, x + 3, y + 18, col);
		tri(x, y + 19, x + 12, y + 6, x + 20, y + 18, RGB(80, 80, 110));
		return;
	}
	oval(x, y + 15, 46, 16, RGB(70, 74, 104));
	oval(x, y + 12, 38, 12, RGB(165, 172, 196));
	float bob = sinf(frame * 0.045f + e->team * 2) * 3;
	glow(x, y - 14 + bob, 28, e->team ? 255 : 90, e->team ? 110 : 200, e->team ? 120 : 255);
	quad(x, y - 40 + bob, x + 15, y - 15 + bob, x, y + 8 + bob, x - 15, y - 15 + bob, col);
	tri(x, y - 40 + bob, x, y - 15 + bob, x - 15, y - 15 + bob, RGBA(255, 255, 255, 140));
	tri(x, y + 8 + bob, x, y - 15 + bob, x + 15, y - 15 + bob, RGBA(0, 0, 0, 60));
	for (int i = 0; i < 3; i++) {                        // small crystals circling it
		float a = frame * 0.03f + i * 2.094f;
		sparkle(x + cosf(a) * 25, y - 8 + sinf(a) * 9, 4, RGB(235, 250, 255));
	}
	if (shielded(e->team)) {                             // shield
		oval(x, y - 12, 50, 62, RGBA(200, 235, 255, 35));
	}
	bar(x, y - 49, 40, e->hp / e->max, col);
}

// a hero (knight) or a minion (little soldier). "d" mirrors the drawing so it faces the right way
static void drawFighter(const Ent *e, float x, float y) {
	bool hero = e->kind == K_HERO;
	float k = hero ? 1.0f : 0.66f;
	float d = cosf(e->face) < 0 ? -1.0f : 1.0f;
	u32 col = TEAM_COL[e->team];
	u32 deep = e->team ? RGB(150, 36, 62) : RGB(28, 78, 160);
	u32 steel = RGB(220, 230, 246), dark = RGB(24, 28, 54), gold = RGB(255, 204, 92);
	float feet = y + e->r;
	float step = sinf(e->walk) * 4 * k;
	float bob = e->walk > 0 ? -fabsf(cosf(e->walk)) * 1.5f : sinf(frame * 0.07f + e->x * 0.1f) * 0.6f;

	oval(x, feet + 1, 20 * k, 7 * k, RGBA(0, 0, 0, 100));
	if (hero) {                                          // ring under each hero
		oval(x, feet + 1, 27, 10, e == &E[HERO0] ? RGBA(255, 214, 100, 170) : RGBA(255, 90, 106, 140));
		oval(x, feet + 1, 21, 7, RGBA(14, 30, 46, 210));
	}
	// legs
	thick(x - 3 * k, feet - 9 * k, x - 4 * k + step, feet, 3.6f * k, dark);
	thick(x + 3 * k, feet - 9 * k, x + 4 * k - step, feet, 3.6f * k, dark);
	float by = feet - 9 * k + bob;                       // waist
	if (hero) {                                          // cape
		float flap = sinf(frame * 0.12f + e->team) * 2 - (e->walk > 0 ? 4 : 0);
		tri(x - d * 2, by - 14, x - d * (12 - flap), by + 5, x + d * 1, by + 3, deep);
	}
	// body
	quad(x - 7 * k, by - 13 * k, x + 7 * k, by - 13 * k, x + 5 * k, by + 1, x - 5 * k, by + 1, col);
	tri(x - 5 * k, by - 12 * k, x + 5 * k, by - 12 * k, x, by - 5 * k, hero ? gold : deep);
	fillr(x - 5 * k, by - 2 * k, 10 * k, 2 * k, dark);
	// head
	float hy = by - 18 * k;
	disc(x, hy, 6.2f * k, hero ? steel : RGB(255, 208, 168));
	if (!hero) tri(x - 7 * k, hy - 1, x + 7 * k, hy - 1, x, hy - 10 * k, deep);   // pointed helmet
	fillr(x + (d > 0 ? 0 : -5 * k), hy - 1, 5 * k, 2.4f * k, dark);
	if (hero) {
		tri(x - 2, hy - 5, x + 2, hy - 5, x - d * 9, hy - 12, HEROES[e->type].acc);            // plume
		disc(x - 7, by - 12, 3.4f, gold);
		disc(x + 7, by - 12, 3.4f, gold);
	}
	// sword arm: swings right after each attack
	float since = e->acd - e->cd;
	float a = -1.15f;                                    // angle, 0 = straight forward, negative = up
	if (e->cd > 0 && since < 0.22f) a = -1.9f + since / 0.22f * 2.5f;
	float hx = x + d * 8 * k, hand = by - 6 * k;
	thick(x + d * 4 * k, by - 11 * k, hx, hand, 3 * k, dark);
	int ht = hero ? e->type : 0;                          // which hero: 0 knight, 1 archer, 2 brute, 3 rogue
	float len = hero ? (ht == 1 ? 12.0f : ht == 2 ? 15.0f : ht == 3 ? 11.0f : 17.0f) : 13.0f;
	float tx = hx + d * cosf(a) * len, ty = hand + sinf(a) * len;
	u32 blade = !hero ? steel : ht == 1 ? RGB(150, 255, 170) : ht == 2 ? RGB(255, 190, 110) : ht == 3 ? RGB(215, 170, 255) : RGB(190, 245, 255);
	thick(hx, hand, tx, ty, hero ? (ht == 2 ? 3.6f : 3.0f) : 2.0f, blade);
	if (hero && ht == 2) quad(tx - 4, ty - 4, tx + 4, ty - 4, tx + 4, ty + 4, tx - 4, ty + 4, RGB(205, 210, 226));   // hammer head
	if (hero && ht == 1) {                              // bow
		thick(hx + d * 2, hand - 9, hx + d * 8, hand, 1.6f, blade);
		thick(hx + d * 8, hand, hx + d * 2, hand + 9, 1.6f, blade);
		thick(hx + d * 2, hand - 9, hx + d * 2, hand + 9, 0.8f, steel);
	}
	disc(hx, hand, 2.4f * k, gold);
	if (e->cd > 0 && since < 0.22f) sparkle(tx, ty, hero ? 5 : 3, RGB(255, 255, 255));
	if (!hero) { disc(x - d * 5 * k, by - 5 * k, 4.2f * k, steel); disc(x - d * 5 * k, by - 5 * k, 2.2f * k, col); }  // shield

	if (e == &E[HERO0]) tri(x, hy - 18, x - 4, hy - 25, x + 4, hy - 25, gold);   // this one is you
	bar(x, hy - (hero ? 15 : 10), hero ? 24 : 13, e->hp / e->max, e->team ? RGB(255, 138, 150) : RGB(124, 196, 255));
}

static void drawMonster(const Ent *e, float x, float y) {
	if (e->hp <= 0) return;
	bool big = e->r > 12;
	float k = e->r / 9.0f, b = sinf(frame * 0.08f + e->hx) * 1.2f;
	u32 skin = big ? RGB(150, 88, 214) : RGB(190, 126, 64), deep = big ? RGB(92, 48, 150) : RGB(122, 76, 40);
	oval(x, y + 8 * k, 24 * k, 8 * k, RGBA(0, 0, 0, 100));
	if (big) glow(x, y, 28, 190, 130, 255);
	tri(x - 9 * k, y - 4 * k + b, x - 13 * k, y - 17 * k + b, x - 2 * k, y - 8 * k + b, RGB(240, 226, 196));  // horns
	tri(x + 9 * k, y - 4 * k + b, x + 13 * k, y - 17 * k + b, x + 2 * k, y - 8 * k + b, RGB(240, 226, 196));
	fillr(x - 8 * k, y + 3 * k, 5 * k, 7 * k, deep);
	fillr(x + 3 * k, y + 3 * k, 5 * k, 7 * k, deep);
	oval(x, y + b, 21 * k, 18 * k, deep);
	oval(x, y - 1 * k + b, 18 * k, 14 * k, skin);
	disc(x - 4 * k, y - 3 * k + b, 2.2f * k, RGB(255, 238, 90));
	disc(x + 4 * k, y - 3 * k + b, 2.2f * k, RGB(255, 238, 90));
	tri(x - 5 * k, y + 2 * k + b, x - 2 * k, y + 2 * k + b, x - 3.5f * k, y + 6 * k + b, RGB(255, 255, 255));
	tri(x + 5 * k, y + 2 * k + b, x + 2 * k, y + 2 * k + b, x + 3.5f * k, y + 6 * k + b, RGB(255, 255, 255));
	bar(x, y - 19 * k, 22 * k, e->hp / e->max, RGB(255, 200, 87));
}

static void drawEnt(const Ent *e, float ox, float oy) {
	float x = e->x - ox, y = e->y - oy;
	if (x < -40 || x > SCR_W + 40 || y < -50 || y > SCR_H + 60) return;
	if (e->kind == K_TOWER) drawTower(e, x, y);
	else if (e->kind == K_NEXUS) drawNexus(e, x, y);
	else if (e->kind == K_CREEP) drawMonster(e, x, y);
	else if (e->hp > 0) drawFighter(e, x, y);
}

// ---- scenery: placed once, away from lanes, river, camps and bases ----
#define MAX_DECOR 260
typedef struct { short x, y, type; } Decor;     // 0 tree, 1 rock, 2 bush
static Decor decor[MAX_DECOR];
static int nDecor;

static void makeMap(void) {
	nDecor = 0;
	for (int gy = 0; gy < 21; gy++)
		for (int gx = 0; gx < 21; gx++) {
			int h = (gx * 73 + gy * 151 + gx * gy * 31) % 997;
			float x = 30 + gx * 57 + h % 23 - 11, y = 30 + gy * 57 + (h / 7) % 23 - 11;
			bool clear = true;
			for (int l = 0; l < LANES; l++)
				for (int i = 0; i + 1 < PATH_PTS; i++)
					if (segDist(PATH[l][i][0], PATH[l][i][1], PATH[l][i + 1][0], PATH[l][i + 1][1], x, y) < LANE_W / 2 + 22) clear = false;
			if (segDist(240, 240, 960, 960, x, y) < 55) clear = false;             // river
			for (int c = 0; c < CREEPS; c++) if (hypotf(x - CAMP[c][0], y - CAMP[c][1]) < 55) clear = false;
			if (hypotf(x - 110, y - 1090) < 190 || hypotf(x - 1090, y - 110) < 190) clear = false;
			if (clear && h % 4 != 0 && nDecor < MAX_DECOR) decor[nDecor++] = (Decor){ (short)x, (short)y, (short)(h % 5 < 3 ? 0 : h % 5 == 3 ? 1 : 2) };
		}
}

static void road(float x1, float y1, float x2, float y2, float w, u32 c) {
	C2D_DrawLine(x1, y1, c, x2, y2, c, w, 0);
	disc(x1, y1, w / 2, c);
	disc(x2, y2, w / 2, c);
}

static void drawMap(float ox, float oy) {
	fillr(0, 0, SCR_W, SCR_H, RGB(32, 88, 62));
	for (int i = 0; i < 600; i++) {                      // grass tufts
		float gx = (i * 131) % WORLD - ox, gy = (i * 467) % WORLD - oy;
		if (gx < -5 || gx > SCR_W + 5 || gy < -5 || gy > SCR_H + 5) continue;
		tri(gx - 3, gy + 3, gx, gy - 3, gx + 1, gy + 3, RGB(62, 126, 80));
		tri(gx, gy + 3, gx + 3, gy - 2, gx + 4, gy + 3, RGB(48, 108, 72));
	}
	road(240 - ox, 240 - oy, 960 - ox, 960 - oy, 84, RGB(104, 148, 110));     // river
	road(240 - ox, 240 - oy, 960 - ox, 960 - oy, 68, RGB(36, 112, 150));
	for (int i = 0; i < 24; i++) {                       // ripples
		float p = 250 + i * 30 + sinf(frame * 0.03f + i) * 5, off = (i % 3 - 1) * 17;
		fillr(p + off - ox, p - off - oy, 12, 2, RGBA(190, 240, 255, 110));
	}
	for (int t = 0; t < 2; t++) {                        // the two bases
		float bx = PATH[1][t ? PATH_PTS - 1 : 0][0] - ox, by = PATH[1][t ? PATH_PTS - 1 : 0][1] - oy;
		disc(bx, by, 150, t ? RGB(112, 72, 82) : RGB(66, 88, 128));
		disc(bx, by, 122, t ? RGB(136, 94, 100) : RGB(88, 112, 150));
	}
	for (int pass = 0; pass < 2; pass++)                 // lanes: dark edge first, then the path
		for (int l = 0; l < LANES; l++)
			for (int i = 0; i + 1 < PATH_PTS; i++)
				road(PATH[l][i][0] - ox, PATH[l][i][1] - oy, PATH[l][i + 1][0] - ox, PATH[l][i + 1][1] - oy,
				     pass ? LANE_W : LANE_W + 9, pass ? RGB(142, 126, 96) : RGB(80, 74, 58));
	for (int c = 0; c < CREEPS; c++) {                   // camp clearings
		float x = CAMP[c][0] - ox, y = CAMP[c][1] - oy;
		if (x < -50 || x > SCR_W + 50 || y < -50 || y > SCR_H + 50) continue;
		oval(x, y + 5, c == CREEPS - 1 ? 82 : 62, c == CREEPS - 1 ? 54 : 40, RGB(88, 96, 70));
	}
	for (int i = 0; i < nDecor; i++) {
		float x = decor[i].x - ox, y = decor[i].y - oy;
		if (x < -24 || x > SCR_W + 24 || y < -28 || y > SCR_H + 28) continue;
		if (decor[i].type == 0) {                        // pine tree
			oval(x + 3, y + 15, 26, 9, RGBA(0, 0, 0, 80));
			fillr(x - 2, y + 5, 5, 11, RGB(90, 62, 44));
			tri(x - 14, y + 9, x, y - 12, x + 14, y + 9, RGB(22, 98, 70));
			tri(x - 11, y - 1, x, y - 20, x + 11, y - 1, RGB(36, 126, 82));
			tri(x - 7, y - 9, x, y - 25, x + 1, y - 9, RGB(72, 158, 98));
		} else if (decor[i].type == 1) {                 // rock
			oval(x, y + 6, 30, 12, RGBA(0, 0, 0, 70));
			oval(x, y, 26, 18, RGB(104, 116, 132));
			oval(x - 4, y - 4, 15, 8, RGB(150, 163, 180));
		} else {                                         // flowering bush
			disc(x - 7, y + 2, 8, RGB(40, 112, 70));
			disc(x + 7, y + 2, 8, RGB(40, 112, 70));
			disc(x, y - 4, 9, RGB(58, 140, 84));
			disc(x - 4, y - 4, 2, RGB(255, 160, 205));
			disc(x + 5, y + 1, 2, RGB(255, 160, 205));
		}
	}
}

static void drawTop(void) {
	float ox = camx, oy = camy;
	drawMap(ox, oy);

	int order[MAX_ENT];                                  // draw from back to front
	for (int i = 0; i < nE; i++) {
		int j = i;
		while (j > 0 && E[order[j - 1]].y > E[i].y) { order[j] = order[j - 1]; j--; }
		order[j] = i;
	}
	for (int i = 0; i < nE; i++) drawEnt(&E[order[i]], ox, oy);

	char buf[48];
	for (int i = 0; i < nFx; i++) {
		const Fx *f = &fxs[i];
		u32 c = RGBA(255, 255, 255, (int)(255 * clampf(f->t * 3, 0, 1)));
		if (f->kind == 0) C2D_DrawLine(f->x1 - ox, f->y1 - oy, c, f->x2 - ox, f->y2 - oy, c, f->r, 0);
		else if (f->kind == 1) {
			float rr = f->r * (1.2f - f->t);
			u32 rc = (TEAM_COL[f->team] & 0x00FFFFFF) | ((u32)(110 * clampf(f->t * 3, 0, 1)) << 24);
			disc(f->x1 - ox, f->y1 - oy, rr, rc);        // a soft expanding wave
		} else {
			snprintf(buf, sizeof(buf), "%d", f->val);
			text(f->x1 - ox - 6, f->y1 - oy - (0.7f - f->t) * 20, 0.4f, RGBA(255, 200, 87, (int)(255 * clampf(f->t * 3, 0, 1))), buf);
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

	if (state == ST_OVER) {
		fillr(0, 0, SCR_W, SCR_H, RGBA(11, 30, 43, 190));
		textC(SCR_W / 2, 66, 1.1f, won ? RGB(124, 220, 160) : RGB(255, 138, 150), won ? "Victory" : "Defeat");
		snprintf(buf, sizeof(buf), "Time %d:%02d   kills %d   level %d", (int)T / 60, (int)T % 60, kills, me->lvl);
		textC(SCR_W / 2, 122, 0.5f, RGB(214, 232, 245), buf);
		textC(SCR_W / 2, 164, 0.6f, RGB(255, 200, 87), "Press A to return to the lobby");
	}
}

// ---------------------------------------------------------------- lobby
static void tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c) {
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0);
}

static void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u32 c) {
	tri(x1, y1, x2, y2, x3, y3, c);
	tri(x1, y1, x3, y3, x4, y4, c);
}

static void grad(float x, float y, float w, float h, u32 top, u32 bottom) {
	C2D_DrawRectangle(x, y, 0, w, h, top, top, bottom, bottom);
}
static void oval(float cx, float cy, float w, float h, u32 c) { C2D_DrawEllipseSolid(cx - w / 2, cy - h / 2, 0, w, h, c); }
static void glow(float x, float y, float r, int red, int green, int blue) {   // soft light made of faint layers
	for (int i = 5; i >= 1; i--) disc(x, y, r * i / 5, RGBA(red, green, blue, 16));
}
static void sparkle(float x, float y, float r, u32 c) {
	quad(x, y - r, x + r * 0.28f, y, x, y + r, x - r * 0.28f, y, c);
	quad(x - r, y, x, y - r * 0.28f, x + r, y, x, y + r * 0.28f, c);
}
static void shadowText(float cx, float y, float size, u32 c, const char *str) {
	textC(cx + 1.5f, y + 1.5f, size, RGBA(0, 0, 0, 190), str);
	textC(cx, y, size, c, str);
}
static void frameBox(float x, float y, float w, float h) {       // dark glass with a gold trim
	grad(x, y, w, h, RGBA(30, 48, 96, 225), RGBA(8, 12, 34, 225));
	fillr(x, y, w, 1, RGB(255, 214, 120));
	fillr(x, y + h - 1, w, 1, RGB(160, 112, 40));
	sparkle(x, y, 4, RGB(255, 232, 170));
	sparkle(x + w, y + h, 4, RGB(255, 232, 170));
}

// ---- knight animation: idle, raise the sword, slash, hold, return ----
#define ANIM_LOOP   300      // frames in one loop (60 per second)
#define ANIM_RAISE  150
#define ANIM_SLASH  192
#define ANIM_HOLD   204
#define ANIM_RETURN 240
#define ANIM_END    272

// smooth movement from a to b while t goes from "from" to "to"
static float ease(int t, int from, int to, float a, float b) {
	float p = clampf((t - from) / (float)(to - from), 0, 1);
	return a + (b - a) * p * p * (3 - 2 * p);
}

// the sword arm: "arm" turns at the shoulder, "wrist" turns the sword in the hand
static void knightPose(int t, float *arm, float *wrist, float *lift) {
	*arm = 0; *wrist = 0; *lift = 0;
	if (t >= ANIM_RAISE && t < ANIM_SLASH) {
		*arm = ease(t, ANIM_RAISE, ANIM_RAISE + 28, 0, -1.35f);
		*wrist = ease(t, ANIM_RAISE, ANIM_RAISE + 28, 0, 1.0f);
		*lift = ease(t, ANIM_RAISE, ANIM_SLASH, 0, -5);
	} else if (t >= ANIM_SLASH && t < ANIM_HOLD) {
		*arm = ease(t, ANIM_SLASH, ANIM_HOLD, -1.35f, -0.45f);
		*wrist = ease(t, ANIM_SLASH, ANIM_HOLD, 1.0f, 2.35f);
		*lift = ease(t, ANIM_SLASH, ANIM_SLASH + 6, -5, 4);
	} else if (t >= ANIM_HOLD && t < ANIM_END) {
		*arm = ease(t, ANIM_RETURN, ANIM_END, -0.45f, 0);
		*wrist = ease(t, ANIM_RETURN, ANIM_END, 2.35f, 0);
		*lift = ease(t, ANIM_RETURN, ANIM_END, 4, 0);
	}
}

static void turnAround(float px, float py, float angle) {
	C2D_ViewTranslate(px, py);
	C2D_ViewRotate(angle);
	C2D_ViewTranslate(-px, -py);
}

static void drawSwordArm(float x, float y, float arm, float wrist) {
	u32 steel = RGB(214, 226, 244), dark = RGB(24, 34, 66), gold = RGB(255, 204, 92);
	C3D_Mtx saved;
	C2D_ViewSave(&saved);
	turnAround(x + 27, y - 40, arm);
	quad(x + 25, y - 40, x + 33, y - 36, x + 43, y + 12, x + 34, y + 15, dark);
	turnAround(x + 40, y + 17, wrist);
	for (int i = 0; i < 5; i++) glow(x + 45, y - 54 + i * 15, 13, 90, 220, 255);
	quad(x + 41, y - 56, x + 49, y - 56, x + 49, y + 16, x + 41, y + 16, RGB(235, 252, 255));
	tri(x + 41, y - 56, x + 49, y - 56, x + 45, y - 74, RGB(235, 252, 255));
	fillr(x + 44, y - 62, 2, 76, RGB(90, 205, 255));
	if (knightClock >= ANIM_RAISE && knightClock < ANIM_SLASH) {          // the blade charges up
		float c = (knightClock - ANIM_RAISE) / (float)(ANIM_SLASH - ANIM_RAISE);
		glow(x + 45, y - 66, 8 + 20 * c, 170, 250, 255);
		sparkle(x + 45, y - 70, 4 + 10 * c, RGB(255, 255, 255));
	}
	quad(x + 31, y + 12, x + 45, y + 18, x + 59, y + 12, x + 45, y + 25, gold);
	fillr(x + 42, y + 22, 6, 13, dark);
	disc(x + 45, y + 37, 4, gold);
	disc(x + 40, y + 17, 7, steel);
	C2D_ViewRestore(&saved);
}

// crescent of light and a shockwave on the stage
static void drawSlashEffect(float x, float y) {
	int t = knightClock - ANIM_SLASH;
	if (t < 0 || t >= 36) return;
	float fade = 1 - t / 36.0f;
	float sweep = clampf(t / 10.0f, 0, 1);
	for (int i = 0; i < 18; i++) {
		float p = i / 18.0f, q = (i + 1) / 18.0f;
		if (p > sweep) break;
		float a = -1.9f + p * 2.7f, b = -1.9f + q * 2.7f;
		float wa = 22 * sinf(p * 3.14159f), wb = 22 * sinf(q * 3.14159f);
		float cx = x + 24, cy = y - 24;
		quad(cx + cosf(a) * 88, cy + sinf(a) * 80, cx + cosf(b) * 88, cy + sinf(b) * 80,
		     cx + cosf(b) * (88 - wb), cy + sinf(b) * (80 - wb), cx + cosf(a) * (88 - wa), cy + sinf(a) * (80 - wa),
		     RGBA(165, 245, 255, (int)(230 * fade)));
	}
	for (int i = 0; i < 12; i++) {                                         // sparks flying out
		float a = i * 0.5236f, d = 12 + t * (2.0f + (i % 3) * 0.7f);
		sparkle(x + 66 + cosf(a) * d, y + sinf(a) * d * 0.7f, 6 * fade, RGBA(255, 240, 180, (int)(255 * fade)));
	}
}

// our own champion, a blue knight, drawn from simple shapes
static void drawKnight(float x, float y) {
	float sway = sinf(frame * 0.045f) * 5;
	float arm, wrist, lift;
	knightPose(knightClock, &arm, &wrist, &lift);
	float feet = y;                                                    // boots stay on the stage
	float breath = sinf(frame * 0.06f) * 2;                            // breathing
	y += breath + lift;
	float nod = sinf(frame * 0.06f - 0.9f) * 1.2f;                     // the head follows a bit later
	u32 steel = RGB(214, 226, 244), shade = RGB(128, 148, 188), dark = RGB(24, 34, 66);
	u32 blue = RGB(58, 160, 255), gold = RGB(255, 204, 92), goldDark = RGB(178, 122, 40), ice = RGB(150, 245, 255);
	// cape
	quad(x - 20, y - 48, x + 20, y - 48, x + 48 + sway, y + 54, x - 48 + sway, y + 54, RGB(28, 42, 112));
	tri(x - 20, y - 46, x - 48 + sway, y + 54, x - 24 + sway, y + 48, RGB(50, 82, 178));
	tri(x + 20, y - 46, x + 48 + sway, y + 54, x + 24 + sway, y + 48, RGB(50, 82, 178));
	C2D_DrawLine(x - 48 + sway, y + 54, gold, x - 20, y - 46, gold, 1.5f, 0);
	C2D_DrawLine(x + 48 + sway, y + 54, gold, x + 20, y - 46, gold, 1.5f, 0);
	// legs and boots
	quad(x - 18, y + 18, x - 3, y + 18, x - 8, feet + 64, x - 22, feet + 64, dark);
	quad(x + 3, y + 18, x + 18, y + 18, x + 22, feet + 64, x + 8, feet + 64, dark);
	quad(x - 20, feet + 38, x - 6, feet + 38, x - 9, feet + 66, x - 23, feet + 66, steel);
	quad(x + 6, feet + 38, x + 20, feet + 38, x + 23, feet + 66, x + 9, feet + 66, shade);
	tri(x - 20, feet + 42, x - 6, feet + 42, x - 13, feet + 33, gold);
	tri(x + 6, feet + 42, x + 20, feet + 42, x + 13, feet + 33, gold);
	oval(x - 18, feet + 67, 22, 9, steel);
	oval(x + 18, feet + 67, 22, 9, shade);
	// armour, lit from the left
	quad(x - 25, y - 46, x + 25, y - 46, x + 15, y + 14, x - 15, y + 14, dark);
	quad(x - 23, y - 45, x, y - 45, x, y + 12, x - 14, y + 12, steel);
	quad(x, y - 45, x + 23, y - 45, x + 14, y + 12, x, y + 12, shade);
	tri(x - 17, y - 40, x + 17, y - 40, x, y - 5, gold);
	tri(x - 13, y - 38, x + 13, y - 38, x, y - 11, blue);
	glow(x, y - 28, 13, 150, 245, 255);
	quad(x, y - 36, x + 6, y - 29, x, y - 21, x - 6, y - 29, ice);
	tri(x - 2, y - 33, x + 1, y - 35, x - 3, y - 28, RGB(255, 255, 255));
	// skirt plates and belt
	quad(x - 16, y + 18, x - 2, y + 18, x - 5, y + 34, x - 20, y + 30, blue);
	quad(x + 2, y + 18, x + 16, y + 18, x + 20, y + 30, x + 5, y + 34, RGB(36, 112, 204));
	fillr(x - 17, y + 12, 34, 7, goldDark);
	fillr(x - 17, y + 12, 34, 3, gold);
	disc(x, y + 15, 5, gold);
	disc(x, y + 15, 2.5f, ice);
	// arms
	quad(x - 25, y - 40, x - 33, y - 36, x - 41, y + 2, x - 32, y + 5, dark);
	quad(x - 35, y - 16, x - 28, y - 14, x - 33, y + 2, x - 42, y, steel);
	// shoulders
	for (int side = -1; side <= 1; side += 2) {
		float px = x + side * 28;
		oval(px, y - 40, 30, 25, gold);
		oval(px, y - 42, 27, 21, side < 0 ? blue : RGB(36, 112, 204));
		oval(px - 4, y - 47, 11, 5, RGBA(255, 255, 255, 120));
		tri(px + side * 6, y - 48, px + side * 22, y - 62, px + side * 14, y - 40, gold);
	}
	disc(x - 37, y + 5, 7, steel);
	// helmet
	y += nod;
	disc(x, y - 65, 18, shade);
	disc(x - 2, y - 66, 16, steel);
	oval(x - 7, y - 75, 9, 5, RGBA(255, 255, 255, 190));
	quad(x - 14, y - 69, x + 14, y - 69, x + 11, y - 61, x - 11, y - 61, dark);
	fillr(x - 2, y - 63, 4, 14, dark);
	if (frame % 170 >= 8) {                                            // eyes blink now and then
		glow(x - 6, y - 65, 6, 150, 245, 255);
		glow(x + 6, y - 65, 6, 150, 245, 255);
		tri(x - 10, y - 66, x - 3, y - 65, x - 9, y - 63, ice);
		tri(x + 10, y - 66, x + 3, y - 65, x + 9, y - 63, ice);
	}
	tri(x - 4, y - 80, x + 4, y - 80, x, y - 72, gold);
	tri(x - 3, y - 81, x + 6, y - 80, x + 26 + sway, y - 98, blue);          // plume
	tri(x, y - 82, x + 5, y - 79, x + 30 + sway, y - 90, ice);
	y -= nod;
	drawSwordArm(x, y, arm, wrist);
	drawSlashEffect(x, y);
}

// ---- hero portraits for the selection screen (the knight is drawn by drawKnight) ----
static void drawRangerArt(float x, float y) {
	float sway = sinf(frame * 0.05f) * 4;
	float feet = y;
	y += sinf(frame * 0.06f) * 2;
	u32 green = RGB(70, 190, 110), deep = RGB(26, 92, 70), mid = RGB(40, 140, 92), skin = RGB(255, 218, 186), hair = RGB(255, 226, 130);
	u32 dark = RGB(24, 34, 66), gold = RGB(255, 204, 92), leather = RGB(120, 84, 56), leaf = RGB(170, 255, 190);
	quad(x - 20, y - 46, x + 20, y - 46, x + 46 + sway, y + 56, x - 46 + sway, y + 56, deep);            // cloak
	tri(x - 20, y - 44, x - 46 + sway, y + 56, x - 22 + sway, y + 50, mid);
	quad(x - 16, y + 18, x - 3, y + 18, x - 7, feet + 64, x - 20, feet + 64, dark);                       // legs and boots
	quad(x + 3, y + 18, x + 16, y + 18, x + 20, feet + 64, x + 7, feet + 64, dark);
	quad(x - 19, feet + 40, x - 6, feet + 40, x - 8, feet + 66, x - 21, feet + 66, leather);
	quad(x + 6, feet + 40, x + 19, feet + 40, x + 21, feet + 66, x + 8, feet + 66, RGB(98, 68, 46));
	oval(x - 16, feet + 67, 22, 8, leather);
	oval(x + 16, feet + 67, 22, 8, RGB(98, 68, 46));
	quad(x - 22, y - 44, x + 22, y - 44, x + 14, y + 20, x - 14, y + 20, dark);                          // tunic
	quad(x - 20, y - 43, x + 20, y - 43, x + 12, y + 18, x - 12, y + 18, green);
	tri(x - 20, y - 43, x + 20, y - 43, x, y - 18, mid);
	fillr(x - 14, y + 11, 28, 6, leather);
	disc(x, y + 14, 4, gold);
	thick(x - 17, y - 41, x + 13, y + 10, 4, leather);                                                    // quiver strap
	for (int i = 0; i < 3; i++) {                                                                         // arrows over the shoulder
		thick(x + 14 + i * 4, y - 48, x + 20 + i * 4, y - 74, 1.5f, leather);
		tri(x + 17 + i * 4, y - 74, x + 23 + i * 4, y - 74, x + 20 + i * 4, y - 82, leaf);
	}
	thick(x + 22, y - 40, x + 30, y - 6, 8, green);                                                       // free arm
	disc(x + 31, y - 4, 6, skin);
	thick(x - 22, y - 40, x - 40, y - 8, 8, green);                                                       // bow arm
	disc(x - 41, y - 6, 6, skin);
	float bx = x - 15, by = y - 6;                                                                        // the bow
	for (int i = 0; i < 11; i++) {
		float a = -1.3f + i * 0.2364f, b = a + 0.2364f;
		thick(bx - 26 * cosf(a), by + 56 * sinf(a), bx - 26 * cosf(b), by + 56 * sinf(b), 3.5f, gold);
	}
	thick(bx - 26 * cosf(1.3f), by + 56 * sinf(1.3f), bx - 26 * cosf(-1.3f), by + 56 * sinf(-1.3f), 1.0f, RGB(235, 245, 255));
	disc(x, y - 64, 21, green);                                                                           // hood and face
	disc(x, y - 60, 15, skin);
	oval(x, y - 72, 30, 13, hair);
	tri(x - 20, y - 74, x + 20, y - 74, x + 6 + sway * 0.5f, y - 108, green);
	glow(x - 5, y - 59, 6, 170, 255, 190);
	glow(x + 5, y - 59, 6, 170, 255, 190);
	disc(x - 5, y - 59, 2.4f, dark);
	disc(x + 5, y - 59, 2.4f, dark);
	fillr(x - 3, y - 52, 6, 1.5f, RGB(214, 120, 120));
}

static void drawBruteArt(float x, float y) {
	float feet = y;
	y += sinf(frame * 0.05f) * 2;
	u32 orange = RGB(255, 150, 60), shade = RGB(214, 110, 40), deep = RGB(130, 60, 24), steel = RGB(176, 184, 205), steel2 = RGB(130, 138, 160);
	u32 dark = RGB(36, 30, 44), skin = RGB(236, 172, 128), gold = RGB(255, 204, 92), wood = RGB(120, 84, 56);
	thick(x + 54, y + 54, x + 54, y - 58, 7, wood);                                                       // war hammer
	quad(x + 32, y - 88, x + 78, y - 88, x + 78, y - 58, x + 32, y - 58, steel2);
	quad(x + 32, y - 88, x + 78, y - 88, x + 72, y - 80, x + 38, y - 80, RGB(230, 236, 248));
	quad(x - 24, y + 16, x - 2, y + 16, x - 6, feet + 64, x - 28, feet + 64, dark);                       // legs and boots
	quad(x + 2, y + 16, x + 24, y + 16, x + 28, feet + 64, x + 6, feet + 64, dark);
	oval(x - 20, feet + 67, 32, 10, steel);
	oval(x + 20, feet + 67, 32, 10, steel2);
	quad(x - 36, y - 42, x + 36, y - 42, x + 25, y + 20, x - 25, y + 20, dark);                          // wide armour
	quad(x - 34, y - 40, x, y - 40, x, y + 18, x - 23, y + 18, orange);
	quad(x, y - 40, x + 34, y - 40, x + 23, y + 18, x, y + 18, shade);
	fillr(x - 25, y + 10, 50, 8, deep);
	fillr(x - 25, y + 10, 50, 3, gold);
	disc(x, y + 14, 6, gold);
	tri(x - 15, y - 34, x + 15, y - 34, x, y - 6, gold);
	tri(x - 10, y - 32, x + 10, y - 32, x, y - 13, deep);
	thick(x + 42, y - 30, x + 54, y - 8, 10, dark);                                                       // arms
	disc(x + 54, y - 6, 8, skin);
	thick(x - 42, y - 30, x - 48, y + 6, 10, dark);
	disc(x - 48, y + 8, 8, skin);
	for (int s = -1; s <= 1; s += 2) {                                                                    // spiked shoulders
		float px = x + s * 40;
		oval(px, y - 42, 36, 30, steel);
		oval(px, y - 44, 31, 23, s < 0 ? orange : shade);
		tri(px + s * 4, y - 54, px + s * 18, y - 80, px + s * 15, y - 48, steel);
	}
	disc(x, y - 62, 17, skin);                                                                            // head
	tri(x - 13, y - 56, x + 13, y - 56, x, y - 28, RGB(200, 90, 40));
	oval(x, y - 70, 38, 24, steel);
	tri(x - 17, y - 72, x - 28, y - 102, x - 9, y - 78, RGB(245, 235, 205));
	tri(x + 17, y - 72, x + 28, y - 102, x + 9, y - 78, RGB(245, 235, 205));
	fillr(x - 14, y - 66, 28, 6, dark);
	glow(x - 6, y - 63, 6, 255, 140, 70);
	glow(x + 6, y - 63, 6, 255, 140, 70);
	disc(x - 6, y - 63, 2, RGB(255, 190, 90));
	disc(x + 6, y - 63, 2, RGB(255, 190, 90));
}

static void drawRogueArt(float x, float y) {
	float sway = sinf(frame * 0.07f) * 6;
	float feet = y;
	y += sinf(frame * 0.06f) * 2;
	u32 violet = RGB(168, 110, 255), mid = RGB(110, 66, 210), deep = RGB(50, 28, 100), dark = RGB(20, 16, 44), skin = RGB(240, 208, 190);
	u32 pink = RGB(255, 110, 200), steel = RGB(220, 230, 246), gold = RGB(255, 204, 92);
	quad(x + 8, y - 44, x + 18, y - 50, x + 60 + sway, y - 20, x + 48 + sway, y - 12, pink);              // scarf tail
	tri(x + 48 + sway, y - 12, x + 60 + sway, y - 20, x + 62 + sway, y - 2, RGB(210, 60, 150));
	quad(x - 13, y + 16, x - 2, y + 16, x - 6, feet + 64, x - 17, feet + 64, dark);                       // legs and boots
	quad(x + 2, y + 16, x + 13, y + 16, x + 17, feet + 64, x + 6, feet + 64, dark);
	quad(x - 16, feet + 40, x - 4, feet + 40, x - 6, feet + 47, x - 17, feet + 47, violet);
	quad(x + 4, feet + 40, x + 16, feet + 40, x + 17, feet + 47, x + 6, feet + 47, violet);
	oval(x - 12, feet + 66, 20, 7, deep);
	oval(x + 12, feet + 66, 20, 7, deep);
	quad(x - 18, y - 44, x + 18, y - 44, x + 11, y + 18, x - 11, y + 18, dark);                           // slim torso
	quad(x - 16, y - 43, x + 16, y - 43, x + 9, y + 16, x - 9, y + 16, mid);
	thick(x - 15, y - 42, x + 9, y + 12, 3, dark);
	thick(x + 15, y - 42, x - 9, y + 12, 3, dark);
	disc(x, y - 14, 3.5f, gold);
	fillr(x - 12, y + 10, 24, 5, deep);
	tri(x - 16, y - 46, x + 16, y - 46, x, y - 30, pink);
	thick(x - 18, y - 38, x - 30, y - 10, 7, mid);                                                        // arms and twin daggers
	thick(x + 18, y - 38, x + 30, y - 10, 7, mid);
	glow(x - 40, y - 34, 10, 190, 150, 255);
	glow(x + 40, y - 34, 10, 190, 150, 255);
	thick(x - 30, y - 8, x - 44, y - 46, 4, steel);
	tri(x - 47, y - 44, x - 41, y - 48, x - 46, y - 58, steel);
	thick(x + 30, y - 8, x + 44, y - 46, 4, steel);
	tri(x + 47, y - 44, x + 41, y - 48, x + 46, y - 58, steel);
	disc(x - 30, y - 8, 5, skin);
	disc(x + 30, y - 8, 5, skin);
	disc(x, y - 64, 19, deep);                                                                            // hood, face and mask
	tri(x - 14, y - 74, x + 14, y - 74, x + 16 + sway * 0.6f, y - 102, deep);
	oval(x, y - 61, 24, 20, skin);
	quad(x - 12, y - 58, x + 12, y - 58, x + 9, y - 46, x - 9, y - 46, dark);
	glow(x - 5, y - 62, 6, 255, 110, 200);
	glow(x + 5, y - 62, 6, 255, 110, 200);
	disc(x - 5, y - 62, 2.4f, pink);
	disc(x + 5, y - 62, 2.4f, pink);
}

static void drawHeroArt(int type, float x, float y) {
	if (type == 0) {
		int saved = knightClock;                 // the card shows him standing, not slashing
		knightClock = 0;
		drawKnight(x, y);
		knightClock = saved;
	} else if (type == 1) drawRangerArt(x, y);
	else if (type == 2) drawBruteArt(x, y);
	else drawRogueArt(x, y);
}

// top screen: four tall hero cards, the chosen one lifts up and glows
static void drawPickTop(void) {
	grad(0, 0, SCR_W, 140, RGB(10, 12, 44), RGB(62, 46, 120));
	grad(0, 140, SCR_W, 100, RGB(62, 46, 120), RGB(14, 14, 40));
	for (int i = 0; i < 30; i++) {
		int a = 110 + (int)(100 * sinf(frame * 0.05f + i * 1.7f));
		sparkle((i * 97) % SCR_W, (i * 53) % 130, i % 7 == 0 ? 3.5f : 1.5f, RGBA(255, 255, 255, a));
	}
	shadowText(SCR_W / 2, 6, 0.8f, RGB(255, 214, 120), "CHOOSE YOUR HERO");
	fillr(70, 30, 70, 1, RGB(255, 214, 120));
	fillr(260, 30, 70, 1, RGB(255, 214, 120));
	const float cw = 86, gap = 8, ch = 160;
	float x0 = (SCR_W - (NHEROES * cw + (NHEROES - 1) * gap)) / 2;
	for (int i = 0; i < NHEROES; i++) {
		const HeroDef *h = &HEROES[i];
		bool on = i == pickSel;
		float x = x0 + i * (cw + gap), y = on ? 36 : 46;
		if (on) {                                                         // pulsing gold outline
			int a = 150 + (int)(90 * sinf(frame * 0.1f));
			fillr(x - 5, y - 5, cw + 10, ch + 10, RGBA(255, 224, 140, a / 2));
			fillr(x - 2, y - 2, cw + 4, ch + 4, RGB(255, 214, 120));
		} else fillr(x - 1, y - 1, cw + 2, ch + 2, RGB(90, 96, 140));
		grad(x, y, cw, ch, h->card, RGB(10, 14, 40));
		glow(x + cw / 2, y + 70, 40, (h->acc & 255), (h->acc >> 8) & 255, (h->acc >> 16) & 255);
		C3D_Mtx saved;
		C2D_ViewSave(&saved);
		C2D_ViewTranslate(x + cw / 2, y + 88);
		float s = on ? 0.62f : 0.54f;
		C2D_ViewScale(s, s);
		drawHeroArt(i, 0, 0);
		C2D_ViewRestore(&saved);
		if (!on) fillr(x, y, cw, ch, RGBA(0, 0, 8, 120));
		fillr(x, y + ch - 26, cw, 26, RGBA(0, 0, 0, 175));
		fillr(x, y + ch - 26, cw, 1, on ? RGB(255, 214, 120) : RGB(120, 126, 170));
		textC(x + cw / 2, y + ch - 25, 0.48f, on ? RGB(255, 255, 255) : RGB(190, 200, 225), h->name);
		textC(x + cw / 2, y + ch - 12, 0.32f, h->acc, h->role);
	}
	tri(10, 120, 20 - 6, 112, 20 - 6, 128, RGB(255, 214, 120));          // arrows hint at left/right
	tri(SCR_W - 10, 120, SCR_W - 14, 112, SCR_W - 14, 128, RGB(255, 214, 120));
	textC(SCR_W / 2, 216, 0.42f, RGB(190, 215, 255), "D-Pad / L R: choose    A: select    B: back");
}

// bottom screen: stats of the chosen hero, a strip of tiles you can tap, and the buttons
static void drawPickBottom(void) {
	const HeroDef *h = &HEROES[pickSel];
	grad(0, 0, BOT_W, SCR_H, RGB(30, 28, 80), RGB(7, 9, 26));
	for (int i = 0; i < 18; i++)
		sparkle((i * 71) % BOT_W, (i * 43) % SCR_H, 2, RGBA(150, 200, 255, 70 + (int)(50 * sinf(frame * 0.04f + i))));
	shadowText(160, 4, 0.8f, RGB(255, 214, 120), h->name);
	textC(160, 31, 0.45f, h->acc, h->role);
	frameBox(10, 50, 300, 80);
	static const char *lab[4] = { "HP", "ATK", "RANGE", "SPEED" };
	float v[4] = { h->hp / 1500.0f, h->atk / 60.0f, h->rng / 85.0f, h->spd / 95.0f };
	for (int r = 0; r < 4; r++) {
		float y = 58 + r * 18;
		text(22, y - 1, 0.42f, RGB(224, 236, 255), lab[r]);
		fillr(82, y + 2, 210, 8, RGBA(0, 0, 0, 170));
		fillr(82, y + 2, 210 * clampf(v[r], 0.05f, 1), 8, h->acc);
		fillr(82, y + 2, 210 * clampf(v[r], 0.05f, 1), 2, RGBA(255, 255, 255, 120));
	}
	textC(160, 134, 0.42f, RGB(214, 228, 250), h->d1);
	textC(160, 148, 0.42f, RGB(214, 228, 250), h->d2);
	for (int i = 0; i < NHEROES; i++) {                                  // tap a tile to look at that hero
		float x = 68 + i * 48, y = 168;
		bool on = i == pickSel;
		if (on) fillr(x - 3, y - 3, 46, 36, RGBA(255, 224, 140, 150 + (int)(90 * sinf(frame * 0.1f))));
		fillr(x - 1, y - 1, 42, 32, on ? RGB(255, 214, 120) : RGB(90, 96, 140));
		grad(x, y, 40, 30, HEROES[i].card, RGB(10, 14, 40));
		char c[2] = { HEROES[i].name[0], 0 };
		shadowText(x + 20, y + 4, 0.7f, RGB(255, 255, 255), c);
	}
	grad(10, 205, 80, 31, RGB(120, 130, 170), RGB(50, 56, 90));
	fillr(10, 205, 80, 2, RGBA(255, 255, 255, 150));
	shadowText(50, 211, 0.5f, RGB(255, 255, 255), "BACK (B)");
	grad(100, 205, 210, 31, RGB(255, 192, 62), RGB(176, 92, 16));
	fillr(100, 205, 210, 2, RGBA(255, 255, 255, 150));
	shadowText(205, 210, 0.6f, RGB(255, 255, 255), "SELECT (A)");
}

static void drawLobbyTop(void) {
	char buf[48];
	grad(0, 0, SCR_W, 110, RGB(10, 12, 44), RGB(62, 46, 120));            // twilight sky
	grad(0, 110, SCR_W, 80, RGB(62, 46, 120), RGB(244, 142, 128));
	for (int i = 0; i < 40; i++) {                                        // twinkling stars
		int a = 110 + (int)(100 * sinf(frame * 0.05f + i * 1.7f));
		sparkle((i * 97) % SCR_W, (i * 53) % 108, i % 7 == 0 ? 3.5f : 1.5f, RGBA(255, 255, 255, a));
	}
	glow(68, 82, 52, 190, 200, 255);                                      // moon
	disc(68, 82, 19, RGB(234, 238, 255));
	disc(62, 77, 5, RGB(208, 214, 242));
	disc(76, 89, 3, RGB(208, 214, 242));
	for (int i = 0; i < 7; i++)                                           // far mountains
		tri(i * 70 - 60, 192, i * 70 + 20, 120 + (i * 29) % 38, i * 70 + 100, 192, RGB(86, 62, 128));
	for (int i = 0; i < 8; i++)                                           // near mountains
		tri(i * 62 - 55, 192, i * 62, 154 + (i * 17) % 24, i * 62 + 55, 192, RGB(48, 38, 90));

	float bob = sinf(frame * 0.03f) * 4;                                  // floating crystal
	grad(317, 80 + bob, 6, 112 - bob, RGBA(120, 235, 255, 210), RGBA(120, 235, 255, 20));
	glow(320, 66 + bob, 42, 110, 230, 255);
	quad(320, 34 + bob, 335, 64 + bob, 320, 98 + bob, 305, 64 + bob, RGB(88, 200, 245));
	tri(320, 34 + bob, 320, 98 + bob, 305, 64 + bob, RGB(180, 248, 255));
	tri(320, 34 + bob, 335, 64 + bob, 320, 64 + bob, RGB(130, 226, 255));
	fillr(296, 152, 9, 40, RGB(30, 26, 66));
	fillr(335, 152, 9, 40, RGB(30, 26, 66));
	tri(294, 152, 307, 152, 300, 132, RGB(30, 26, 66));
	tri(333, 152, 346, 152, 340, 132, RGB(30, 26, 66));

	grad(0, 190, SCR_W, 50, RGB(36, 32, 76), RGB(8, 9, 24));              // ground
	for (int i = -4; i <= 4; i++)                                         // slowly turning light rays
	{
		float rx = 200 + i * 44 + sinf(frame * 0.012f + i) * 14;
		tri(200, 20, rx - 9, 205, rx + 9, 205, RGBA(150, 190, 255, 12));
	}
	oval(200, 205, 190, 40, RGBA(80, 220, 255, 40));                      // stage
	oval(200, 207, 158, 33, RGB(255, 204, 92));
	oval(200, 205, 154, 30, RGB(38, 54, 112));
	oval(200, 204, 120, 22, RGB(112, 232, 255));
	oval(200, 204, 114, 19, RGB(30, 44, 96));
	for (int i = 0; i < 8; i++) {                                         // lights circling the stage
		float a = frame * 0.02f + i * 0.7854f;
		sparkle(200 + cosf(a) * 67, 205 + sinf(a) * 12.5f, 4, RGB(190, 250, 255));
	}
	oval(200, 205, 74, 11, RGBA(0, 0, 0, 110));
	if (knightClock >= ANIM_SLASH && knightClock < ANIM_SLASH + 30) {      // shockwave across the stage
		float t = (knightClock - ANIM_SLASH) / 30.0f;
		oval(200, 205, 80 + 220 * t, 16 + 36 * t, RGBA(150, 240, 255, (int)(100 * (1 - t))));
	}
	drawKnight(200, 134);

	for (int i = 0; i < 22; i++) {                                        // rising embers
		float rise = fmodf(frame * (0.25f + (i % 4) * 0.1f) + i * 41, 200);
		float ex = (i * 83) % SCR_W + sinf(frame * 0.03f + i) * 6;
		sparkle(ex, 230 - rise, 1.5f + i % 3, RGBA(255, 220, 140, (int)(220 * (1 - rise / 200))));
	}

	frameBox(8, 8, 108, 38);                                              // profile
	disc(27, 27, 13, RGB(255, 204, 92));
	disc(27, 27, 11, RGB(36, 112, 204));
	sparkle(27, 27, 8, RGB(255, 255, 255));
	text(46, 11, 0.5f, RGB(255, 255, 255), "Player");
	snprintf(buf, sizeof(buf), "Wins %d / %d", victories, played);
	text(46, 27, 0.38f, RGB(190, 220, 255), buf);
	frameBox(268, 6, 124, 24);
	shadowText(330, 9, 0.6f, RGB(255, 214, 120), "LANE RUSH");
	frameBox(8, 208, 104, 24);
	shadowText(60, 212, 0.48f, RGB(255, 255, 255), "Blue Knight");
}

static void menuButton(float y, float h, const char *label, int id, u32 top, u32 bottom) {
	bool on = menuSel == id;
	if (on) {                                                             // pulsing outline
		int a = 150 + (int)(90 * sinf(frame * 0.1f));
		fillr(36, y - 4, 248, h + 8, RGBA(255, 224, 140, a / 2));
		fillr(38, y - 2, 244, h + 4, RGBA(255, 240, 190, a));
		tri(14, y + h / 2 - 8, 14, y + h / 2 + 8, 28, y + h / 2, RGB(255, 214, 120));
		tri(306, y + h / 2 - 8, 306, y + h / 2 + 8, 292, y + h / 2, RGB(255, 214, 120));
	}
	grad(40, y, 240, h, top, bottom);
	fillr(40, y, 240, 2, RGBA(255, 255, 255, 150));
	fillr(40, y + h - 3, 240, 3, RGBA(0, 0, 0, 90));
	float sx = 40 + fmodf(frame * 2.4f + id * 90, 480);                   // shine sweeping across
	if (sx < 244) quad(sx + 16, y + 2, sx + 36, y + 2, sx + 20, y + h - 3, sx, y + h - 3, RGBA(255, 255, 255, 48));
	shadowText(160, y + h / 2 - 11, 0.75f, RGB(255, 255, 255), label);
}

static void drawLobbyBottom(void) {
	grad(0, 0, BOT_W, SCR_H, RGB(30, 28, 80), RGB(7, 9, 26));
	for (int i = 0; i < 18; i++)
		sparkle((i * 71) % BOT_W, (i * 43) % SCR_H, 2, RGBA(150, 200, 255, 70 + (int)(50 * sinf(frame * 0.04f + i))));
	if (state == ST_HELP) {
		static const char *lines[] = {
			"Circle Pad / D-Pad: move",
			"Y / R (or ATTACK): hit nearby enemies.",
			"A: dash   B: burst   X: ultimate (Lv 4)",
			"SELECT: auto-attack on / off",
			"Stay near your nexus to heal.",
			"Break a red tower, then the nexus.",
		};
		shadowText(160, 12, 0.7f, RGB(255, 214, 120), "How to play");
		frameBox(10, 44, 300, 142);
		for (int i = 0; i < 6; i++) text(22, 51 + i * 22, 0.48f, RGB(224, 236, 255), lines[i]);
		grad(90, 195, 140, 34, RGB(86, 134, 220), RGB(36, 62, 130));
		fillr(90, 195, 140, 2, RGBA(255, 255, 255, 150));
		shadowText(160, 202, 0.6f, RGB(255, 255, 255), "Back (B)");
		return;
	}
	shadowText(160, 15, 0.8f, RGB(255, 214, 120), "LOBBY");
	fillr(60, 27, 52, 1, RGB(255, 214, 120));
	fillr(208, 27, 52, 1, RGB(255, 214, 120));
	sparkle(60, 27, 5, RGB(255, 232, 170));
	sparkle(260, 27, 5, RGB(255, 232, 170));
	menuButton(62, 62, "BATTLE", M_BATTLE, RGB(255, 192, 62), RGB(176, 92, 16));
	menuButton(138, 46, "How to play", M_HELP, RGB(86, 134, 220), RGB(36, 62, 130));
	textC(160, 210, 0.42f, RGB(150, 175, 220), "A: select   Y: knight attack   START: quit");
}

static void startMatch(void) {
	enemyHero = (myHero + 1 + rand() % (NHEROES - 1)) % NHEROES;   // the red team plays a different hero
	resetGame();
	state = ST_PLAY;
}

// lobby, help and result screens
static void menuInput(u32 kDown, touchPosition tp) {
	bool touched = kDown & KEY_TOUCH;
	if (state == ST_OVER) {
		if (kDown & KEY_A) { resetGame(); knightClock = 0; state = ST_LOBBY; }
	} else if (state == ST_PICK) {
		if (kDown & (KEY_DLEFT | KEY_L)) pickSel = (pickSel + NHEROES - 1) % NHEROES;
		if (kDown & (KEY_DRIGHT | KEY_R)) pickSel = (pickSel + 1) % NHEROES;
		bool ok = kDown & KEY_A, back = kDown & KEY_B;
		if (touched) {
			if (tp.py >= 168 && tp.py < 198)
				for (int i = 0; i < NHEROES; i++)
					if (tp.px >= 68 + i * 48 && tp.px < 108 + i * 48) pickSel = i;
			if (tp.py >= 205 && tp.py < 236) {
				if (tp.px >= 10 && tp.px < 90) back = true;
				else if (tp.px >= 100 && tp.px < 310) ok = true;
			}
		}
		if (ok) { myHero = pickSel; startMatch(); }
		else if (back) state = ST_LOBBY;
	} else if (state == ST_HELP) {
		if ((kDown & (KEY_A | KEY_B)) || (touched && tp.px >= 90 && tp.px < 230 && tp.py >= 195 && tp.py < 229))
			state = ST_LOBBY;
	} else {
		if ((kDown & KEY_Y) && (knightClock < ANIM_RAISE || knightClock >= ANIM_END)) knightClock = ANIM_RAISE;
		if (kDown & (KEY_UP | KEY_DOWN)) menuSel = (menuSel + 1) % M_COUNT;
		int choice = (kDown & KEY_A) ? menuSel : -1;
		if (touched && tp.px >= 40 && tp.px < 280) {
			if (tp.py >= 62 && tp.py < 124) choice = M_BATTLE;
			else if (tp.py >= 138 && tp.py < 184) choice = M_HELP;
		}
		if (choice >= 0) menuSel = choice;
		if (choice == M_BATTLE) { pickSel = myHero; state = ST_PICK; }   // go and choose a hero first
		else if (choice == M_HELP) state = ST_HELP;
	}
}

static void drawButton(float x, const char *label, float cd, float maxcd, u32 col) {
	const float w = 74;
	fillr(x, 150, w, 70, RGB(11, 30, 51));
	fillr(x + 3, 153, w - 6, 64, col);
	if (cd > 0) {                                // darkens from the top while recharging
		fillr(x + 3, 153, w - 6, 64 * clampf(cd / maxcd, 0, 1), RGBA(0, 0, 0, 170));
		if (maxcd >= 2) {                        // long cooldowns also show the seconds left
			char num[8];
			snprintf(num, sizeof(num), "%d", (int)ceilf(cd));
			textC(x + w / 2, 175, 0.7f, RGB(255, 255, 255), num);
		}
	}
	if (cd <= 0 || maxcd < 2) textC(x + w / 2, 178, 0.4f, RGB(255, 255, 255), label);
}

static void drawBottom(void) {
	fillr(0, 0, BOT_W, SCR_H, RGB(14, 33, 51));
	const float mx0 = 6, my0 = 5, size = 140, k = size / WORLD;   // square minimap
	fillr(mx0 - 2, my0 - 2, size + 4, size + 4, RGB(255, 204, 92));
	fillr(mx0, my0, size, size, RGB(28, 78, 58));
	C2D_DrawLine(mx0 + 240 * k, my0 + 240 * k, RGB(36, 112, 150), mx0 + 960 * k, my0 + 960 * k, RGB(36, 112, 150), 9, 0);
	for (int l = 0; l < LANES; l++)
		for (int i = 0; i + 1 < PATH_PTS; i++)
			C2D_DrawLine(mx0 + PATH[l][i][0] * k, my0 + PATH[l][i][1] * k, RGB(142, 126, 96),
			             mx0 + PATH[l][i + 1][0] * k, my0 + PATH[l][i + 1][1] * k, RGB(142, 126, 96), 5, 0);
	for (int i = 0; i < nE; i++) {               // every living unit is a dot
		const Ent *e = &E[i];
		if (e->hp <= 0) continue;
		float x = mx0 + e->x * k, y = my0 + e->y * k;
		if (e->kind >= K_TOWER) {
			float s = e->kind == K_NEXUS ? 5 : 3;
			fillr(x - s, y - s, s * 2, s * 2, TEAM_COL[e->team]);
		} else if (e->kind == K_HERO) {
			disc(x, y, 4, i == HERO0 ? RGB(255, 214, 100) : RGB(255, 255, 255));
			disc(x, y, 2.6f, TEAM_COL[e->team]);
		} else disc(x, y, e->kind == K_CREEP ? (e->r > 12 ? 4 : 2.6f) : 1.5f, TEAM_COL[e->team]);
	}
	float vx = mx0 + camx * k, vy = my0 + camy * k, vw = SCR_W * k, vh = SCR_H * k;   // what the top screen shows
	u32 white = RGB(255, 255, 255);
	fillr(vx, vy, vw, 1, white); fillr(vx, vy + vh, vw, 1, white);
	fillr(vx, vy, 1, vh, white); fillr(vx + vw, vy, 1, vh + 1, white);

	for (int t = 0; t < 2; t++) {
		float y = 6 + t * 40;
		text(156, y, 0.45f, TEAM_COL[t], t ? "Red nexus" : "Blue nexus");
		fillr(156, y + 18, 156, 8, RGBA(0, 0, 0, 170));
		fillr(156, y + 18, 156 * clampf(E[NEXUS(t)].hp / E[NEXUS(t)].max, 0, 1), 8, TEAM_COL[t]);
	}
	text(156, 88, 0.42f, RGB(214, 232, 245), shielded(1) ? "Break one red tower" : "Attack the red");
	text(156, 102, 0.42f, RGB(214, 232, 245), shielded(1) ? "to open the nexus." : "nexus now!");
	text(156, 122, 0.4f, RGB(190, 150, 255), "Purple: jungle");
	text(156, 135, 0.4f, E[FIRST_CREEP + CREEPS - 1].hp > 0 ? RGB(190, 150, 255) : RGB(130, 140, 150),
	     E[FIRST_CREEP + CREEPS - 1].hp > 0 ? "Guardian is awake" : "Guardian defeated");

	drawButton(6, "DASH (A)", E[HERO0].s1, 5, RGB(58, 120, 200));
	drawButton(85, "BURST (B)", E[HERO0].s2, 9, RGB(200, 90, 80));
	if (E[HERO0].lvl < ULT_LEVEL) drawButton(164, "LV 4", 0, ULT_CD, RGB(70, 75, 85));
	else drawButton(164, "ULT (X)", E[HERO0].s3, ULT_CD, RGB(140, 80, 190));
	drawButton(243, "ATTACK (Y)", E[HERO0].cd, E[HERO0].acd, RGB(230, 150, 40));      // hold it to keep hitting
	textC(160, 223, 0.38f, RGB(150, 175, 220), autoAtk ? "SELECT: auto-attack ON" : "SELECT: auto-attack OFF");
}

// Anti-aliasing: each screen is drawn at twice the size, then shrunk smoothly onto the LCD.
#define AA_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY))

static C3D_RenderTarget *smoothTarget(gfxScreen_t screen, int width) {
	// 3DS screens are stored sideways, so height comes first
	C3D_RenderTarget *t = C3D_RenderTargetCreate(SCR_H * 2, width * 2, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	if (t) C3D_RenderTargetSetOutput(t, screen, GFX_LEFT, AA_FLAGS);
	return t;
}

static void beginSmooth(C3D_RenderTarget *t) {
	C2D_SceneBegin(t);
	C2D_ViewReset();
	C2D_ViewScale(2.0f, 2.0f);          // the game still uses normal screen coordinates
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(4096);
	C2D_Prepare();
	C3D_RenderTarget *top = smoothTarget(GFX_TOP, SCR_W);
	C3D_RenderTarget *bot = smoothTarget(GFX_BOTTOM, BOT_W);
	bool smooth = top && bot;
	if (!smooth) {                       // not enough video memory: use the normal screens
		if (top) C3D_RenderTargetDelete(top);
		if (bot) C3D_RenderTargetDelete(bot);
		top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
		bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	}
	textBuf = C2D_TextBufNew(2048);
	srand((unsigned)time(NULL));
	makeMap();
	resetGame();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;
		touchPosition tp = { 0, 0 };
		if (kHeld & KEY_TOUCH) hidTouchRead(&tp);

		if (state == ST_PLAY) update(kDown, kHeld, tp);
		else menuInput(kDown, tp);
		frame++;
		knightClock = (knightClock + 1) % ANIM_LOOP;

		C2D_TextBufClear(textBuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, RGB(15, 42, 51));
		if (smooth) beginSmooth(top);
		else C2D_SceneBegin(top);
		bool lobby = state == ST_LOBBY || state == ST_HELP;
		if (state == ST_PICK) drawPickTop();
		else if (lobby) drawLobbyTop();
		else drawTop();
		C2D_TargetClear(bot, RGB(14, 33, 51));
		if (smooth) beginSmooth(bot);
		else C2D_SceneBegin(bot);
		if (state == ST_PICK) drawPickBottom();
		else if (lobby) drawLobbyBottom();
		else drawBottom();
		C3D_FrameEnd(0);
	}

	C2D_TextBufDelete(textBuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
