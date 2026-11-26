// Lane Rush - a 5v5 MOBA for Nintendo 3DS (libctru + citro2d), top-down, with a Mobile Legends style map
// Blue base bottom-left, red base top-right. Three lanes with 3 turrets each, a river and bridge in the middle,
// jungle between the lanes, 12 hiding bushes (3x3 tiles), jungle camps, river crabs, the Turtle (2:00) and the Lord (8:00).
// Break all 3 turrets of one lane, then the enemy nexus, before the red team does the same to you.
// Controls: Circle Pad / D-Pad = move | Y or R (or touch ATTACK, hold it) = attack | SELECT = auto-attack on/off
//           A (or touch DASH) = dash strike | B (or touch BURST) = shockwave
//           X (or touch ULT) = ultimate, unlocks at level 4 | START = quit
// A voice clip (sdmc:/3ds/lanerush_welcome.wav) is played when a match starts, like the announcer in Mobile Legends.
// Lobby: PROFILE = take a real photo with the 3DS camera (or pick an avatar), change your username and bio, and see your rank (wins raise it, losses lower it; it is saved on the SD card).
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
#define TILE 24                // the map is a grid of 50 x 50 tiles, 24 pixels each
#define LANE_W 52              // width of a lane
#define LANES 3                // top, middle, bottom
#define PATH_PTS 6
#define MAX_ENT 160
#define MAX_FX 40
#define MAX_SPAWN 40
#define WAVE_EVERY 30.0f       // a new minion wave in every lane each 30 seconds
#define FIRST_WAVE 5.0f        // ...the first one at 0:05
#define WAVE_SIZE 3            // minions per team, per lane, in every wave
#define DT (1.0f / 60.0f)
#define ULT_LEVEL 4
#define ULT_CD 30.0f
#define ULT_RADIUS 90.0f

enum { K_MINION, K_HERO, K_CREEP, K_TOWER, K_NEXUS };
#define NEUTRAL 2              // jungle monsters belong to no team
enum { ST_LOBBY, ST_HELP, ST_PLAY, ST_OVER, ST_PICK, ST_PROFILE, ST_CAMERA };   // ST_PICK = choose your hero, ST_PROFILE = your profile, ST_CAMERA = take your photo
enum { M_BATTLE, M_PROFILE, M_HELP, M_COUNT };   // lobby menu entries

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
	int tgt;                   // jungle monsters: the hero they are fighting (-1 = calm)
	int role;                  // heroes: which lane they play, or jungler / roamer
	int bush;                  // which bush the unit stands in (-1 = none)
	int hid;                   // bit 0: hidden from blue, bit 1: hidden from red (hero in a bush)
	float buffB, buffR;        // Blue Buff and Red Buff seconds left
	int emote;                 // emote shown over the hero (0 = none)
	float emoteT;              // seconds the emote stays
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

// entity slots: 0..19 buildings (TOWER(team, lane, tier) for the 18 towers, NEXUS(team) for the two nexuses),
// 20..29 = heroes (20 is you, 21..24 your computer team-mates, 25..29 the red team), 30..41 = jungle and river monsters, 42.. = minions
#define TOWERS_PER_LANE 3
#define TOWER(team, lane, tier) ((team) * 10 + (lane) * TOWERS_PER_LANE + (tier))   // tier 0 = outer, 1 = middle, 2 = inner
#define NEXUS(team) ((team) * 10 + 9)
#define BUILDINGS 20
#define TEAM_SIZE 5                                  // 5v5
#define HERO0 20
#define HERO(team, s) (HERO0 + (team) * TEAM_SIZE + (s))
#define FIRST_CREEP (HERO0 + 2 * TEAM_SIZE)
#define CREEPS 12
#define FIRST_MINION (FIRST_CREEP + CREEPS)
#define TURTLE_SLOT (FIRST_CREEP + 10)
#define LORD_SLOT (FIRST_CREEP + 11)
enum { ROLE_TOP, ROLE_MID, ROLE_BOT, ROLE_JUNGLE, ROLE_ROAM };   // the first three match the lane numbers

// each lane as a path from the blue base to the red base (red walks it backwards)
static const float PATH[LANES][PATH_PTS][2] = {
	{ {110, 1090}, {110, 250}, {160, 160}, {250, 110}, {700, 110}, {1090, 110} },      // top
	{ {110, 1090}, {306, 894}, {502, 698}, {698, 502}, {894, 306}, {1090, 110} },      // middle
	{ {110, 1090}, {500, 1090}, {950, 1090}, {1040, 1040}, {1090, 950}, {1090, 110} }, // bottom
};
// 3 towers in every lane for each team, listed from the outer tower (far from home) to the inner tower (next to the base)
static const float TOWER_POS[2][LANES][TOWERS_PER_LANE][2] = {
	{   // blue
		{ {110, 440},  {110, 650},  {110, 860}  },     // top
		{ {460, 740},  {350, 850},  {240, 960}  },     // middle
		{ {760, 1090}, {550, 1090}, {340, 1090} },     // bottom
	}, {   // red (the blue map mirrored over the river)
		{ {440, 110},  {650, 110},  {860, 110}  },     // top
		{ {740, 460},  {850, 350},  {960, 240}  },     // middle
		{ {1090, 760}, {1090, 550}, {1090, 340} },     // bottom
	},
};

// ---- jungle and river monsters ----
enum { C_BLUEBUFF, C_REDBUFF, C_SMALL, C_CRAB, C_TURTLE, C_LORD };
typedef struct {
	const char *name;
	float r, hp, atk, rng, spd, acd;       // body size and fighting stats
	float first, respawn, xp;              // first appears at (seconds), comes back this long after dying, xp for the killers
	u32 col;                               // colour on the minimap
} CampDef;
static const CampDef CDEF[6] = {
	{ "Blue Buff", 11,  900, 30, 24, 40, 1.1f,  10,  90, 120, RGB(90, 170, 255) },
	{ "Red Buff",  11,  900, 36, 24, 40, 1.1f,  10,  90, 120, RGB(255, 100, 80) },
	{ "Small camp", 8,  420, 20, 20, 40, 1.0f,  10,  60,  45, RGB(214, 170, 90) },
	{ "River crab", 8,  520, 14, 18, 30, 1.2f,  30,  45,  60, RGB(255, 150, 110) },
	{ "Turtle",    16, 2400, 50, 30, 34, 1.5f, 120, 150, 200, RGB(70, 200, 150) },
	{ "Lord",      20, 4200, 70, 34, 30, 1.6f, 480, 180, 350, RGB(190, 130, 255) },
};
typedef struct { int type; float x, y; } Camp;
static const Camp CAMP[CREEPS] = {
	// blue side: Blue Buff + small camp beside the top lane, Red Buff + small camp beside the bottom lane
	{ C_BLUEBUFF, 270, 700 }, { C_REDBUFF, 500, 930 }, { C_SMALL, 260, 540 }, { C_SMALL, 660, 940 },
	// red side (the same camps mirrored)
	{ C_BLUEBUFF, 700, 270 }, { C_REDBUFF, 930, 500 }, { C_SMALL, 540, 260 }, { C_SMALL, 940, 660 },
	// river: two crabs, the Turtle at the top-left end, the Lord at the bottom-right end
	{ C_CRAB, 510, 510 }, { C_CRAB, 690, 690 }, { C_TURTLE, 330, 330 }, { C_LORD, 870, 870 },
};
static int campSide(int c) { return c < 4 ? 0 : c < 8 ? 1 : -1; }   // which team's jungle a camp belongs to (-1 = river)

// ---- bushes: 12 hiding spots of 3 x 3 tiles. The numbers are the tile column and row of the top-left tile. ----
#define BUSHES 12
#define BUSH_TILES 3
#define REVEAL 56.0f           // an enemy this close to a bush (or inside it) reveals what hides there
static const unsigned char BUSH[BUSHES][2] = {
	{ 6, 21 }, { 21, 6 }, { 26, 41 }, { 41, 26 },      // beside the top lane (blue and red half) and the bottom lane (blue and red half)
	{ 22, 29 }, { 29, 22 },                            // beside the middle lane, next to each outer tower
	{ 16, 16 }, { 31, 31 },                            // river
	{ 13, 24 }, { 24, 13 }, { 23, 34 }, { 34, 23 },    // jungle
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

// ---------------------------------------------------------------- player profile (photo, username, bio, rank)
#define NAME_CH 12                      // longest username (characters)
#define BIO_CH 80                       // longest bio (characters)
#define AVATARS 8                       // photos you can choose from
#define BIO_LINES 4
#define BIO_SCALE 0.42f
#define BIO_W 226.0f                    // width the bio is wrapped to on the profile card
#define RANKS 7
#define RANK_WIN 25                     // rank points for a win
#define RANK_LOSS 12                    // rank points lost for a defeat
#define SAVE_PATH "sdmc:/3ds/lanerush_profile.sav"
#define SAVE_MAGIC 0x4C525032u

static char pName[NAME_CH * 4 + 1] = "Player";
static char pBio[BIO_CH * 4 + 1] = "Tell everyone about yourself!";
static int pAvatar, rankPts, rankDelta;
static char bioLines[BIO_LINES][BIO_CH * 4 + 2];
static int bioN;
static C2D_TextBuf measBuf;             // only used to measure text while wrapping the bio

static const char *RANK_NAME[RANKS] = { "Warrior", "Elite", "Master", "Grandmaster", "Epic", "Legend", "Mythic" };
static const int RANK_AT[RANKS] = { 0, 100, 250, 450, 700, 1000, 1400 };   // rank points needed for each tier
static const u32 RANK_COL[RANKS] = {
	RGB(176, 150, 120), RGB(190, 204, 222), RGB(255, 200, 80), RGB(255, 140, 70),
	RGB(200, 110, 255), RGB(90, 210, 255), RGB(255, 90, 110),
};

static int rankTier(int pts) {
	int t = 0;
	for (int i = 0; i < RANKS; i++) if (pts >= RANK_AT[i]) t = i;
	return t;
}

typedef struct { u32 magic; s32 avatar, rankPts, played, victories, hero; char name[NAME_CH * 4 + 1], bio[BIO_CH * 4 + 1]; } SaveData;

static void saveProfile(void) {
	SaveData d;
	memset(&d, 0, sizeof(d));
	d.magic = SAVE_MAGIC;
	d.avatar = pAvatar; d.rankPts = rankPts; d.played = played; d.victories = victories; d.hero = myHero;
	snprintf(d.name, sizeof(d.name), "%s", pName);
	snprintf(d.bio, sizeof(d.bio), "%s", pBio);
	FILE *f = fopen(SAVE_PATH, "wb");
	if (!f) return;                      // no SD card? the profile just lives until the game is closed
	fwrite(&d, sizeof(d), 1, f);
	fclose(f);
}

static void loadProfile(void) {
	SaveData d;
	FILE *f = fopen(SAVE_PATH, "rb");
	if (!f) return;
	size_t n = fread(&d, sizeof(d), 1, f);
	fclose(f);
	if (n != 1 || d.magic != SAVE_MAGIC) return;
	d.name[sizeof(d.name) - 1] = 0;
	d.bio[sizeof(d.bio) - 1] = 0;
	pAvatar = (d.avatar >= 0 && d.avatar <= AVATARS) ? d.avatar : 0;   // AVATARS itself = your camera photo
	rankPts = d.rankPts > 0 ? d.rankPts : 0;
	played = d.played > 0 ? d.played : 0;
	victories = d.victories > 0 ? d.victories : 0;
	myHero = (d.hero >= 0 && d.hero < NHEROES) ? d.hero : 0;
	if (d.name[0]) snprintf(pName, sizeof(pName), "%s", d.name);
	snprintf(pBio, sizeof(pBio), "%s", d.bio);
}

static float textWidth(const char *s, float scale) {
	C2D_Text t;
	float w, h;
	C2D_TextBufClear(measBuf);
	C2D_TextParse(&t, measBuf, s);
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	return w;
}

// cut the bio into lines that fit the profile card (done once, whenever the bio changes)
static void wrapBio(void) {
	char cur[BIO_CH * 4 + 2] = "";
	const char *p = pBio;
	bioN = 0;
	while (*p && bioN < BIO_LINES) {
		while (*p == ' ' || *p == '\n') p++;
		if (!*p) break;
		char word[BIO_CH * 4 + 2], test[2 * BIO_CH * 4 + 4];
		int n = 0;
		while (*p && *p != ' ' && *p != '\n' && n < BIO_CH * 4) word[n++] = *p++;
		word[n] = 0;
		if (!cur[0] && textWidth(word, BIO_SCALE) > BIO_W) {        // one huge word: cut it to fit
			int cut = n;
			while (cut > 1 && textWidth(word, BIO_SCALE) > BIO_W) {
				cut--;
				while (cut > 1 && ((unsigned char)word[cut] & 0xC0) == 0x80) cut--;   // never cut inside a character
				word[cut] = 0;
			}
			p -= (n - cut);
			n = cut;
		}
		snprintf(test, sizeof(test), "%s%s%s", cur, cur[0] ? " " : "", word);
		if (!cur[0] || textWidth(test, BIO_SCALE) <= BIO_W) {
			snprintf(cur, sizeof(cur), "%s", test);
		} else {                                                    // it does not fit: finish the line, redo this word on the next
			snprintf(bioLines[bioN++], sizeof(bioLines[0]), "%s", cur);
			cur[0] = 0;
			p -= n;
		}
	}
	if (cur[0] && bioN < BIO_LINES) snprintf(bioLines[bioN++], sizeof(bioLines[0]), "%s", cur);
}

// opens the 3DS keyboard; returns true when the player confirmed new text
static bool askText(char *dst, size_t dstSize, const char *hint, int maxChars, bool required) {
	SwkbdState kb;
	char tmp[BIO_CH * 4 + 8];
	memset(tmp, 0, sizeof(tmp));
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, maxChars);
	swkbdSetHintText(&kb, hint);
	swkbdSetInitialText(&kb, dst);
	swkbdSetValidation(&kb, required ? SWKBD_NOTEMPTY_NOTBLANK : SWKBD_ANYTHING, 0, 0);
	if (swkbdInputText(&kb, tmp, sizeof(tmp)) != SWKBD_BUTTON_CONFIRM) return false;
	for (char *c = tmp; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
	snprintf(dst, dstSize, "%s", tmp);
	return true;
}

static void editName(void) { if (askText(pName, sizeof(pName), "Enter your username", NAME_CH, true)) saveProfile(); }
static void editBio(void) { if (askText(pBio, sizeof(pBio), "Write something about yourself", BIO_CH, false)) { wrapBio(); saveProfile(); } }

// ---- real photo from the 3DS camera ----
#define PHOTO_ID AVATARS                // avatar number of your camera photo (the emblems are 0..AVATARS-1)
#define PHOTO_N 64                      // the saved photo is 64 x 64 pixels
#define PHOTO_PATH "sdmc:/3ds/lanerush_photo.bin"
#define PHOTO_MAGIC 0x4C525048u
#define CAM_W 320                       // camera picture size
#define CAM_H 240
#define CAM_SQ 240                      // the centre 240 x 240 of the picture is the photo
#define CAM_X0 ((CAM_W - CAM_SQ) / 2)
#define PREV_N 256                      // texture size of the live preview

static u16 photoPix[PHOTO_N * PHOTO_N]; // your photo, RGB565
static bool photoOK, photoTexOK, prevOK;
static C3D_Tex photoTex, prevTex;
static Tex3DS_SubTexture photoSub = { PHOTO_N, PHOTO_N, 0.0f, 1.0f, 1.0f, 0.0f };
static Tex3DS_SubTexture prevSub = { CAM_SQ, CAM_SQ, 0.0f, 1.0f, CAM_SQ / (float)PREV_N, 1.0f - CAM_SQ / (float)PREV_N };
static C2D_Image photoImg = { &photoTex, &photoSub };
static C2D_Image prevImg = { &prevTex, &prevSub };
static const char *profMsg = "";        // short message on the profile screen (for example: camera not available)
static int profMsgT;
static bool camOpen, camInner = true, camPending, camShot;
static Handle camEvent;
static u16 *camBuf;
static u32 camMax;
static int camFrames;
static bool texFlip;                    // set by the "upside down?" button on the camera screen (kept on the SD card)
#define CAMCFG_PATH "sdmc:/3ds/lanerush_cam.cfg"

// the GPU keeps textures in 8 x 8 tiles with the pixels of a tile in Z-order, and upside down
static u32 morton8(u32 x, u32 y) {
	x &= 7; y &= 7;
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
static void texPut(u16 *dst, int n, int x, int y, u16 v) {
	int ym = texFlip ? y : n - 1 - y;   // texFlip turns every picture over (the camera screen has a button for it)
	dst[((((ym >> 3) * (n >> 3)) + (x >> 3)) << 6) + morton8(x, ym)] = v;
}

static void saveCamCfg(void) {
	FILE *f = fopen(CAMCFG_PATH, "wb");
	if (!f) return;
	fputc(texFlip ? 1 : 0, f);
	fclose(f);
}
static void loadCamCfg(void) {
	FILE *f = fopen(CAMCFG_PATH, "rb");
	if (!f) return;
	int c = fgetc(f);
	fclose(f);
	texFlip = c == 1;
}

static void initPhotoTextures(void) {
	photoTexOK = C3D_TexInit(&photoTex, PHOTO_N, PHOTO_N, GPU_RGB565);
	if (photoTexOK) {
		C3D_TexSetFilter(&photoTex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&photoTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memset(photoTex.data, 0, PHOTO_N * PHOTO_N * 2);
	}
	prevOK = C3D_TexInit(&prevTex, PREV_N, PREV_N, GPU_RGB565);
	if (prevOK) {
		C3D_TexSetFilter(&prevTex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&prevTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		memset(prevTex.data, 0, PREV_N * PREV_N * 2);
	}
}

static void photoToTexture(void) {
	if (!photoTexOK) return;
	for (int y = 0; y < PHOTO_N; y++)
		for (int x = 0; x < PHOTO_N; x++) texPut((u16 *)photoTex.data, PHOTO_N, x, y, photoPix[y * PHOTO_N + x]);
	C3D_TexFlush(&photoTex);
}

static void savePhoto(void) {
	u32 magic = PHOTO_MAGIC;
	FILE *f = fopen(PHOTO_PATH, "wb");
	if (!f) return;
	fwrite(&magic, sizeof(magic), 1, f);
	fwrite(photoPix, sizeof(photoPix), 1, f);
	fclose(f);
}

static void loadPhoto(void) {
	u32 magic = 0;
	if (!photoTexOK) return;
	FILE *f = fopen(PHOTO_PATH, "rb");
	if (!f) return;
	bool ok = fread(&magic, sizeof(magic), 1, f) == 1 && magic == PHOTO_MAGIC &&
	          fread(photoPix, sizeof(photoPix), 1, f) == 1;
	fclose(f);
	if (!ok) return;
	photoOK = true;
	photoToTexture();
}

// ---- announcer voice: plays sdmc:/3ds/lanerush_welcome.wav when a match starts ----
// The file must be a 16-bit PCM WAV (mono or stereo, any sample rate). No file or no DSP firmware = the game stays silent.
#define WELCOME_PATH "sdmc:/3ds/lanerush_welcome.wav"
#define WELCOME_MAX (2 * 1024 * 1024)
static bool audioOK;
static u8 *welcomeData;
static u32 welcomeFrames, welcomeRate;
static int welcomeCh;
static ndspWaveBuf welcomeBuf;

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

static void initAudio(void) {
	if (R_FAILED(ndspInit())) return;                // needs dspfirm.cdc on the SD card (dump it with the DSP1 app)
	audioOK = true;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	FILE *f = fopen(WELCOME_PATH, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 44 || size > WELCOME_MAX) { fclose(f); return; }
	u8 *buf = (u8 *)malloc((size_t)size);
	if (!buf) { fclose(f); return; }
	bool ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!ok || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) { free(buf); return; }
	u32 fmt = 0, ch = 0, rate = 0, bits = 0, dlen = 0;
	const u8 *data = NULL;
	for (long pos = 12; pos + 8 <= size; ) {         // walk through the chunks of the WAV file
		u32 len = rd32(buf + pos + 4);
		const u8 *body = buf + pos + 8;
		if (!memcmp(buf + pos, "fmt ", 4) && len >= 16 && pos + 8 + 16 <= size) {
			fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
		} else if (!memcmp(buf + pos, "data", 4)) {
			data = body;
			dlen = len;
			if ((long)dlen > size - (pos + 8)) dlen = (u32)(size - (pos + 8));
			break;
		}
		pos += 8 + (long)len + (long)(len & 1);
	}
	if (fmt == 1 && bits == 16 && (ch == 1 || ch == 2) && rate > 0 && data && dlen >= 2 * ch) {
		welcomeFrames = dlen / (2 * ch);
		welcomeCh = (int)ch;
		welcomeRate = rate;
		welcomeData = (u8 *)linearAlloc(welcomeFrames * 2 * ch);   // the DSP reads from linear memory
		if (welcomeData) {
			memcpy(welcomeData, data, welcomeFrames * 2 * ch);
			DSP_FlushDataCache(welcomeData, welcomeFrames * 2 * ch);
		}
	}
	free(buf);
}

static void playWelcome(void) {
	if (!audioOK || !welcomeData) return;
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)welcomeRate);
	ndspChnSetFormat(0, welcomeCh == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	float mix[12] = { 1.0f, 1.0f };
	ndspChnSetMix(0, mix);
	memset(&welcomeBuf, 0, sizeof(welcomeBuf));
	welcomeBuf.data_vaddr = welcomeData;
	welcomeBuf.nsamples = welcomeFrames;
	welcomeBuf.looping = false;
	ndspChnWaveBufAdd(0, &welcomeBuf);
}

static void exitAudio(void) {
	if (!audioOK) return;
	ndspChnReset(0);
	if (welcomeData) linearFree(welcomeData);
	welcomeData = NULL;
	ndspExit();
	audioOK = false;
}

static void camStop(void) {
	if (!camOpen) return;
	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);
	if (camPending) { svcCloseHandle(camEvent); camPending = false; }
	camExit();
	camOpen = false;
	free(camBuf);
	camBuf = NULL;
}

// starts the selfie camera (inner = true) or the outer camera; false if the camera cannot be used
static bool camStart(bool inner) {
	CAMU_Select sel = inner ? SELECT_IN1 : SELECT_OUT1;
	u32 maxBytes = 0;
	if (!prevOK || !photoTexOK) return false;
	camBuf = (u16 *)malloc(CAM_W * CAM_H * 2);
	if (!camBuf) return false;
	if (R_FAILED(camInit())) { free(camBuf); camBuf = NULL; return false; }
	camOpen = true;
	camInner = inner;
	camFrames = 0;
	camPending = false;
	camShot = false;
	CAMU_SetSize(sel, SIZE_QVGA, CONTEXT_A);
	CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetNoiseFilter(sel, true);
	CAMU_SetAutoExposure(sel, true);
	CAMU_SetAutoWhiteBalance(sel, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	if (R_FAILED(CAMU_GetMaxBytes(&maxBytes, CAM_W, CAM_H))) { camStop(); return false; }
	camMax = maxBytes;
	CAMU_SetTransferBytes(PORT_CAM1, maxBytes, CAM_W, CAM_H);
	CAMU_Activate(sel);
	CAMU_ClearBuffer(PORT_CAM1);
	if (R_FAILED(CAMU_StartCapture(PORT_CAM1))) { camStop(); return false; }
	return true;
}

// turns the latest camera picture into your 64 x 64 photo
static void camMakePhoto(void) {
	for (int dy = 0; dy < PHOTO_N; dy++)
		for (int dx = 0; dx < PHOTO_N; dx++) {
			int x0 = dx * CAM_SQ / PHOTO_N, x1 = (dx + 1) * CAM_SQ / PHOTO_N;
			int y0 = dy * CAM_SQ / PHOTO_N, y1 = (dy + 1) * CAM_SQ / PHOTO_N;
			int r = 0, g = 0, b = 0, n = 0;
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++) {
					u16 px = camBuf[y * CAM_W + CAM_X0 + (camInner ? CAM_SQ - 1 - x : x)];
					r += (px >> 11) & 31; g += (px >> 5) & 63; b += px & 31; n++;
				}
			if (n == 0) n = 1;
			photoPix[dy * PHOTO_N + dx] = (u16)(((r / n) << 11) | ((g / n) << 5) | (b / n));
		}
	photoOK = true;
	photoToTexture();
	savePhoto();
}

// called every frame while the camera screen is open: shows the live picture and takes the photo when asked
static void camPoll(void) {
	if (!camOpen || state != ST_CAMERA) return;
	if (!camPending) {
		if (R_FAILED(CAMU_SetReceiving(&camEvent, camBuf, PORT_CAM1, CAM_W * CAM_H * 2, (s16)camMax))) return;
		camPending = true;
	}
	if (svcWaitSynchronization(camEvent, 0) != 0) return;       // the next picture is not ready yet
	svcCloseHandle(camEvent);
	camPending = false;
	camFrames++;
	for (int y = 0; y < CAM_SQ; y++) {                          // live preview (the selfie camera is mirrored, like a mirror)
		const u16 *row = camBuf + y * CAM_W + CAM_X0;
		for (int x = 0; x < CAM_SQ; x++) texPut((u16 *)prevTex.data, PREV_N, x, y, row[camInner ? CAM_SQ - 1 - x : x]);
	}
	C3D_TexFlush(&prevTex);
	if (camShot) {
		camShot = false;
		CAMU_PlayShutterSound(SHUTTER_SOUND_NORMAL);
		camMakePhoto();
		pAvatar = PHOTO_ID;
		saveProfile();
		camStop();
		state = ST_PROFILE;
	}
}
static float dist(const Ent *a, const Ent *b) { float dx = a->x - b->x, dy = a->y - b->y; return sqrtf(dx * dx + dy * dy); }
// a nexus can be attacked once one whole lane of its team's towers has fallen
static bool laneOpen(int team) {
	for (int l = 0; l < LANES; l++)
		if (E[TOWER(team, l, 0)].hp <= 0 && E[TOWER(team, l, 1)].hp <= 0 && E[TOWER(team, l, 2)].hp <= 0) return true;
	return false;
}
static bool shielded(int team) { return !laneOpen(team); }
// shielded buildings cannot be hurt: the nexus until a lane is open, a tower until the one in front of it has fallen
static bool shieldedEnt(int i) {
	const Ent *e = &E[i];
	if (e->kind == K_NEXUS) return !laneOpen(e->team);
	if (e->kind == K_TOWER && e->wp > 0) return E[i - 1].hp > 0;
	return false;
}
static float homeX(int team) { return team ? WORLD - 150 : 150; }
static float homeY(int team) { return team ? 150 : WORLD - 150; }
static float spawnX(int team, int s) { return homeX(team) + (s % 3 - 1) * 22.0f; }      // five heroes stand in two rows
static float spawnY(int team, int s) { return homeY(team) + (s / 3) * 22.0f - 11.0f; }
static int bushAt(float x, float y) {            // which bush covers this point (-1 = none)
	for (int b = 0; b < BUSHES; b++) {
		float bx = BUSH[b][0] * TILE, by = BUSH[b][1] * TILE;
		if (x >= bx && x < bx + BUSH_TILES * TILE && y >= by && y < by + BUSH_TILES * TILE) return b;
	}
	return -1;
}
// only you and the jungler of each team fight jungle monsters
static bool isHunter(const Ent *u) { return u->kind == K_HERO && (u == &E[HERO0] || u->role == ROLE_JUNGLE); }

static char notice[48];                          // a short message at the top of the screen
static float noticeT;
static float lordUntil[2];                       // until when each team's minions carry the Lord's blessing
// emotes: little faces over a hero, sent with the L and R buttons (R = next emote, L = previous emote)
enum { EM_DAZED = 1, EM_LOVE, EM_LAUGH, EM_CRY, EMOTES = 4 };
#define EMOTE_TIME 2.6f
static int emoteSel = EMOTES - 1;           // the emote you sent last; R goes to the next one, L to the previous one
static void sendEmote(Ent *u, int type) { u->emote = type; u->emoteT = EMOTE_TIME; }
static void moodEmote(Ent *u, int type) { if (u->emoteT <= 0) sendEmote(u, type); }   // computer heroes do not spam
static void announce(const char *msg) { snprintf(notice, sizeof(notice), "%s", msg); noticeT = 4.0f; }

static void addFx(int kind, float x1, float y1, float x2, float y2, float t, float r, int team, int val) {
	if (nFx >= MAX_FX) nFx = MAX_FX - 1;          // overwrite the newest one when full
	fxs[nFx++] = (Fx){ kind, x1, y1, x2, y2, t, r, team, val };
}

static void setEnt(Ent *e, int kind, int team) {
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->team = team;
	e->face = team == 1 ? 2.356f : -0.785f;
	e->tgt = -1;
	e->bush = -1;
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

static const int ROLE_OF[2][TEAM_SIZE] = {
	{ ROLE_MID, ROLE_TOP, ROLE_BOT, ROLE_MID, ROLE_JUNGLE },      // blue: you (free to go anywhere), then four computer team-mates
	{ ROLE_TOP, ROLE_BOT, ROLE_MID, ROLE_JUNGLE, ROLE_ROAM },     // red: five computer players
};

static void resetGame(void) {
	nE = FIRST_MINION;
	nFx = 0;
	nQ = 0;
	T = 0;
	nextWave = FIRST_WAVE;
	kills = 0;
	emoteSel = EMOTES - 1;
	lordUntil[0] = lordUntil[1] = 0;
	noticeT = 0;
	for (int t = 0; t < 2; t++) {
		for (int l = 0; l < LANES; l++)
			for (int k = 0; k < TOWERS_PER_LANE; k++) {
				Ent *tw = &E[TOWER(t, l, k)];
				setEnt(tw, K_TOWER, t);
				tw->lane = l;
				tw->wp = k;                                    // the tier
				tw->hp = tw->max = k == 0 ? 1500.0f : k == 1 ? 1800.0f : 2100.0f;
				tw->x = TOWER_POS[t][l][k][0];
				tw->y = TOWER_POS[t][l][k][1];
			}
		Ent *nx = &E[NEXUS(t)];
		setEnt(nx, K_NEXUS, t);
		nx->x = PATH[1][t ? PATH_PTS - 1 : 0][0];
		nx->y = PATH[1][t ? PATH_PTS - 1 : 0][1];
		for (int s = 0; s < TEAM_SIZE; s++) {
			Ent *h = &E[HERO(t, s)];
			setEnt(h, K_HERO, t);
			applyHero(h, ((t ? enemyHero : myHero) + s) % NHEROES);
			h->role = ROLE_OF[t][s];
			h->lane = h->role < LANES ? h->role : ROLE_MID;
			h->wp = 1;
			h->x = spawnX(t, s);
			h->y = spawnY(t, s);
		}
	}
	for (int i = 0; i < CREEPS; i++) {
		Ent *c = &E[FIRST_CREEP + i];
		const CampDef *d = &CDEF[CAMP[i].type];
		setEnt(c, K_CREEP, NEUTRAL);
		c->type = CAMP[i].type;
		c->r = d->r; c->hp = c->max = d->hp; c->atk = d->atk; c->rng = d->rng; c->spd = d->spd; c->acd = d->acd;
		c->x = c->hx = CAMP[i].x;
		c->y = c->hy = CAMP[i].y;
		c->hp = 0;                               // not there yet: it appears after its first spawn time
		c->rt = d->first;
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
		if (e->kind == K_CREEP && !isHunter(u)) continue;                  // only you and the jungler hunt monsters
		if (u->kind == K_CREEP && e->kind != K_HERO) continue;
		if (u->team < 2 && ((e->hid >> u->team) & 1)) continue;            // hiding in a bush
		float dx = u->x - e->x, dy = u->y - e->y, lim = range + e->r;
		float d2 = dx * dx + dy * dy;
		if (d2 > lim * lim) continue;
		if ((e->kind == K_NEXUS || e->kind == K_TOWER) && shieldedEnt(i)) continue;
		float d = sqrtf(d2) - e->r;
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

// what the team gets for taking a jungle or river monster
static void creepReward(const Ent *c, int by) {
	if (by < 0 || E[by].kind != K_HERO) return;
	Ent *k = &E[by];
	int team = k->team;
	if (c->type != C_SMALL && c->type != C_CRAB) sendEmote(k, EM_LOVE);   // buffs, Turtle and Lord make heroes happy
	switch (c->type) {
	case C_BLUEBUFF: k->buffB = 70; break;                                  // skills recharge faster
	case C_REDBUFF:  k->buffR = 70; break;                                  // +25% damage
	case C_SMALL:    k->hp = fminf(k->max, k->hp + k->max * 0.15f); break;
	case C_CRAB:     k->hp = fminf(k->max, k->hp + k->max * 0.25f); break;
	case C_TURTLE:                                                          // the whole team grows stronger
		for (int s = 0; s < TEAM_SIZE; s++) {
			Ent *h = &E[HERO(team, s)];
			if (h->hp <= 0) continue;
			gainXp(h, 150);
			h->hp = fminf(h->max, h->hp + h->max * 0.25f);
		}
		announce(team ? "Red team slayed the Turtle" : "Blue team slayed the Turtle");
		break;
	case C_LORD:                                                            // heals the team, and stronger minions for 90 seconds
		for (int s = 0; s < TEAM_SIZE; s++) {
			Ent *h = &E[HERO(team, s)];
			if (h->hp > 0) h->hp = fminf(h->max, h->hp + h->max * 0.3f);
		}
		lordUntil[team] = T + 90.0f;
		announce(team ? "Red team slayed the Lord" : "Blue team slayed the Lord");
		break;
	default: break;
	}
}

static void die(int i, int by) {
	Ent *t = &E[i];
	t->hp = 0;
	int kt = by >= 0 ? E[by].team : -1;                  // the team that landed the last hit
	float xp = t->kind == K_NEXUS ? 0 : t->kind == K_TOWER ? 90 : t->kind == K_HERO ? 110 : t->kind == K_CREEP ? CDEF[t->type].xp : 18;
	if (t->kind == K_CREEP) {
		t->rt = CDEF[t->type].respawn;                   // monsters come back when their respawn timer ends
		t->tgt = -1;
		creepReward(t, by);
	}
	if (t->kind == K_HERO) {
		t->rt = 5.0f + t->lvl * 1.5f;
		if (t->team == 1) kills++;
		if (by >= 0 && E[by].kind == K_HERO) sendEmote(&E[by], EM_LAUGH);   // the one who got the kill laughs
	}
	for (int h = HERO0; h < HERO0 + 2 * TEAM_SIZE; h++) {
		const Ent *g = &E[h];
		if (g->hp <= 0 || g->team == t->team) continue;
		if (t->kind == K_CREEP && g->team != kt) continue;       // only the team that took the camp learns from it
		if (dist(g, t) < 150) gainXp(&E[h], xp);
	}
	addFx(1, t->x, t->y, 0, 0, 0.45f, t->r * 2, t->team, 0);
	if (t->kind == K_NEXUS) endGame(t->team == 1);
}

static void hit(int i, float amount, int by) {
	Ent *t = &E[i];
	if (t->hp <= 0 || shieldedEnt(i)) return;
	if (by >= 0 && E[by].buffR > 0) amount *= 1.25f;
	if (by >= 0 && t->kind == K_CREEP && E[by].kind == K_HERO) t->tgt = by;   // the monster fights back at this hero
	t->hp -= amount;
	if (t->kind == K_HERO) addFx(2, t->x, t->y - t->r - 4, 0, 0, 0.7f, 0, t->team, (int)amount);
	if (t->hp <= 0 && t->max > 0) die(i, by);
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
	int me = (int)(u - E);
	float x1 = u->x, y1 = u->y;
	u->x = clampf(u->x + cosf(u->face) * HEROES[u->type].dash, 30, WORLD - 30);
	u->y = clampf(u->y + sinf(u->face) * HEROES[u->type].dash, 30, WORLD - 30);
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || e->kind >= K_TOWER) continue;
		if (e->kind == K_CREEP && !isHunter(u)) continue;
		if (segDist(x1, y1, u->x, u->y, e->x, e->y) < e->r + 10) hit(i, 70 + u->lvl * 14, me);
	}
	addFx(0, x1, y1, u->x, u->y, 0.25f, 4, u->team, 0);
}

static void burst(Ent *u) {                      // a shockwave around the hero
	if (u->s2 > 0 || u->hp <= 0) return;
	u->s2 = 9;
	int me = (int)(u - E);
	const float R = HEROES[u->type].burst;
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || (e->kind == K_CREEP && !isHunter(u))) continue;
		if (dist(u, e) < R + e->r) hit(i, e->kind >= K_TOWER ? 60 : 110 + u->lvl * 20, me);
	}
	addFx(1, u->x, u->y, 0, 0, 0.5f, R, u->team, 0);
}

static void ultimate(Ent *u) {                   // a huge blast, unlocked at ULT_LEVEL
	if (u->lvl < ULT_LEVEL || u->s3 > 0 || u->hp <= 0) return;
	u->s3 = ULT_CD;
	int me = (int)(u - E);
	for (int i = 0; i < nE; i++) {
		Ent *e = &E[i];
		if (e->team == u->team || e->hp <= 0 || (e->kind == K_CREEP && !isHunter(u))) continue;
		if (dist(u, e) < HEROES[u->type].ult + e->r) hit(i, e->kind >= K_TOWER ? 120 : 220 + u->lvl * 30, me);
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

// where a lane hero waits for the next minions: at the outermost tower of its lane that still stands
static void standPoint(int team, int lane, float *x, float *y) {
	for (int k = 0; k < TOWERS_PER_LANE; k++)
		if (E[TOWER(team, lane, k)].hp > 0) { *x = TOWER_POS[team][lane][k][0]; *y = TOWER_POS[team][lane][k][1]; return; }
	*x = E[NEXUS(team)].x;
	*y = E[NEXUS(team)].y;
}

// the jungler farms the camps on its own side and in the river, and takes the Turtle (level 5) and the Lord (level 8)
static bool aiJungler(Ent *u, float *mx, float *my) {
	int best = -1;
	float bd = 1e9f;
	for (int c = 0; c < CREEPS; c++) {
		const Ent *e = &E[FIRST_CREEP + c];
		if (e->hp <= 0) continue;
		int side = campSide(c);
		if (side >= 0 && side != u->team) continue;              // stay out of the enemy jungle
		if (e->type == C_TURTLE && u->lvl < 5) continue;
		if (e->type == C_LORD && u->lvl < 8) continue;
		float d = hypotf(e->x - u->x, e->y - u->y);
		if (d < bd) { bd = d; best = c; }
	}
	if (best < 0) return false;                                  // nothing to farm: play like a lane hero for now
	const Ent *e = &E[FIRST_CREEP + best];
	u->face = atan2f(e->y - u->y, e->x - u->x);
	float d = bd - e->r;
	if (d > u->rng * 0.85f) toward(u, e->x, e->y, mx, my);
	else if (d < 50 && u->s2 <= 0) burst(u);
	return true;
}

static void aiHero(Ent *u, float *mx, float *my) {
	*mx = *my = 0;
	if (u->hp < u->max * 0.3f) { if (!u->low) moodEmote(u, EM_CRY); u->low = true; }   // low health: cries and runs home
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
	if (u->role == ROLE_JUNGLE && aiJungler(u, mx, my)) return;
	int lane = u->role < LANES ? u->role : (1 + (int)(T / 50)) % LANES;   // roamers change lane every 50 seconds
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
		if (front < 0) {
			float sx, sy;
			standPoint(u->team, lane, &sx, &sy);
			toward(u, sx, sy, mx, my);
		} else toward(u, p[0], p[1], mx, my);
	}
}

// jungle monsters only fight back at whoever hit them, and give up if you lead them too far from home
#define LEASH 150.0f
static void aiCreep(Ent *u, float *mx, float *my) {
	*mx = *my = 0;
	Ent *h = u->tgt >= 0 ? &E[u->tgt] : NULL;
	if (h && (h->hp <= 0 || hypotf(h->x - u->hx, h->y - u->hy) > (u->type >= C_TURTLE ? LEASH + 60 : LEASH))) { u->tgt = -1; h = NULL; }
	if (h) {
		u->face = atan2f(h->y - u->y, h->x - u->x);
		if (dist(u, h) - h->r > u->rng * 0.8f) toward(u, h->x, h->y, mx, my);
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
	if (win) { victories++; rankDelta = RANK_WIN; }
	else { rankDelta = -RANK_LOSS; if (rankPts + rankDelta < 0) rankDelta = -rankPts; }
	rankPts += rankDelta;
	saveProfile();
	state = ST_OVER;
}

static void addMinion(int team, int lane) {
	if (nE >= MAX_ENT) return;
	Ent *m = &E[nE++];
	setEnt(m, K_MINION, team);
	m->lane = lane;
	m->wp = 1;
	if (T < lordUntil[team]) { m->hp = m->max = 400; m->atk = 22; m->r = 6.5f; }   // the Lord's blessing: stronger minions
	const float *a = lanePoint(team, lane, 0);
	m->x = a[0] + (rand() % 21) - 10;
	m->y = a[1] + (rand() % 21) - 10;
}

// who hides in a bush, and who can see them. Only heroes hide. A hero in a bush is shown to the other team
// when one of its units is in the same bush or stands right next to it.
static void updateVision(void) {
	bool seen[2][BUSHES];
	memset(seen, 0, sizeof(seen));
	for (int i = HERO0; i < nE; i++) {
		Ent *e = &E[i];
		e->hid = 0;
		e->bush = (e->hp > 0 && e->kind != K_CREEP) ? bushAt(e->x, e->y) : -1;
		if (e->bush >= 0 && e->team < 2) seen[e->team][e->bush] = true;
	}
	for (int i = HERO0; i < HERO0 + 2 * TEAM_SIZE; i++) {
		Ent *e = &E[i];
		if (e->bush < 0) continue;
		int other = 1 - e->team;
		bool shown = seen[other][e->bush];
		for (int j = HERO0; j < nE && !shown; j++) {
			const Ent *o = &E[j];
			if (o->team != other || o->hp <= 0) continue;
			float dx = o->x - e->x, dy = o->y - e->y;
			if (dx * dx + dy * dy < REVEAL * REVEAL) shown = true;
		}
		if (!shown) e->hid |= 1 << other;
	}
}

static void update(u32 kDown, u32 kHeld, touchPosition tp) {
	Ent *me = &E[HERO0];
	T += DT;
	if (noticeT > 0) noticeT -= DT;
	if (T >= nextWave) {
		nextWave += WAVE_EVERY;
		for (int m = 0; m < WAVE_SIZE; m++)
			for (int l = 0; l < LANES && nQ < MAX_SPAWN; l++) spawnQ[nQ++] = (Spawn){ T + m * 0.8f, l };
	}
	for (int i = nQ - 1; i >= 0; i--)
		if (spawnQ[i].at <= T) {
			if (nE + 2 <= MAX_ENT) { addMinion(0, spawnQ[i].lane); addMinion(1, spawnQ[i].lane); }   // both teams or neither
			spawnQ[i] = spawnQ[--nQ];
		}
	updateVision();

	// your hero: skills first, then movement
	if (kDown & KEY_R) { emoteSel = (emoteSel + 1) % EMOTES; sendEmote(me, emoteSel + 1); }            // R: next emote
	if (kDown & KEY_L) { emoteSel = (emoteSel + EMOTES - 1) % EMOTES; sendEmote(me, emoteSel + 1); }   // L: previous emote
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
	bool atkHeld = (kHeld & KEY_Y) ||                // attack button: Y or the touch button
				   ((kHeld & KEY_TOUCH) && tp.py >= 150 && tp.py < 220 && tp.px >= 243 && tp.px < 317);
	circlePosition cp;
	hidCircleRead(&cp);

	for (int i = HERO0; i < nE; i++) {
		Ent *u = &E[i];
		u->cd -= DT; u->s1 -= DT; u->s2 -= DT; u->s3 -= DT;   // cooldowns run even while dead
		if (u->emoteT > 0) u->emoteT -= DT;
		if (u->buffB > 0) { u->buffB -= DT; u->s1 -= DT * 0.5f; u->s2 -= DT * 0.5f; u->s3 -= DT * 0.5f; }   // Blue Buff
		if (u->buffR > 0) u->buffR -= DT;                                                                  // Red Buff
		if (u->hp <= 0) {                        // fallen heroes and monsters come back after a while
			if (u->kind == K_HERO || u->kind == K_CREEP) {
				u->rt -= DT;
				if (u->rt <= 0) {
					u->hp = u->max;
					if (u->kind == K_HERO) {
						int s = (i - HERO0) % TEAM_SIZE;
						u->x = spawnX(u->team, s);
						u->y = spawnY(u->team, s);
						u->buffB = u->buffR = 0;
						u->low = false;
						sendEmote(u, EM_DAZED);          // just woke up at the base
					} else {
						u->x = u->hx;
						u->y = u->hy;
						u->tgt = -1;
						if (u->type == C_TURTLE) announce("The Turtle has spawned");
						else if (u->type == C_LORD) announce("The Lord has spawned");
					}
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
		int tg;
		if (u->kind == K_CREEP)                  // monsters only hit the hero that attacked them
			tg = (u->tgt >= 0 && dist(u, &E[u->tgt]) - E[u->tgt].r <= u->rng) ? u->tgt : -1;
		else tg = pick(u, u->rng);               // everyone else auto-attacks the nearest enemy in range
		if (tg >= 0) {
			if (mx == 0 && my == 0) u->face = atan2f(E[tg].y - u->y, E[tg].x - u->x);
			if (u->cd <= 0 && (u != me || autoAtk || atkHeld)) {      // you hit only when you press attack (or auto is on)
				u->cd = u->acd;
				addFx(0, u->x, u->y, E[tg].x, E[tg].y, 0.14f, 1.5f, u->team, 0);
				hit(tg, u->atk, i);
			}
		}
		if (u->kind == K_HERO && dist(u, &E[NEXUS(u->team)]) < 100)   // heal next to your own nexus
			u->hp = fminf(u->max, u->hp + 90 * DT);
	}

	for (int i = HERO0; i < nE; i++) {           // units push each other apart a little
		Ent *a = &E[i];
		if (a->hp <= 0) continue;
		for (int j = i + 1; j < nE; j++) {
			Ent *b = &E[j];
			if (b->hp <= 0) continue;
			float dx = b->x - a->x, dy = b->y - a->y, m = a->r + b->r;
			if (fabsf(dx) >= m || fabsf(dy) >= m) continue;
			float d2 = dx * dx + dy * dy;
			if (d2 >= m * m) continue;
			float d = sqrtf(d2);
			if (d <= 0.01f) { dx = 1; dy = 0; d = 1; }       // stacked units: pick a direction
			float p = (m - d) * 0.25f / d;
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
			hit(tg, b->atk, i);
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
	if (shieldedEnt((int)(e - E))) oval(x, y - 16, 36, 60, RGBA(200, 235, 255, 30));   // protected by the tower in front
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
		oval(x, feet + 1, 27, 10, e == &E[HERO0] ? RGBA(255, 214, 100, 170) : e->team ? RGBA(255, 90, 106, 140) : RGBA(90, 170, 255, 140));
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
	float k = e->r / 9.0f, b = sinf(frame * 0.08f + e->hx) * 1.2f;
	float d = cosf(e->face) < 0 ? -1.0f : 1.0f;
	oval(x, y + 8 * k, 24 * k, 8 * k, RGBA(0, 0, 0, 100));
	if (e->type == C_CRAB) {                                     // river crab: orange shell, big claws, eyes on stalks
		float s = e->r / 8.0f, sn = sinf(frame * 0.12f + e->hx) * 2 * s;
		u32 deep = RGB(168, 60, 40), shell = RGB(240, 112, 72);
		for (int side = -1; side <= 1; side += 2) {
			for (int l = 0; l < 3; l++)
				thick(x + side * 6 * s, y + 2 * s + l * 2 * s, x + side * (13 + l * 2) * s, y + (6 + l * 3) * s + b, 1.6f, deep);
			thick(x + side * 8 * s, y - 1 * s, x + side * 13 * s, y - 6 * s + sn * side, 2.2f, deep);
			disc(x + side * 14 * s, y - 7 * s + sn * side, 4.2f * s, RGB(255, 128, 84));
			tri(x + side * 14 * s, y - 7 * s + sn * side, x + side * 19 * s, y - 11 * s + sn * side, x + side * 18 * s, y - 5 * s + sn * side, deep);
		}
		oval(x, y + 2 * s, 22 * s, 13 * s, deep);
		oval(x, y, 20 * s, 11 * s, shell);
		oval(x - 3 * s, y - 2 * s, 9 * s, 4 * s, RGB(255, 176, 124));
		for (int side = -1; side <= 1; side += 2) {
			thick(x + side * 3 * s, y - 4 * s, x + side * 3 * s, y - 9 * s, 1.2f, deep);
			disc(x + side * 3 * s, y - 10 * s, 2.2f * s, RGB(255, 255, 255));
			disc(x + side * 3 * s + d * 0.6f, y - 10 * s, 1.1f * s, RGB(20, 20, 30));
		}
		bar(x, y - 17 * s, 20 * s, e->hp / e->max, RGB(255, 200, 87));
		return;
	}
	if (e->type == C_TURTLE) {                                   // Turtle: big green shell, head and flippers
		float s = e->r / 16.0f;
		glow(x, y, 34, 70, 200, 150);
		oval(x - d * 18 * s, y + 6 * s, 10 * s, 6 * s, RGB(84, 160, 100));                       // tail
		oval(x - 13 * s, y + 9 * s, 14 * s, 8 * s, RGB(96, 176, 104));                           // flippers
		oval(x + 13 * s, y + 9 * s, 14 * s, 8 * s, RGB(96, 176, 104));
		oval(x, y + 3 * s, 46 * s, 22 * s, RGB(44, 112, 82));
		oval(x, y - 1 * s, 42 * s, 24 * s, RGB(66, 160, 116));
		oval(x - 4 * s, y - 5 * s, 24 * s, 11 * s, RGB(94, 198, 144));
		for (int i = -1; i <= 1; i++) oval(x + i * 11 * s, y - 1 * s + (i == 0 ? -2 * s : 1 * s), 9 * s, 6 * s, RGB(48, 124, 92));
		disc(x + d * 21 * s, y - 2 * s + b, 7.5f * s, RGB(150, 210, 120));                       // head
		disc(x + d * 24 * s, y - 4 * s + b, 1.8f * s, RGB(20, 24, 30));
		bar(x, y - 21 * s, 30 * s, e->hp / e->max, RGB(255, 200, 87));
		return;
	}
	bool buff = e->type == C_BLUEBUFF || e->type == C_REDBUFF, big = e->type == C_LORD;
	u32 skin = RGB(190, 126, 64), deep = RGB(122, 76, 40);
	int gr = 0, gg = 0, gb = 0;
	if (e->type == C_BLUEBUFF) { skin = RGB(110, 170, 255); deep = RGB(48, 96, 190); gr = 80; gg = 160; gb = 255; }
	else if (e->type == C_REDBUFF) { skin = RGB(255, 120, 96); deep = RGB(170, 50, 44); gr = 255; gg = 100; gb = 80; }
	else if (big) { skin = RGB(150, 88, 214); deep = RGB(92, 48, 150); gr = 190; gg = 130; gb = 255; }
	if (buff) glow(x, y, 22, gr, gg, gb);
	if (big) glow(x, y, 32, gr, gg, gb);
	tri(x - 9 * k, y - 4 * k + b, x - 13 * k, y - 17 * k + b, x - 2 * k, y - 8 * k + b, RGB(240, 226, 196));  // horns
	tri(x + 9 * k, y - 4 * k + b, x + 13 * k, y - 17 * k + b, x + 2 * k, y - 8 * k + b, RGB(240, 226, 196));
	if (big) {                                                   // the Lord wears a little crown
		for (int i = -1; i <= 1; i++) tri(x + i * 5 * k - 2 * k, y - 11 * k + b, x + i * 5 * k, y - 17 * k + b, x + i * 5 * k + 2 * k, y - 11 * k + b, RGB(255, 214, 100));
	}
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
// ---------------------------------------------------------------- anime heroes (original designs)
// Every hero is drawn in "local" space: the origin is the hero's feet, +x is the way he faces, up is negative y.
// The figure is about 53 units tall (head on top). All animation comes from the hero's own timers:
// attack swing, dash, burst and ultimate cooldowns.
typedef struct { u32 skin, hair, hair2, eye, main, main2, trim, glow; } Look;
static const Look LOOK[NHEROES] = {
	// Blue Knight: azure hair, white and blue royal armour, gold trim
	{ RGB(255, 229, 208), RGB(66, 132, 238),  RGB(196, 228, 255), RGB(70, 215, 255),  RGB(52, 112, 226), RGB(22, 54, 150),  RGB(255, 206, 96),  RGB(120, 225, 255) },
	// Lyra: mint-silver hair, emerald and white dress with feathers
	{ RGB(255, 232, 214), RGB(170, 232, 204), RGB(246, 255, 238), RGB(60, 235, 160),  RGB(34, 166, 112), RGB(16, 90, 86),   RGB(255, 222, 130), RGB(150, 255, 200) },
	// Grom: flame-red mane, crimson and steel plate
	{ RGB(238, 176, 134), RGB(226, 56, 40),   RGB(255, 150, 66),  RGB(255, 196, 60),  RGB(160, 46, 38),  RGB(78, 28, 36),   RGB(255, 206, 96),  RGB(255, 140, 60)  },
	// Vex: violet hair, shadow suit with pink light
	{ RGB(242, 216, 204), RGB(96, 52, 198),   RGB(190, 142, 255), RGB(255, 96, 200),  RGB(60, 38, 126),  RGB(22, 16, 52),   RGB(255, 110, 200), RGB(206, 124, 255) },
};

typedef struct {
	float step, bob, lean, hover;      // walking, breathing, leaning forward, floating
	float ap, ds, bs, us;              // how far each animation has gone
	bool moving, atk, dash, burst, ult, blink;
	int team;
} Pose;

static u32 dim(u32 c, float f) { return RGB((int)((c & 255) * f), (int)(((c >> 8) & 255) * f), (int)(((c >> 16) & 255) * f)); }
static u32 mixc(u32 a, u32 b, float t) {
	return RGB((int)((a & 255) * (1 - t) + (b & 255) * t), (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t),
	           (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t));
}
static u32 alpha(u32 c, int a) { return (c & 0x00FFFFFFu) | ((u32)a << 24); }

static void arcLine(float cx, float cy, float r, float a0, float a1, float w, u32 c) {
	int n = (int)(fabsf(a1 - a0) * 4) + 4;                  // more segments for longer arcs
	for (int i = 0; i < n; i++) {
		float p = a0 + (a1 - a0) * i / n, q = a0 + (a1 - a0) * (i + 1) / n;
		thick(cx + cosf(p) * r, cy + sinf(p) * r, cx + cosf(q) * r, cy + sinf(q) * r, w, c);
	}
}

// a four-sided piece of clothing or armour with a darker side, so it looks round instead of flat
static void panel(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u32 c, u32 shade) {
	quad(x1, y1, x2, y2, x3, y3, x4, y4, c);
	float ax = x1 + (x2 - x1) * 0.6f, ay = y1 + (y2 - y1) * 0.6f, dx = x4 + (x3 - x4) * 0.6f, dy = y4 + (y3 - y4) * 0.6f;
	quad(ax, ay, x2, y2, x3, y3, dx, dy, alpha(shade, 120));
}

// one big shiny anime eye, with lashes
static void eyeAt(float x, float y, float w, float h, u32 iris, bool blink) {
	u32 dark = RGB(34, 22, 56), white = RGB(255, 255, 255);
	if (blink) { thick(x - w / 2 - 0.2f, y + h * 0.2f, x + w / 2 + 0.5f, y + h * 0.2f, 1.0f, dark); return; }
	oval(x, y, w + 0.9f, h + 0.9f, dark);
	oval(x, y, w, h, mixc(iris, white, 0.12f));
	oval(x, y - h * 0.22f, w * 0.92f, h * 0.55f, dim(iris, 0.5f));
	oval(x, y + h * 0.22f, w * 0.7f, h * 0.38f, mixc(iris, white, 0.55f));
	oval(x + 0.1f, y, w * 0.38f, h * 0.5f, dark);
	disc(x - w * 0.22f, y - h * 0.24f, 0.8f, white);
	disc(x + w * 0.2f, y + h * 0.26f, 0.4f, white);
	thick(x - w / 2 - 0.5f, y - h / 2 + 0.3f, x + w / 2 + 0.7f, y - h / 2 - 0.5f, 1.0f, dark);   // upper lash
}

static void faceBase(float cx, float cy, u32 skin) {
	disc(cx, cy, 6.7f, skin);
	tri(cx - 5.2f, cy + 2.5f, cx + 5.2f, cy + 2.5f, cx + 1.2f, cy + 8.4f, skin);                     // pointed chin
	tri(cx - 6.6f, cy - 1, cx - 3.8f, cy - 3, cx - 4, cy + 6, RGBA(60, 20, 40, 40));                    // soft shadow
}

static void faceDetails(float cx, float cy, const Pose *p, const Look *L, bool fierce) {
	u32 brow = dim(L->hair, 0.55f);
	eyeAt(cx - 1.0f, cy + 0.9f, 2.7f, 3.8f, L->eye, p->blink);        // far eye
	eyeAt(cx + 3.7f, cy + 0.9f, 3.1f, 4.2f, L->eye, p->blink);        // near eye
	if (fierce) {
		thick(cx - 2.8f, cy - 3.0f, cx + 0.6f, cy - 2.0f, 0.8f, brow);
		thick(cx + 2.4f, cy - 2.0f, cx + 6.0f, cy - 3.3f, 0.8f, brow);
	} else {
		thick(cx - 2.8f, cy - 2.6f, cx + 0.6f, cy - 2.9f, 0.7f, brow);
		thick(cx + 2.4f, cy - 2.9f, cx + 6.0f, cy - 2.6f, 0.7f, brow);
	}
	thick(cx + 5.0f, cy + 3.3f, cx + 5.7f, cy + 3.9f, 0.5f, dim(L->skin, 0.78f));                      // nose
	if (p->atk || p->ult || p->burst) oval(cx + 2.7f, cy + 5.4f, 2.2f, 2.0f, RGB(190, 60, 76));          // shouting
	else thick(cx + 1.6f, cy + 5.4f, cx + 3.6f, cy + 5.2f, 0.7f, RGB(200, 96, 104));
	oval(cx + 4.4f, cy + 4.5f, 2.6f, 1.2f, RGBA(255, 110, 140, 90));                                    // blush
}

static void neck(float cx, float sh, float hy, u32 skin) { thick(cx + 0.4f, sh + 0.5f, cx + 0.6f, hy + 3, 2.6f, dim(skin, 0.92f)); }

// two legs with knees, a boot each and a cuff
static void legPair(const Pose *p, float hip, u32 col, u32 boot, u32 cuff, float w) {
	for (int s = 0; s < 2; s++) {
		float sg = s ? 1.0f : -1.0f;
		float hx = p->lean * 0.3f + sg * 2.4f, fx = hx + sg * 1.2f - sg * p->step, fy = p->hover;
		float kx = (hx + fx) / 2 + 1.6f, ky = (hip + fy) / 2;
		thick(hx, hip, kx, ky, w, col);
		thick(kx, ky, fx, fy - 1.5f, w * 0.88f, col);
		thick(kx + (fx - kx) * 0.35f, ky + (fy - 1.5f - ky) * 0.35f, fx, fy - 1.5f, w * 1.05f, boot);
		thick(kx - w * 0.6f, ky, kx + w * 0.6f, ky, 0.9f, cuff);
		oval(fx + 1.4f, fy - 1.0f, 6.4f, 2.9f, boot);
	}
}

static void armTo(float sx, float sy, float hx, float hy, float w, u32 col, u32 hand) {
	float ex = (sx + hx) / 2, ey = (sy + hy) / 2 + 2;
	thick(sx, sy, ex, ey, w, col);
	thick(ex, ey, hx, hy, w * 0.9f, col);
	disc(hx, hy, w * 0.62f, hand);
}

static void swordAt(float x, float y, float a, float len, u32 col, u32 gold, u32 gl) {
	float c = cosf(a), s = sinf(a), tx = x + c * len, ty = y + s * len;
	thick(x - c * 4.5f, y - s * 4.5f, x, y, 1.8f, dim(gold, 0.7f));                                  // grip
	disc(x - c * 5, y - s * 5, 1.5f, gold);                                                           // pommel
	thick(x, y, tx, ty, 6.0f, alpha(gl, 60));                                                         // glow
	thick(x, y, tx, ty, 3.6f, col);
	thick(x, y, tx, ty, 1.2f, RGB(255, 255, 255));
	tri(tx - s * 1.8f, ty + c * 1.8f, tx + s * 1.8f, ty - c * 1.8f, tx + c * 5, ty + s * 5, col);
	thick(x - s * 4, y + c * 4, x + s * 4, y - c * 4, 2.0f, gold);                                    // guard
}

// ---- Blue Knight: azure hair, white and blue royal armour, a long cape and a glowing greatsword ----
static void heroKnight(const Pose *p, const Look *L) {
	float hip = -20 + p->bob + p->hover, sh = hip - 15, cx = p->lean, hy = hip - 24.5f, mid = hip - 11;
	u32 white = RGB(238, 244, 255), steel = RGB(184, 200, 230), dark = RGB(26, 34, 70), gold = L->trim;
	float wave = sinf(frame * 0.13f);
	float fl = 2 + wave * 2 + (p->moving ? 3 : 0) + (p->dash ? 10 : 0);
	if (p->ult) {                                                              // wings of light
		glow(cx, mid, 36, 255, 226, 140);
		tri(cx - 2, sh + 2, cx - 28 - wave * 2, sh - 24, cx - 8, sh + 12, RGBA(255, 236, 160, 200));
		tri(cx - 2, sh + 2, cx - 34 - wave * 2, sh - 8, cx - 8, sh + 14, RGBA(255, 244, 190, 180));
		tri(cx - 2, sh + 4, cx - 30 - wave * 2, sh + 8, cx - 8, sh + 16, RGBA(255, 226, 150, 160));
	}
	panel(cx - 2, sh, cx + 3, sh + 1, cx - 5 - fl * 0.5f, hip + 15, cx - 16 - fl, hip + 13 + wave, L->main2, dim(L->main2, 0.4f));   // cape
	thick(cx - 16 - fl, hip + 13 + wave, cx - 5 - fl * 0.5f, hip + 15, 1.1f, gold);
	armTo(cx - 5, sh + 1.5f, cx - 7.5f, hip - 4 + p->step * 0.2f, 2.8f, L->main2, steel);                     // back arm
	legPair(p, hip, dark, white, gold, 3.4f);
	panel(cx - 6, hip - 2, cx + 1, hip - 2, cx + 0.5f, hip + 8, cx - 8 - p->step * 0.3f, hip + 9, L->main, L->main2);     // tassets
	panel(cx, hip - 2, cx + 6.2f, hip - 2, cx + 8 + p->step * 0.3f, hip + 9, cx + 0.5f, hip + 8, white, steel);
	thick(cx - 8 - p->step * 0.3f, hip + 9, cx + 0.5f, hip + 8, 1.0f, gold);
	thick(cx + 0.5f, hip + 8, cx + 8 + p->step * 0.3f, hip + 9, 1.0f, gold);
	panel(cx - 5.6f, sh, cx + 5.6f, sh, cx + 4.2f, hip, cx - 4.2f, hip, white, steel);                        // breastplate
	tri(cx - 4.5f, sh + 0.5f, cx + 4.5f, sh + 0.5f, cx, hip - 2, L->main);
	tri(cx + 1, sh + 0.5f, cx + 4.5f, sh + 0.5f, cx + 0.5f, hip - 2, alpha(L->main2, 130));
	thick(cx - 5, sh + 0.6f, cx + 5, sh + 0.6f, 1.3f, gold);
	fillr(cx - 4.4f, hip - 3, 8.8f, 2.4f, dark);
	disc(cx, hip - 1.8f, 1.6f, gold);
	glow(cx, sh + 5, 4.5f, 120, 225, 255);
	disc(cx, sh + 5, 1.5f, L->glow);
	disc(cx - 0.3f, sh + 4.6f, 0.6f, white);
	disc(cx + 3.3f, sh + 9, 1.1f, TEAM_COL[p->team]);
	neck(cx, sh, hy, L->skin);
	for (int s = -1; s <= 1; s += 2) {                                                                        // pauldrons
		float px = cx + s * 6.4f, py = sh + 0.8f;
		oval(px, py, 8.6f, 6.4f, gold);
		oval(px, py - 0.3f, 7.0f, 5.0f, s < 0 ? L->main : white);
		tri(px + s * 2, py - 2.6f, px + s * 5.8f, py - 7, px + s * 0.4f, py - 3.2f, gold);
	}
	// sword arm
	float sx = cx + 9, sy = hip - 8, a = -0.95f, len = 22;
	if (p->atk)   a = -2.3f + p->ap * 3.0f;
	if (p->dash)  { a = -0.1f; len = 28; }
	if (p->burst) a = frame * 0.9f;
	if (p->ult)   { a = -1.5708f; len = 34; }
	if (p->atk) {                                                              // crescent slash
		arcLine(sx, sy, len + 1, -2.3f, a, 4.4f, RGBA(200, 240, 255, 150));
		arcLine(sx, sy, len - 4, -2.3f, a - 0.2f, 1.8f, RGBA(255, 255, 255, 190));
	}
	if (p->burst) {
		arcLine(cx, mid, 21, 0, 6.28f, 2.4f, RGBA(160, 230, 255, 170));
		arcLine(cx, mid, 14, frame * 0.5f, frame * 0.5f + 3.6f, 1.8f, RGBA(255, 255, 255, 200));
	}
	armTo(cx + 5.4f, sh + 1.5f, sx, sy, 3.0f, L->main2, steel);
	swordAt(sx, sy, a, len, RGB(205, 240, 255), gold, L->glow);
	if (p->atk || p->ult) sparkle(sx + cosf(a) * len, sy + sinf(a) * len, p->ult ? 8 : 5, RGB(255, 255, 255));
	if (p->dash)
		for (int i = 0; i < 4; i++) thick(-9 - i * 4, mid - 8 + i * 5, -26 - i * 6, mid - 8 + i * 5, 1.5f, RGBA(200, 235, 255, 150));
	if (p->ult)
		for (int i = 0; i < 4; i++)
			sparkle(cx + cosf(frame * 0.1f + i * 1.57f) * 20, mid - 8 + sinf(frame * 0.1f + i * 1.57f) * 12, 3, RGB(255, 245, 200));
	// head
	float sw = wave * 1.5f + (p->moving ? 2 : 0) + (p->dash ? 5 : 0) + (p->ult ? 4 : 0);
	tri(cx - 4, hy - 5, cx - 18 - sw, hy - 8, cx - 4, hy + 3, L->hair);
	tri(cx - 4, hy - 1, cx - 19 - sw, hy + 3, cx - 3, hy + 7, L->hair2);
	tri(cx - 3, hy - 7, cx - 11 - sw * 0.6f, hy - 15, cx + 1, hy - 7, L->hair);
	tri(cx - 4, hy + 2, cx - 13 - sw * 0.8f, hy + 14 + wave, cx - 2, hy + 8, dim(L->hair, 0.8f));
	faceBase(cx, hy, L->skin);
	oval(cx - 0.4f, hy - 3.6f, 15.6f, 9.4f, L->hair);
	tri(cx - 7, hy - 2.5f, cx - 2.5f, hy - 2.5f, cx - 4.6f, hy + 3.2f, L->hair);
	tri(cx - 3, hy - 2.5f, cx + 2, hy - 2.5f, cx - 0.4f, hy + 3.8f, L->hair2);
	tri(cx + 1.5f, hy - 2.5f, cx + 7, hy - 3.5f, cx + 5.2f, hy + 2.4f, L->hair);
	tri(cx - 2.5f, hy - 8, cx + 2.5f, hy - 8, cx + 1, hy - 17 - sw * 0.3f, L->hair);
	thick(cx - 4, hy - 7.5f, cx + 1, hy - 8.5f, 1.0f, RGBA(255, 255, 255, 150));
	thick(cx - 6.6f, hy - 3.2f, cx + 6.8f, hy - 3.2f, 1.1f, gold);                                            // circlet
	quad(cx + 1.2f, hy - 5.0f, cx + 2.4f, hy - 3.2f, cx + 1.2f, hy - 1.4f, cx, hy - 3.2f, L->glow);
	faceDetails(cx, hy, p, L, p->atk || p->dash);
}

// ---- Lyra: a graceful archer with a mint ponytail, a feathered emerald dress and a wind bow ----
static void heroLyra(const Pose *p, const Look *L) {
	float hip = -20 + p->bob + p->hover, sh = hip - 15, cx = p->lean, hy = hip - 24.5f, mid = hip - 11;
	float wave = sinf(frame * 0.11f);
	float flow = 3 + wave * 2.5f + (p->moving ? 6 : 0) + (p->dash ? 10 : 0) + (p->ult ? 6 : 0);
	u32 white = RGB(246, 252, 255), gold = L->trim;
	if (p->ult) glow(cx, mid, 36, 150, 255, 190);
	for (int i = 0; i < 3; i++)                                                                               // quiver arrows
		tri(cx - 5 - i * 1.6f, sh - 1 - i * 3, cx - 3.5f - i * 1.6f, sh - 1 - i * 3, cx - 4.2f - i * 1.6f, sh - 6 - i * 3, L->glow);
	thick(cx - 4, sh + 2, cx - 7, hip - 2, 3.4f, RGB(120, 84, 60));
	float px = cx - 3, py = hip - 3;                                                                          // flowing sash
	for (int i = 0; i < 5; i++) {
		float nx = px - 3.4f - flow * 0.15f, ny = py + 2.0f + sinf(frame * 0.17f + i) * 1.6f;
		thick(px, py, nx, ny, 3.2f - i * 0.4f, i % 2 ? L->main : white);
		disc(nx, ny, (3.2f - i * 0.4f) / 2, i % 2 ? L->main : white);
		px = nx; py = ny;
	}
	legPair(p, hip, white, L->main2, gold, 3.0f);
	quad(cx - 3.5f, hip - 1, cx + 3.5f, hip - 1, cx + 9 + p->step * 0.3f, hip + 9, cx - 9 - p->step * 0.3f, hip + 9, L->main);        // skirt
	quad(cx - 3.5f, hip - 1, cx + 3.5f, hip - 1, cx + 7 + p->step * 0.3f, hip + 6, cx - 7 - p->step * 0.3f, hip + 6, white);
	tri(cx + 1, hip - 1, cx + 3.5f, hip - 1, cx + 9 + p->step * 0.3f, hip + 9, alpha(L->main2, 110));
	thick(cx - 9 - p->step * 0.3f, hip + 9, cx + 9 + p->step * 0.3f, hip + 9, 1.1f, gold);
	panel(cx - 4.4f, sh, cx + 4.4f, sh, cx + 3, hip, cx - 3, hip, white, RGB(180, 210, 214));                // bodice
	tri(cx - 3.2f, sh + 1, cx + 3.2f, sh + 1, cx, hip - 3, L->main);
	thick(cx - 4, sh + 0.6f, cx + 4, sh + 0.6f, 1.1f, gold);
	fillr(cx - 3.2f, hip - 2.6f, 6.4f, 2, gold);
	glow(cx, sh + 4, 3.5f, 150, 255, 200);
	disc(cx, sh + 4, 1.2f, L->glow);
	disc(cx + 2.6f, sh + 8, 1.0f, TEAM_COL[p->team]);
	neck(cx, sh, hy, L->skin);
	for (int i = 0; i < 3; i++)                                                                               // feather shoulder piece
		tri(cx + 3, sh + 0.5f, cx + 5 + i * 2, sh - 6 + i * 2.2f, cx + 7.5f + i * 1.2f, sh + 1 + i, i == 1 ? white : gold);
	// the bow
	float bcx = cx + 3, bcy = hip - 9, r = 14.5f;
	if (p->ult) { r = 25; bcx = cx + 5; bcy = hip - 12; }
	float pull = p->atk && p->ap < 0.5f ? p->ap * 16 : (p->ult ? 10 : 0);
	arcLine(bcx, bcy, r, -1.1f, 1.1f, p->ult ? 3.6f : 2.6f, p->ult ? RGB(190, 255, 215) : gold);
	arcLine(bcx, bcy, r - 1.2f, -1.0f, 1.0f, 1.0f, RGB(255, 255, 255));
	float ex = bcx + r * cosf(1.1f), ey = r * sinf(1.1f);
	thick(ex, bcy - ey, ex - pull, bcy, 0.9f, white);
	thick(ex, bcy + ey, ex - pull, bcy, 0.9f, white);
	bool flying = p->atk && p->ap >= 0.5f;
	if (!flying) {                                                                   // arrow ready on the string
		float ax = ex - pull;
		thick(ax, bcy, ax + 19, bcy, 1.4f, L->glow);
		tri(ax + 19, bcy - 2.4f, ax + 19, bcy + 2.4f, ax + 24, bcy, white);
		tri(ax, bcy, ax + 3, bcy - 2.5f, ax + 3, bcy, gold);
		if (p->ult) { glow(ax + 22, bcy, 12, 170, 255, 200); thick(ax, bcy, ax + 40, bcy, 3.0f, RGBA(190, 255, 215, 200)); }
	} else {                                                                         // arrow released
		float ox = bcx + 22 + (p->ap - 0.5f) * 2 * 50;
		thick(ox - 24, bcy, ox, bcy, 3.4f, RGBA(150, 255, 190, 90));
		thick(ox - 15, bcy, ox, bcy, 1.5f, white);
		tri(ox, bcy - 2.8f, ox, bcy + 2.8f, ox + 6, bcy, L->glow);
		sparkle(ox + 3, bcy, 5, white);
	}
	armTo(cx + 4, sh + 1.5f, bcx + r - 1.5f, bcy, 2.6f, white, L->skin);                                      // bow arm
	armTo(cx + 3.5f, sh + 1.5f, ex - pull, bcy, 2.4f, white, L->skin);                                         // pulling arm
	thick(cx + 5, sh + 4, cx + 9, bcy - 1, 1.2f, gold);
	if (p->dash) for (int i = 0; i < 4; i++) {                                                                // wind and leaves
		thick(-7 - i * 4, mid - 8 + i * 5, -24 - i * 6, mid - 8 + i * 5, 1.4f, RGBA(170, 255, 200, 150));
		tri(-16 - i * 7, mid - 10 + i * 5, -12 - i * 7, mid - 12 + i * 5, -14 - i * 7, mid - 5 + i * 5, L->glow);
	}
	if (p->burst) for (int i = 0; i < 8; i++) {                                                               // swirling leaves
		float an = frame * 0.35f + i * 0.785f;
		float lxp = cx + cosf(an) * 21, lyp = mid + sinf(an) * 10;
		tri(lxp - 2.8f, lyp, lxp + 2.8f, lyp, lxp, lyp - 5.5f, L->glow);
	}
	if (p->ult) for (int i = 0; i < 4; i++)
		sparkle(cx + cosf(frame * 0.12f + i * 1.57f) * 22, mid - 6 + sinf(frame * 0.12f + i * 1.57f) * 13, 3, RGB(220, 255, 230));
	// head: a long high ponytail, side locks, feather hair ornament
	float tx = cx - 5.5f, ty = hy - 6.5f;
	for (int i = 0; i < 7; i++) {
		float nx = tx - 3.8f - flow * 0.16f, ny = ty + 2.6f + sinf(frame * 0.14f + i * 0.8f) * 1.5f;
		u32 sc = i == 6 ? L->main : (i % 2 ? L->hair2 : L->hair);
		thick(tx, ty, nx, ny, 5.4f - i * 0.65f, sc);
		disc(nx, ny, (5.4f - i * 0.65f) / 2, sc);
		tx = nx; ty = ny;
	}
	disc(cx - 5.5f, hy - 6.5f, 2.2f, gold);
	faceBase(cx, hy, L->skin);
	oval(cx - 0.4f, hy - 3.6f, 15.6f, 9.4f, L->hair);
	tri(cx - 6.8f, hy - 1, cx - 4.5f, hy - 1, cx - 6.6f, hy + 11, L->hair);
	tri(cx + 5.2f, hy - 2, cx + 7, hy, cx + 6.2f, hy + 8.5f, L->hair2);
	tri(cx - 4, hy - 2.5f, cx + 1, hy - 2.5f, cx - 3, hy + 3.5f, L->hair2);
	tri(cx + 1, hy - 2.5f, cx + 6, hy - 3, cx + 4, hy + 2.6f, L->hair);
	thick(cx - 3, hy - 7.6f, cx + 2, hy - 8.3f, 1.0f, RGBA(255, 255, 255, 170));
	for (int i = 0; i < 3; i++)                                                                               // golden feather ornament
		tri(cx + 2, hy - 7.5f, cx + 4 + i * 3, hy - 14 + i * 2.5f, cx + 7 + i * 1.6f, hy - 7 + i, i == 1 ? white : gold);
	faceDetails(cx, hy, p, L, p->dash);
}

// ---- Grom: a huge fire-maned bruiser with gold-trimmed plate and a rune hammer ----
static void hammerAt(float x, float y, float a, float len, u32 gl) {
	float c = cosf(a), s = sinf(a), tx = x + c * len, ty = y + s * len;
	u32 wood = RGB(130, 88, 56), steel = RGB(196, 204, 224);
	thick(x - c * 4, y - s * 4, tx, ty, 3.0f, wood);
	float nx = -s, ny = c;                                                                     // across the head
	quad(tx + nx * 8 - c * 5.5f, ty + ny * 8 - s * 5.5f, tx - nx * 8 - c * 5.5f, ty - ny * 8 - s * 5.5f,
	     tx - nx * 8 + c * 5.5f, ty - ny * 8 + s * 5.5f, tx + nx * 8 + c * 5.5f, ty + ny * 8 + s * 5.5f, dim(steel, 0.62f));
	quad(tx + nx * 6.6f - c * 4, ty + ny * 6.6f - s * 4, tx - nx * 6.6f - c * 4, ty - ny * 6.6f - s * 4,
	     tx - nx * 6.6f + c * 4, ty - ny * 6.6f + s * 4, tx + nx * 6.6f + c * 4, ty + ny * 6.6f + s * 4, steel);
	glow(tx, ty, 6, (gl & 255), (gl >> 8) & 255, (gl >> 16) & 255);                            // glowing rune
	disc(tx, ty, 1.7f, gl);
	thick(x + c * 3, y + s * 3, x + c * 7, y + s * 7, 3.6f, RGB(255, 204, 92));
}

static void heroGrom(const Pose *p0, const Look *L) {
	Pose q = *p0;
	const Pose *p = &q;
	if (p0->burst) q.hover = -sinf(clampf(p0->bs / 0.5f, 0, 1) * 3.14159f) * 9;                // jumps for the stomp
	float hip = -20 + p->bob + p->hover, sh = hip - 15, cx = p->lean, hy = hip - 24.5f, mid = hip - 11;
	u32 skin = L->skin, dark = RGB(40, 30, 50), steel = RGB(196, 204, 224), gold = L->trim, fur = RGB(244, 228, 204);
	float wave = sinf(frame * 0.13f);
	if (p->ult) {
		glow(cx, mid, 40, 255, 110, 50);
		for (int i = 0; i < 6; i++) {                                                  // flames rising
			float up = fmodf(frame * 0.7f + i * 9, 40);
			tri(cx - 14 + i * 5.5f - 2.4f, mid - up, cx - 14 + i * 5.5f + 2.4f, mid - up, cx - 14 + i * 5.5f, mid - 11 - up, RGBA(255, 190 - (int)up * 3, 60, 200));
		}
	}
	if (p0->burst && p0->bs > 0.2f) {                                                  // cracks in the ground
		float pr = clampf((p0->bs - 0.2f) / 0.3f, 0, 1);
		for (int i = 0; i < 7; i++) {
			float an = -1.2f + i * 0.4f;
			thick(16, 0, 16 + cosf(an) * 30 * pr, sinf(an) * 10 * pr, 1.8f, dark);
		}
		disc(16, 0, 3 + pr * 3, RGBA(255, 214, 140, 150));
	}
	armTo(cx - 7.5f, sh + 2, cx - 10.5f, hip - 3, 4.8f, skin, skin);                                           // back arm
	thick(cx - 10.5f, hip - 6, cx - 10.5f, hip - 2, 5.6f, gold);
	legPair(p, hip, dark, steel, gold, 4.6f);
	panel(cx - 7, hip - 2, cx + 0.5f, hip - 2, cx - 0.5f, hip + 8, cx - 9.5f, hip + 7, L->main, L->main2);     // armoured kilt
	panel(cx, hip - 2, cx + 7.2f, hip - 2, cx + 9, hip + 7, cx + 0.5f, hip + 8, L->main, L->main2);
	thick(cx - 9.5f, hip + 7, cx + 0.5f, hip + 8, 1.2f, gold);
	thick(cx + 0.5f, hip + 8, cx + 9, hip + 7, 1.2f, gold);
	panel(cx - 8.2f, sh, cx + 8.2f, sh, cx + 6.2f, hip, cx - 6.2f, hip, skin, RGB(150, 80, 60));               // bare chest
	thick(cx - 6, sh + 2, cx + 6, hip - 1, 1.8f, dark);                                                       // harness
	thick(cx + 6, sh + 2, cx - 6, hip - 1, 1.8f, dark);
	disc(cx, mid - 1, 2.2f, gold);
	thick(cx - 3, sh + 8, cx + 3, sh + 8, 0.7f, RGBA(130, 60, 40, 150));
	fillr(cx - 6.4f, hip - 3.6f, 12.8f, 3.4f, dark);
	oval(cx, hip - 1.9f, 5, 4, gold);
	disc(cx, hip - 1.9f, 1.3f, TEAM_COL[p->team]);
	neck(cx, sh, hy, skin);
	for (int i = 0; i < 4; i++) oval(cx - 4.5f + i * 3, sh + 0.2f, 6.5f, 4.5f, fur);                         // fur collar
	// hammer
	float hx = cx + 10, hyy = hip - 8, a = -1.25f, len = 25;
	if (p->atk) a = -2.7f + p->ap * 3.3f;                                                                      // overhead slam
	if (p->dash) a = -2.95f;
	if (p->burst) a = 1.15f;
	if (p->ult) { a = -1.75f; len = 28; }
	armTo(cx + 7, sh + 2, hx, hyy, 4.8f, skin, skin);
	thick(hx, hyy - 3.2f, hx, hyy + 2, 5.4f, gold);                                                            // bracer
	hammerAt(hx, hyy, a, len, L->glow);
	for (int s = -1; s <= 1; s += 2) {                                                                         // giant pauldrons
		float px = cx + s * 8.6f, py = sh + 1.4f;
		tri(px + s * 3, py - 3.6f, px + s * 7.5f, py - 11, px + s * 0.4f, py - 4.4f, steel);                   // spike
		oval(px, py, 11.5f, 8.6f, s < 0 ? L->main2 : L->main);                                                  // dome
		oval(px + s * 1.4f, py + 1.4f, 9.4f, 5.6f, s < 0 ? dim(L->main2, 0.75f) : L->main2);                    // lower plate
		arcLine(px, py + 0.4f, 5.2f, 3.4f, 5.9f, 1.4f, steel);                                                  // shiny rim on top
		thick(px - 5, py + 3.4f, px + 5, py + 3.4f, 1.3f, gold);
		disc(px, py - 0.4f, 1.3f, gold);
	}
	float tipx = hx + cosf(a) * len, tipy = hyy + sinf(a) * len;
	if (p->atk && p->ap > 0.6f) {                                                                              // impact
		sparkle(tipx + 4, tipy, 10, RGB(255, 240, 180));
		oval(tipx + 2, tipy + 7, 22, 6, RGBA(200, 190, 170, 140));
	}
	if (p->ult) sparkle(tipx, tipy - 5, 8, RGB(255, 190, 90));
	if (p->dash) for (int i = 0; i < 4; i++) disc(-12 - i * 7, -3 - (i % 2) * 3, 3.8f - i * 0.6f, RGBA(205, 190, 165, 150));   // dust
	// head
	float sw = wave * 1.8f + (p->moving ? 3 : 0) + (p->dash ? 6 : 0) + (p->ult ? 5 : 0);
	tri(cx - 5, hy - 6, cx - 21 - sw, hy - 10, cx - 4, hy + 1, L->hair);
	tri(cx - 5, hy - 1, cx - 23 - sw, hy + 2, cx - 4, hy + 6, L->hair2);
	tri(cx - 4, hy + 2, cx - 17 - sw, hy + 13 + wave, cx - 3, hy + 8, dim(L->hair, 0.8f));
	tri(cx - 3, hy - 7, cx - 12 - sw * 0.6f, hy - 18, cx + 1, hy - 7, L->hair);
	faceBase(cx, hy, skin);
	oval(cx - 0.4f, hy - 3.8f, 15.8f, 9.8f, L->hair);
	tri(cx - 7, hy - 2.5f, cx - 2.5f, hy - 2.5f, cx - 5, hy + 3, L->hair);
	tri(cx - 2.5f, hy - 2.5f, cx + 2.5f, hy - 2.5f, cx, hy + 3.8f, L->hair2);
	tri(cx + 2, hy - 2.5f, cx + 7, hy - 3.8f, cx + 5.6f, hy + 2, L->hair);
	tri(cx - 3.5f, hy - 8, cx - 0.5f, hy - 8, cx - 2.5f, hy - 19 - sw * 0.3f, L->hair);
	tri(cx + 0, hy - 8, cx + 3.5f, hy - 8, cx + 2.6f, hy - 17 - sw * 0.3f, L->hair2);
	thick(cx + 3.6f, hy - 0.4f, cx + 5.7f, hy + 3.6f, 0.8f, RGB(190, 70, 60));                                 // scar
	thick(cx - 6.8f, hy - 3, cx + 6.8f, hy - 3, 2.0f, TEAM_COL[p->team]);                                      // headband
	tri(cx - 6.5f, hy - 3.6f, cx - 6.5f, hy - 1.6f, cx - 17 - sw, hy - 1 + wave, TEAM_COL[p->team]);
	faceDetails(cx, hy, p, L, true);
}

// ---- Vex: a slim shadow assassin with a long ponytail, glowing eyes and twin daggers ----
static void daggerAt(float x, float y, float a, float len, u32 col, u32 gl) {
	float c = cosf(a), s = sinf(a), tx = x + c * len, ty = y + s * len;
	thick(x, y, tx, ty, 4.8f, alpha(gl, 70));
	thick(x, y, tx, ty, 2.5f, col);
	thick(x, y, tx, ty, 0.9f, RGB(255, 255, 255));
	tri(tx - s * 1.3f, ty + c * 1.3f, tx + s * 1.3f, ty - c * 1.3f, tx + c * 4, ty + s * 4, col);
	disc(x, y, 1.8f, RGB(255, 110, 200));
}

static void heroVex(const Pose *p, const Look *L) {
	float hip = -20 + p->bob + p->hover, sh = hip - 15, cx = p->lean, hy = hip - 24.5f, mid = hip - 11;
	u32 dark = L->main2, steel = RGB(226, 236, 250), pink = L->trim;
	float wave = sinf(frame * 0.15f);
	float flow = 3 + (p->moving ? 7 : 0) + (p->dash ? 14 : 0) + (p->ult ? 6 : 0);
	if (p->dash) for (int i = 1; i <= 3; i++) {                                                               // after-images
		float gx = -i * 10;
		disc(gx, hy, 6.8f, RGBA(150, 100, 255, 110 - i * 25));
		quad(gx - 4.5f, sh, gx + 4.5f, sh, gx + 3.5f, hip + 2, gx - 3.5f, hip + 2, RGBA(150, 100, 255, 100 - i * 22));
	}
	if (p->ult) {                                                                                              // shadow clones
		glow(cx, mid, 32, 190, 100, 255);
		for (int s = -1; s <= 1; s += 2) {
			float ccx = cx + s * 16, ccy = hip - 2 * s;
			disc(ccx, ccy - 24.5f, 6.5f, RGBA(150, 90, 255, 120));
			quad(ccx - 4.5f, ccy - 15, ccx + 4.5f, ccy - 15, ccx + 3.5f, ccy + 2, ccx - 3.5f, ccy + 2, RGBA(150, 90, 255, 110));
			daggerAt(ccx + 5, ccy - 8, -0.4f - s * 0.6f, 13, RGBA(200, 170, 255, 200), L->glow);
		}
	}
	float px = cx - 3, py = sh - 1;                                                                           // long scarf
	for (int i = 0; i < 7; i++) {
		float nx = px - 4.8f - flow * 0.18f, ny = py + 1.4f + sinf(frame * 0.18f + i * 0.9f) * 2.0f;
		u32 sc = i % 2 ? pink : dim(pink, 0.8f);
		thick(px, py, nx, ny, 4.0f - i * 0.4f, sc);
		disc(nx, ny, (4.0f - i * 0.4f) / 2, sc);
		px = nx; py = ny;
	}
	tri(cx - 3, hip, cx - 9 - p->step * 0.3f - flow * 0.2f, hip + 11, cx + 1, hip + 4, L->main2);               // coat tail
	tri(cx - 3, hip, cx - 6 - p->step * 0.3f - flow * 0.2f, hip + 8, cx + 1, hip + 3, L->main);
	legPair(p, hip, dark, L->main, pink, 3.2f);
	panel(cx - 5, sh, cx + 5, sh, cx + 3.8f, hip, cx - 3.8f, hip, dark, RGB(8, 6, 24));                       // slim suit
	thick(cx - 4.6f, sh + 1, cx + 3, hip - 1, 1.2f, pink);
	thick(cx + 4.6f, sh + 1, cx - 3, hip - 1, 1.2f, pink);
	fillr(cx - 4, hip - 2.6f, 8, 2.4f, L->main);
	disc(cx, hip - 1.4f, 1.4f, TEAM_COL[p->team]);
	glow(cx, mid - 2, 3.5f, 255, 110, 200);
	disc(cx, mid - 2, 1.2f, pink);
	neck(cx, sh, hy, L->skin);
	quad(cx - 3.4f, sh - 3.4f, cx + 3.6f, sh - 3.4f, cx + 4.4f, sh + 1.4f, cx - 4.4f, sh + 1.4f, L->main);   // high collar
	thick(cx - 4.4f, sh + 1.4f, cx + 4.4f, sh + 1.4f, 1.0f, pink);
	for (int s = -1; s <= 1; s += 2) {                                                                        // shoulder shards
		float spx = cx + s * 5.6f;
		tri(spx - 2.4f, sh + 0.8f, spx + 2.4f, sh + 0.8f, spx + s * 2.0f, sh - 5.5f, L->main);
		tri(spx - 1.2f, sh + 0.8f, spx + 1.2f, sh + 0.8f, spx + s * 4.6f, sh - 3, pink);
	}
	// daggers
	float fx = cx + 8, fy = hip - 9, bx = cx - 6, byy = hip - 6;
	float aF = -1.2f, aB = -2.0f, len = 13;
	if (p->atk) {                                                                                              // crossing slashes
		aF = -2.3f + p->ap * 2.9f;
		aB = 0.8f - p->ap * 3.2f;
		arcLine(fx + 1, fy, 17, -2.3f, aF, 3.4f, RGBA(255, 160, 230, 170));
		arcLine(fx + 1, fy, 15, 0.8f, aB, 2.6f, RGBA(220, 190, 255, 170));
	}
	if (p->dash)  { aF = 0; aB = -0.2f; len = 16; }
	if (p->burst) {
		aF = frame * 0.9f; aB = aF + 3.14f;
		for (int i = 0; i < 3; i++)
			arcLine(cx, mid, 20, frame * 0.5f + i * 2.09f, frame * 0.5f + i * 2.09f + 1.3f, 2.6f, RGBA(255, 150, 225, 190));
	}
	if (p->ult) { aF = -0.3f + wave * 0.5f; aB = -0.9f - wave * 0.5f; len = 15; }
	armTo(cx - 4.4f, sh + 1.8f, bx, byy, 2.6f, dark, L->skin);
	daggerAt(bx, byy, aB, len, steel, L->glow);
	armTo(cx + 4.4f, sh + 1.8f, fx, fy, 2.6f, dark, L->skin);
	daggerAt(fx, fy, aF, len, steel, L->glow);
	if (p->ult) {                                                                                              // big X cut
		thick(cx + 15, hip - 32, cx + 40, hip + 2, 2.8f, RGBA(255, 255, 255, 190));
		thick(cx + 40, hip - 32, cx + 15, hip + 2, 2.8f, RGBA(255, 150, 225, 190));
		sparkle(cx + 27, hip - 15, 8, RGB(255, 255, 255));
	}
	if (p->dash) for (int i = 0; i < 4; i++) thick(-7 - i * 5, mid - 8 + i * 5, -28 - i * 6, mid - 8 + i * 5, 1.4f, RGBA(210, 160, 255, 160));
	// head: sharp bangs, a long ponytail and glowing pink eyes
	float tx = cx - 5.5f, ty = hy - 5.5f;
	for (int i = 0; i < 7; i++) {
		float nx = tx - 4.6f - flow * 0.2f, ny = ty + 2.4f + sinf(frame * 0.16f + i * 0.8f) * 1.6f;
		u32 sc = i % 2 ? L->hair2 : L->hair;
		thick(tx, ty, nx, ny, 5.0f - i * 0.6f, sc);
		disc(nx, ny, (5.0f - i * 0.6f) / 2, sc);
		tx = nx; ty = ny;
	}
	faceBase(cx, hy, L->skin);
	oval(cx - 0.4f, hy - 3.6f, 15.6f, 9.4f, L->hair);
	tri(cx - 7, hy - 2.5f, cx - 2.5f, hy - 2.5f, cx - 6.2f, hy + 6.5f, L->hair);
	tri(cx - 3, hy - 2.5f, cx + 2, hy - 2.5f, cx - 0.6f, hy + 4.4f, L->hair2);
	tri(cx + 2, hy - 2.5f, cx + 7, hy - 3.5f, cx + 6.8f, hy + 5.4f, L->hair);
	tri(cx - 2, hy - 8, cx + 4, hy - 8, cx + 6, hy - 14 - wave, L->hair);
	thick(cx - 3, hy - 7.6f, cx + 2, hy - 8.3f, 1.0f, RGBA(255, 255, 255, 140));
	faceDetails(cx, hy, p, L, true);
	glow(cx + 3.7f, hy + 0.9f, 4.5f, 255, 100, 200);
	quad(cx - 5.6f, hy + 3.4f, cx + 6.6f, hy + 3.4f, cx + 5, hy + 7.2f, cx - 4.6f, hy + 7.2f, dim(L->main, 0.55f));   // half mask
	thick(cx - 5.6f, hy + 3.4f, cx + 6.6f, hy + 3.4f, 0.8f, pink);
}

static void drawHeroSprite(const Ent *e, float x, float y) {
	const Look *L = &LOOK[e->type];
	Pose p;
	memset(&p, 0, sizeof(p));
	float d = cosf(e->face) < 0 ? -1.0f : 1.0f, k = e->type == 2 ? 0.96f : 0.86f;
	float feet = y + e->r;
	float since = e->acd - e->cd;
	p.team = e->team;
	p.moving = e->walk > 0;
	p.atk = e->cd > 0 && since < 0.28f;     p.ap = clampf(since / 0.28f, 0, 1);
	p.dash = e->s1 > 4.65f;                 p.ds = 5 - e->s1;
	p.burst = e->s2 > 8.5f;                 p.bs = 9 - e->s2;
	p.ult = e->s3 > ULT_CD - 1.0f;          p.us = ULT_CD - e->s3;
	p.blink = frame % 190 < 6;
	p.step = p.moving ? sinf(e->walk) * 4 : 0;
	if (p.dash) p.step = 5;
	p.bob = p.moving ? -fabsf(cosf(e->walk)) * 1.6f : sinf(frame * 0.08f + e->x * 0.05f) * 0.7f;
	p.lean = p.dash ? 5 : p.moving ? 1.2f : 0;
	if (p.ult) p.hover = -9 * sinf(clampf(p.us, 0, 1) * 3.14159f);

	oval(x, feet + 1, 20 * k, 7 * k, RGBA(0, 0, 0, 100));
	oval(x, feet + 1, 27, 10, e == &E[HERO0] ? RGBA(255, 214, 100, 170) : e->team ? RGBA(255, 90, 106, 140) : RGBA(90, 170, 255, 140));
	oval(x, feet + 1, 21, 7, RGBA(14, 30, 46, 210));

	C3D_Mtx saved;
	C2D_ViewSave(&saved);
	C2D_ViewTranslate(x, feet);
	C2D_ViewScale(d * k, k);               // d mirrors the drawing so the hero faces the way he moves
	switch (e->type) {
	case 0:  heroKnight(&p, L); break;
	case 1:  heroLyra(&p, L);   break;
	case 2:  heroGrom(&p, L);   break;
	default: heroVex(&p, L);    break;
	}
	C2D_ViewRestore(&saved);

	float barY = feet - 62 * k;
	if (e == &E[HERO0]) tri(x, barY - 3, x - 4, barY - 10, x + 4, barY - 10, RGB(255, 204, 92));   // this one is you
	bar(x, barY, 26, e->hp / e->max, e->team ? RGB(255, 138, 150) : RGB(124, 196, 255));
}

static void drawEnt(const Ent *e, float ox, float oy) {
	float x = e->x - ox, y = e->y - oy;
	if (x < -40 || x > SCR_W + 40 || y < -50 || y > SCR_H + 60) return;
	if (e->kind == K_TOWER) drawTower(e, x, y);
	else if (e->kind == K_NEXUS) drawNexus(e, x, y);
	else if (e->kind == K_CREEP) drawMonster(e, x, y);
	else if (e->hp > 0 && e->kind == K_HERO) drawHeroSprite(e, x, y);
	else if (e->hp > 0) drawFighter(e, x, y);
}

// ---- scenery: placed once, away from lanes, river, camps and bases ----
#define MAX_DECOR 900
#define MAX_PEBBLE 2400
enum { D_PINE, D_ROCK, D_BUSH, D_OAK, D_FLOWERS };
typedef struct { short x, y; unsigned char type, size; } Decor;   // size 0 small, 1 normal, 2 big
typedef struct { short x, y; unsigned char s, shade; } Pebble;    // lane paving: shade 0-2 stone slab, 3 moss, 4 curb stone
typedef struct { short x, y; } Lamp;                              // glowing lanterns beside the lanes
#define MAX_LAMP 200
static Lamp lamps[MAX_LAMP];
static int nLamp;
static Decor decor[MAX_DECOR];
static Pebble pebbles[MAX_PEBBLE];
static int nDecor, nPebble;

#define RIVER_X1 240.0f
#define RIVER_Y1 240.0f
#define RIVER_X2 960.0f
#define RIVER_Y2 960.0f

static unsigned hash2(int a, int b) {              // cheap stable random number from two integers
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u + 1442695041u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static int decorCmp(const void *a, const void *b) { return ((const Decor *)a)->y - ((const Decor *)b)->y; }

static bool nearLane(float x, float y, float margin) {
	for (int l = 0; l < LANES; l++)
		for (int i = 0; i + 1 < PATH_PTS; i++)
			if (segDist(PATH[l][i][0], PATH[l][i][1], PATH[l][i + 1][0], PATH[l][i + 1][1], x, y) < LANE_W / 2 + margin) return true;
	return false;
}

static void addDecor(float x, float y, int type, int size) {
	if (nDecor < MAX_DECOR) decor[nDecor++] = (Decor){ (short)x, (short)y, (unsigned char)type, (unsigned char)size };
}

static void makeMap(void) {
	nDecor = nPebble = 0;

	// forest wall around the edge of the world, two staggered rows so there are no gaps
	for (int row = 0; row < 2; row++)
		for (int i = 0; i < 48; i++) {
			unsigned h = hash2(i, row + 900);
			float p = 8 + i * 25.0f + (h % 9), d = 8 + row * 20 + (h >> 8) % 8;
			int t = (h >> 4) % 5 < 3 ? D_PINE : D_OAK;
			addDecor(p, d, t, 2);                       // top
			addDecor(p, WORLD - d + 14, t, 2);          // bottom
			addDecor(d, p + 12, t, 2);                  // left
			addDecor(WORLD - d, p + 12, t, 2);          // right
		}

	// groves and loose scenery: a jittered grid, thinned out, with clusters (neighbours copy a type)
	for (int gy = 0; gy < 21; gy++)
		for (int gx = 0; gx < 21; gx++) {
			unsigned h = hash2(gx, gy);
			float x = 50 + gx * 54 + (int)(h % 31) - 15, y = 50 + gy * 54 + (int)((h >> 6) % 31) - 15;
			if (x < 60 || y < 60 || x > WORLD - 60 || y > WORLD - 60) continue;
			if (nearLane(x, y, 24)) continue;
			if (segDist(RIVER_X1, RIVER_Y1, RIVER_X2, RIVER_Y2, x, y) < 62) continue;
			bool open = true;
			for (int c = 0; c < CREEPS; c++) if (hypotf(x - CAMP[c].x, y - CAMP[c].y) < (CAMP[c].type >= C_TURTLE ? 80 : 56)) open = false;
			for (int b = 0; b < BUSHES; b++)                  // keep the hiding bushes clear of trees
				if (x > BUSH[b][0] * TILE - 30 && x < (BUSH[b][0] + BUSH_TILES) * TILE + 30 &&
				    y > BUSH[b][1] * TILE - 30 && y < (BUSH[b][1] + BUSH_TILES) * TILE + 30) open = false;
			if (hypotf(x - 110, y - 1090) < 190 || hypotf(x - 1090, y - 110) < 190) open = false;
			if (!open) continue;
			unsigned r = (h >> 12) % 100;
			if (r < 22) continue;                         // leave some open meadow
			int size = (h >> 20) % 3;
			if (r < 52) addDecor(x, y, D_PINE, size);
			else if (r < 68) addDecor(x, y, D_OAK, size);
			else if (r < 78) addDecor(x, y, D_ROCK, size == 2 ? 1 : size);
			else if (r < 90) addDecor(x, y, D_FLOWERS, 1);   // (the real bushes are the 12 hiding spots)
			else addDecor(x, y, D_FLOWERS, 1);
			if (r < 52 && size == 2 && nDecor + 1 < MAX_DECOR)          // big pines come with a friend
				addDecor(x + 17, y + 8, D_PINE, 0);
		}
	qsort(decor, nDecor, sizeof(Decor), decorCmp);      // back to front

	// lane paving: stone slabs, moss and curb stones along every lane, and a glowing lantern every so often
	nLamp = 0;
	for (int l = 0; l < LANES; l++)
		for (int i = 0; i + 1 < PATH_PTS; i++) {
			float x1 = PATH[l][i][0], y1 = PATH[l][i][1], x2 = PATH[l][i + 1][0], y2 = PATH[l][i + 1][1];
			float len = hypotf(x2 - x1, y2 - y1), dx = (x2 - x1) / len, dy = (y2 - y1) / len, nx = -dy, ny = dx;
			for (float s = 6; s < len; s += 15)                       // slabs (and a little moss), two per step
				for (int side = 0; side < 2; side++) {
					unsigned h = hash2((int)(x1 + s) + side * 31, (int)(y1 + l * 77 + i));
					if (h % 5 == 0 || nPebble >= MAX_PEBBLE - 4) continue;
					float off = (side ? 1 : -1) * (3 + ((h >> 4) % 100) / 100.0f * (LANE_W / 2 - 10));
					unsigned char shade = (h >> 12) % 8 == 0 ? 3 : (unsigned char)((h >> 18) % 3);
					pebbles[nPebble++] = (Pebble){ (short)(x1 + dx * s + nx * off), (short)(y1 + dy * s + ny * off),
												   (unsigned char)(1 + (h >> 14) % 3), shade };
				}
			for (float s = 4; s < len; s += 10)                       // curb stones along both edges
				for (int side = 0; side < 2 && nPebble < MAX_PEBBLE; side++) {
					float off = (side ? 1 : -1) * (LANE_W / 2.0f + 1);
					pebbles[nPebble++] = (Pebble){ (short)(x1 + dx * s + nx * off), (short)(y1 + dy * s + ny * off), 1, 4 };
				}
			int k = 0;
			for (float s = 60; s < len - 20; s += 96, k++) {          // lanterns, one side then the other
				float off = (k & 1 ? 1 : -1) * (LANE_W / 2.0f + 8);
				float lx = x1 + dx * s + nx * off, ly = y1 + dy * s + ny * off;
				bool ok = nLamp < MAX_LAMP && hypotf(lx - 110, ly - 1090) > 170 && hypotf(lx - 1090, ly - 110) > 170 && hypotf(lx - 600, ly - 600) > 80;
				for (int t = 0; t < 2; t++) for (int q = 0; q < LANES; q++) for (int w = 0; w < TOWERS_PER_LANE; w++)
					if (hypotf(lx - TOWER_POS[t][q][w][0], ly - TOWER_POS[t][q][w][1]) < 36) ok = false;
				for (int bb = 0; bb < BUSHES; bb++)
					if (lx > BUSH[bb][0] * TILE - 14 && lx < (BUSH[bb][0] + BUSH_TILES) * TILE + 14 &&
						ly > BUSH[bb][1] * TILE - 14 && ly < (BUSH[bb][1] + BUSH_TILES) * TILE + 14) ok = false;
				if (ok) lamps[nLamp++] = (Lamp){ (short)lx, (short)ly };
			}
		}
}

static void road(float x1, float y1, float x2, float y2, float w, u32 c) {
	C2D_DrawLine(x1, y1, c, x2, y2, c, w, 0);
	disc(x1, y1, w / 2, c);
	disc(x2, y2, w / 2, c);
}

// draw a lane/river piece only when it can be seen (the bounding box grows by the width)
static bool segVisible(float x1, float y1, float x2, float y2, float w) {
	float lo = fminf(x1, x2) - w, hi = fmaxf(x1, x2) + w, up = fminf(y1, y2) - w, dn = fmaxf(y1, y2) + w;
	return !(hi < 0 || lo > SCR_W || dn < 0 || up > SCR_H);
}

static void pine(float x, float y, float s) {
	oval(x + 3 * s, y + 15 * s, 26 * s, 9 * s, RGBA(0, 0, 0, 80));
	fillr(x - 2 * s, y + 5 * s, 5 * s, 11 * s, RGB(90, 62, 44));
	tri(x - 14 * s, y + 9 * s, x, y - 12 * s, x + 14 * s, y + 9 * s, RGB(20, 92, 66));
	tri(x - 11 * s, y - 1 * s, x, y - 20 * s, x + 11 * s, y - 1 * s, RGB(34, 122, 80));
	tri(x - 7 * s, y - 9 * s, x, y - 25 * s, x + 7 * s, y - 9 * s, RGB(70, 158, 98));
	tri(x, y - 25 * s, x + 7 * s, y - 9 * s, x + 1 * s, y - 9 * s, RGBA(0, 0, 0, 45));   // shaded side
}

static void oak(float x, float y, float s) {
	oval(x + 3 * s, y + 14 * s, 30 * s, 10 * s, RGBA(0, 0, 0, 80));
	quad(x - 3 * s, y - 2 * s, x + 3 * s, y - 2 * s, x + 4 * s, y + 14 * s, x - 4 * s, y + 14 * s, RGB(96, 66, 46));
	disc(x - 9 * s, y - 6 * s, 10 * s, RGB(34, 112, 70));
	disc(x + 9 * s, y - 6 * s, 10 * s, RGB(34, 112, 70));
	disc(x, y - 14 * s, 13 * s, RGB(52, 142, 84));
	disc(x - 3 * s, y - 17 * s, 7 * s, RGB(92, 178, 108));
	disc(x + 7 * s, y - 10 * s, 3 * s, RGB(76, 160, 96));
}

static void drawDecor(const Decor *d, float x, float y) {
	float s = d->size == 0 ? 0.75f : d->size == 2 ? 1.2f : 1.0f;
	switch (d->type) {
	case D_PINE: pine(x, y, s); break;
	case D_OAK:  oak(x, y, s);  break;
	case D_ROCK:
		oval(x, y + 6 * s, 30 * s, 12 * s, RGBA(0, 0, 0, 70));
		oval(x, y, 26 * s, 18 * s, RGB(98, 110, 128));
		oval(x + 7 * s, y + 4 * s, 12 * s, 9 * s, RGB(80, 90, 108));
		oval(x - 4 * s, y - 4 * s, 15 * s, 8 * s, RGB(150, 163, 180));
		if (d->size == 1) { oval(x + 15, y + 5, 10, 7, RGB(110, 122, 140)); oval(x + 14, y + 3, 5, 3, RGB(160, 172, 190)); }
		break;
	case D_BUSH:
		oval(x, y + 8 * s, 28 * s, 8 * s, RGBA(0, 0, 0, 60));
		disc(x - 7 * s, y + 2 * s, 8 * s, RGB(36, 106, 66));
		disc(x + 7 * s, y + 2 * s, 8 * s, RGB(36, 106, 66));
		disc(x, y - 4 * s, 9 * s, RGB(58, 140, 84));
		disc(x - 3 * s, y - 6 * s, 4 * s, RGB(90, 172, 104));
		disc(x - 4 * s, y - 4 * s, 2, RGB(255, 160, 205));
		disc(x + 5 * s, y + 1 * s, 2, RGB(255, 160, 205));
		break;
	default:                                              // wild flowers on the grass
		for (int k = 0; k < 6; k++) {
			unsigned h = hash2(d->x + k * 17, d->y);
			float fx = x + (int)(h % 21) - 10, fy = y + (int)((h >> 5) % 11) - 5;
			static const u32 FL[4] = { RGB(255, 236, 140), RGB(255, 255, 255), RGB(255, 160, 205), RGB(170, 200, 255) };
			fillr(fx, fy, 1, 3, RGB(40, 110, 66));
			disc(fx + 0.5f, fy, 2, FL[(h >> 11) & 3]);
		}
		break;
	}
}

// a stone lantern with a glowing orange flame
static void drawLamp(float x, float y) {
	oval(x + 1, y + 7, 18, 7, RGBA(0, 0, 0, 80));
	fillr(x - 7, y - 3, 14, 10, RGB(96, 90, 80));
	fillr(x - 7, y - 3, 14, 3, RGB(156, 146, 126));
	fillr(x - 5, y + 3, 10, 4, RGB(74, 68, 60));
	float fl = 0.5f + 0.5f * sinf(frame * 0.09f + x * 0.3f);
	disc(x, y - 6, 6.5f, RGB(118, 70, 42));
	glow(x, y - 7, 17, 255, 150, 60);
	disc(x, y - 7, 4.6f, RGB(255, (int)(172 + 44 * fl), 92));
	disc(x - 1, y - 8, 1.8f, RGB(255, 240, 190));
}

// the wooden bridge where the middle lane crosses the river
static void drawBridge(float ox, float oy) {
	float cx = 600 - ox, cy = 600 - oy;
	if (cx < -110 || cx > SCR_W + 110 || cy < -110 || cy > SCR_H + 110) return;
	const float dx = 0.7071f, dy = -0.7071f;              // along the lane
	const float nx = 0.7071f, ny = 0.7071f;               // across the lane (along the river)
	const float half = 56, wide = LANE_W / 2 + 5;
	#define BP(a, b) cx + dx * (a) + nx * (b), cy + dy * (a) + ny * (b)
	quad(BP(-half, -wide - 3), BP(half, -wide - 3), BP(half, wide + 3), BP(-half, wide + 3), RGBA(0, 0, 0, 70));   // shadow on the water
	quad(BP(-half, -wide), BP(half, -wide), BP(half, wide), BP(-half, wide), RGB(92, 62, 40));
	for (int i = 0; i < 14; i++) {                        // planks
		float a0 = -half + 2 + i * 8, a1 = a0 + 6.5f;
		quad(BP(a0, -wide + 2), BP(a1, -wide + 2), BP(a1, wide - 2), BP(a0, wide - 2), i & 1 ? RGB(170, 126, 78) : RGB(190, 146, 92));
	}
	for (int side = -1; side <= 1; side += 2) {           // rails and posts
		float b = side * (wide - 1);
		thick(BP(-half, b), BP(half, b), 3, RGB(70, 46, 30));
		for (int i = 0; i < 5; i++) {
			float a = -half + 4 + i * 26;
			fillr(cx + dx * a + nx * b - 2, cy + dy * a + ny * b - 6, 4, 8, RGB(112, 78, 50));
			fillr(cx + dx * a + nx * b - 2, cy + dy * a + ny * b - 6, 4, 2, RGB(190, 146, 92));
		}
	}
	#undef BP
}

static void drawMap(float ox, float oy) {
	// ---- ground: two greens, soft meadow patches, then tufts and flowers
	fillr(0, 0, SCR_W, SCR_H, RGB(32, 88, 68));
	const int PC = 96;
	for (int cy = (int)floorf(oy / PC) - 1; cy <= (int)((oy + SCR_H) / PC) + 1; cy++)
		for (int cx = (int)floorf(ox / PC) - 1; cx <= (int)((ox + SCR_W) / PC) + 1; cx++) {
			unsigned h = hash2(cx, cy + 31);
			float x = cx * PC + (int)(h % PC) - ox, y = cy * PC + (int)((h >> 8) % PC) - oy;
			float w = 70 + (h >> 16) % 60, hh = w * 0.6f;
			if (h & 0x1000000) oval(x, y, w, hh, RGBA(70, 140, 90, 70));      // sunny meadow
			else oval(x, y, w, hh, RGBA(18, 66, 50, 80));                     // shady dip
		}
	const int GC = 36;                                    // grass tufts and flowers, one cell at a time
	for (int cy = (int)floorf(oy / GC) - 1; cy <= (int)((oy + SCR_H) / GC) + 1; cy++)
		for (int cx = (int)floorf(ox / GC) - 1; cx <= (int)((ox + SCR_W) / GC) + 1; cx++) {
			unsigned h = hash2(cx, cy);
			float gx = cx * GC + (int)(h % GC) - ox, gy = cy * GC + (int)((h >> 8) % GC) - oy;
			unsigned r = (h >> 16) % 10;
			if (r < 6) {
				tri(gx - 3, gy + 3, gx, gy - 3, gx + 1, gy + 3, RGB(64, 130, 82));
				tri(gx, gy + 3, gx + 3, gy - 2, gx + 4, gy + 3, RGB(50, 110, 74));
				if (r < 2) tri(gx - 6, gy + 3, gx - 4, gy - 1, gx - 3, gy + 3, RGB(80, 148, 92));
			} else if (r == 6) {
				disc(gx, gy, 1.6f, (h >> 24) & 1 ? RGB(255, 236, 150) : RGB(255, 255, 255));
			}
		}

	// ---- river: muddy bank, shallow water, deep water, lighter current, ripples
	float rx1 = RIVER_X1 - ox, ry1 = RIVER_Y1 - oy, rx2 = RIVER_X2 - ox, ry2 = RIVER_Y2 - oy;
	if (segVisible(rx1, ry1, rx2, ry2, 50)) {
		road(rx1, ry1, rx2, ry2, 98, RGB(76, 80, 72));        // stone bank
		road(rx1, ry1, rx2, ry2, 90, RGB(118, 122, 106));
		road(rx1, ry1, rx2, ry2, 80, RGB(52, 118, 128));      // shallow water
		road(rx1, ry1, rx2, ry2, 66, RGB(30, 84, 112));       // deep teal water
		road(rx1, ry1, rx2, ry2, 32, RGB(22, 62, 94));
		for (int i = 0; i < 26; i++) {                    // current streaks flowing downstream
			float p = 250 + fmodf(i * 28.0f + frame * 0.35f, 720.0f), off = (i * 7 % 5 - 2) * 10 + sinf(i) * 4;
			if (p > 950) continue;
			float a = (p > 900 ? (950 - p) / 50.0f : p < 290 ? (p - 250) / 40.0f : 1.0f);
			float sx = p + off * 0.7071f - ox, sy = p - off * 0.7071f - oy;
			if (sx < -20 || sx > SCR_W + 20 || sy < -20 || sy > SCR_H + 20) continue;
			fillr(sx, sy, 13, 2, RGBA(200, 244, 255, (int)(105 * clampf(a, 0, 1))));
			fillr(sx + 5, sy + 4, 7, 1, RGBA(200, 244, 255, (int)(60 * clampf(a, 0, 1))));
		}
		for (int i = 0; i < 9; i++) {                     // lily pads and reeds near the bank
			float p = 300 + i * 80, off = (i & 1 ? 34 : -34);
			float lx = p + off * 0.7071f - ox, ly = p - off * 0.7071f - oy;
			if (lx < -12 || lx > SCR_W + 12 || ly < -12 || ly > SCR_H + 12) continue;
			if (i & 1) { oval(lx, ly, 12, 6, RGB(60, 140, 84)); oval(lx - 1, ly - 1, 4, 2, RGB(255, 190, 220)); }
			else { fillr(lx, ly - 8, 1.5f, 10, RGB(100, 140, 70)); fillr(lx + 3, ly - 6, 1.5f, 8, RGB(120, 156, 80)); disc(lx + 0.7f, ly - 9, 2, RGB(110, 80, 50)); }
		}
	}

	// ---- bases: paved courtyard with a glowing rune ring
	for (int t = 0; t < 2; t++) {
		float bx = PATH[1][t ? PATH_PTS - 1 : 0][0] - ox, by = PATH[1][t ? PATH_PTS - 1 : 0][1] - oy;
		if (bx < -170 || bx > SCR_W + 170 || by < -170 || by > SCR_H + 170) continue;
		u32 rim = t ? RGB(84, 52, 62) : RGB(44, 62, 100), mid = t ? RGB(126, 86, 94) : RGB(78, 102, 144),
		    in = t ? RGB(146, 104, 110) : RGB(100, 126, 166), tile = t ? RGB(132, 92, 98) : RGB(86, 110, 152);
		disc(bx, by, 156, RGBA(0, 0, 0, 55));
		disc(bx, by, 150, rim);
		disc(bx, by, 144, mid);
		for (int k = 0; k < 16; k++) {                    // paving stones around the rim
			float a = k * 0.3927f, c = cosf(a), s = sinf(a);
			quad(bx + c * 126 - s * 7, by + s * 126 + c * 7, bx + c * 126 + s * 7, by + s * 126 - c * 7,
			     bx + c * 141 + s * 7, by + s * 141 - c * 7, bx + c * 141 - s * 7, by + s * 141 + c * 7, k & 1 ? tile : in);
		}
		disc(bx, by, 122, in);
		disc(bx, by, 98, tile);
		disc(bx, by, 94, in);
		u32 glowc = TEAM_COL[t] & 0x00FFFFFF;
		float pulse = 0.5f + 0.5f * sinf(frame * 0.04f + t * 2);
		disc(bx, by, 76, glowc | ((u32)(30 + 20 * pulse) << 24));
		disc(bx, by, 72, in);
		for (int k = 0; k < 8; k++) {                     // rune spokes
			float a = k * 0.7854f + 0.39f;
			thick(bx + cosf(a) * 40, by + sinf(a) * 40, bx + cosf(a) * 70, by + sinf(a) * 70, 2, glowc | ((u32)(110 + 90 * pulse) << 24));
		}
		float px = homeX(t) - ox, py = homeY(t) - oy;     // where your hero comes back to life
		oval(px, py + 9, 44, 16, glowc | ((u32)(60 + 50 * pulse) << 24));
		oval(px, py + 9, 32, 11, in);
		oval(px, py + 9, 22, 7, glowc | ((u32)(90 + 60 * pulse) << 24));
	}

	// ---- lanes: dark edge, grass-blend, dirt, lighter worn centre, pebbles
	for (int pass = 0; pass < 3; pass++)
		for (int l = 0; l < LANES; l++)
			for (int i = 0; i + 1 < PATH_PTS; i++) {
				float x1 = PATH[l][i][0] - ox, y1 = PATH[l][i][1] - oy, x2 = PATH[l][i + 1][0] - ox, y2 = PATH[l][i + 1][1] - oy;
				if (!segVisible(x1, y1, x2, y2, LANE_W)) continue;
				static const float W[3] = { LANE_W + 12, LANE_W + 5, LANE_W - 2 };               // mossy edge, stone curb, paved lane
				static const u32 C[3] = { RGB(24, 58, 48), RGB(96, 98, 88), RGB(148, 156, 140) };
				road(x1, y1, x2, y2, W[pass], C[pass]);
			}
	for (int i = 0; i < nPebble; i++) {                  // flagstones, moss and curb stones
		float x = pebbles[i].x - ox, y = pebbles[i].y - oy;
		if (x < -14 || x > SCR_W + 14 || y < -14 || y > SCR_H + 14) continue;
		unsigned h = hash2(pebbles[i].x, pebbles[i].y);
		int sh = pebbles[i].shade, sz = pebbles[i].s;
		if (sh == 4) {                                   // curb stone
			fillr(x - 5, y - 3, 10, 7, RGB(70, 72, 66));
			fillr(x - 4.5f, y - 3, 9, 5, (h & 1) ? RGB(122, 124, 110) : RGB(108, 110, 98));
			fillr(x - 4.5f, y - 3, 9, 1.5f, RGB(158, 160, 144));
		} else if (sh == 3) {                            // moss
			oval(x, y, 10 + sz * 3, 6 + sz * 2, RGBA(66, 142, 82, 200));
			oval(x - 2, y - 1, 5 + sz, 3 + sz, RGBA(110, 188, 110, 190));
		} else {                                         // flagstone with a dark gap, a light top edge and sometimes a crack
			static const u32 SL[3] = { RGB(170, 178, 160), RGB(152, 162, 146), RGB(186, 192, 172) };
			float w = 8 + sz * 3 + (h & 3), hh = 6 + sz * 2 + ((h >> 2) & 3);
			fillr(x - w / 2 - 1, y - hh / 2 - 1, w + 2, hh + 2, RGB(84, 94, 84));
			fillr(x - w / 2, y - hh / 2, w, hh, SL[sh]);
			fillr(x - w / 2, y - hh / 2, w, 1.5f, RGB(206, 212, 190));
			if (h & 0x100) thick(x - w / 3, y - hh / 4, x + w / 4, y + hh / 3, 1, RGB(96, 106, 94));
		}
	}
	for (int i = 0; i < nLamp; i++) {
		float x = lamps[i].x - ox, y = lamps[i].y - oy;
		if (x < -30 || x > SCR_W + 30 || y < -30 || y > SCR_H + 30) continue;
		drawLamp(x, y);
	}
	drawBridge(ox, oy);

	// ---- camps: trampled dirt rings with stones (buffs, Turtle and Lord glow in their own colour)
	for (int c = 0; c < CREEPS; c++) {
		float x = CAMP[c].x - ox, y = CAMP[c].y - oy;
		if (x < -70 || x > SCR_W + 70 || y < -60 || y > SCR_H + 60) continue;
		int ty = CAMP[c].type;
		bool boss = ty >= C_TURTLE, buff = ty <= C_REDBUFF;
		u32 gc = CDEF[ty].col & 0x00FFFFFF;
		float w = ty == C_LORD ? 100 : ty == C_TURTLE ? 88 : buff ? 72 : ty == C_SMALL ? 56 : 46, h = w * 0.64f;
		oval(x, y + 5, w + 10, h + 8, RGBA(20, 50, 36, 110));
		oval(x, y + 5, w, h, RGB(104, 92, 68));
		oval(x - 2, y + 3, w - 14, h - 10, RGB(126, 112, 82));
		oval(x + 3, y + 5, w - 34, h - 22, RGB(112, 100, 72));
		if (boss || buff) {
			float pulse = 0.5f + 0.5f * sinf(frame * 0.05f);
			oval(x, y + 5, w - 22, h - 16, gc | ((u32)(26 + 26 * pulse) << 24));
			for (int k = 0; k < 6; k++) {
				float a = k * 1.047f + 0.3f;
				sparkle(x + cosf(a) * (w * 0.34f), y + 5 + sinf(a) * (h * 0.34f), 3.5f, RGBA(240, 240, 255, (int)(120 + 110 * pulse)));
			}
		}
		for (int k = 0; k < 7; k++) {                     // rim stones
			float a = k * 0.8976f + c;
			float sx = x + cosf(a) * (w * 0.5f), sy = y + 5 + sinf(a) * (h * 0.5f);
			oval(sx, sy + 1, 8, 5, RGBA(0, 0, 0, 60));
			oval(sx, sy, 7, 5, RGB(112, 122, 138));
			oval(sx - 1, sy - 1, 3, 2, RGB(160, 172, 188));
		}
	}

	for (int i = 0; i < nDecor; i++) {
		float x = decor[i].x - ox, y = decor[i].y - oy;
		if (x < -30 || x > SCR_W + 30 || y < -34 || y > SCR_H + 34) continue;
		drawDecor(&decor[i], x, y);
	}

	// ---- soft shade near the edge of the world, under the forest wall
	const float EDGE = 34;
	if (ox < EDGE) fillr(0, 0, EDGE - ox, SCR_H, RGBA(8, 28, 24, 90));
	if (oy < EDGE) fillr(0, 0, SCR_W, EDGE - oy, RGBA(8, 28, 24, 90));
	if (ox + SCR_W > WORLD - EDGE) fillr(WORLD - EDGE - ox, 0, SCR_W, SCR_H, RGBA(8, 28, 24, 90));
	if (oy + SCR_H > WORLD - EDGE) fillr(0, WORLD - EDGE - oy, SCR_W, SCR_H, RGBA(8, 28, 24, 90));
}

// an emote face. type 1 dazed, 2 in love, 3 laughing, 4 crying. (cx, cy) is the middle of the face, s the scale, a the opacity
static void drawEmote(int type, float cx, float cy, float s, int a, bool bubble) {
    u32 ink = alpha(RGB(70, 36, 30), a), mouth = alpha(RGB(150, 30, 34), a), tongue = alpha(RGB(240, 100, 80), a);
    if (bubble) {                                          // a speech bubble so it is easy to spot
        oval(cx, cy, 50 * s, 46 * s, alpha(RGB(255, 255, 255), (int)(a * 0.88f)));
        tri(cx - 5 * s, cy + 19 * s, cx + 5 * s, cy + 19 * s, cx, cy + 29 * s, alpha(RGB(255, 255, 255), (int)(a * 0.88f)));
    }
    disc(cx, cy, 13 * s, alpha(RGB(190, 64, 48), a));                       // outline
    disc(cx, cy, 11.6f * s, alpha(RGB(255, 241, 230), a));
    oval(cx + 2 * s, cy + 6 * s, 18 * s, 10 * s, alpha(RGB(255, 224, 208), a));
    oval(cx - 8 * s, cy + 3 * s, 5.5f * s, 3.2f * s, alpha(RGB(255, 150, 120), a));   // rosy cheeks
    oval(cx + 8 * s, cy + 3 * s, 5.5f * s, 3.2f * s, alpha(RGB(255, 150, 120), a));
    float w = 1.5f * s + 0.4f;
    switch (type) {
    case EM_DAZED:                                         // tired eyes, tongue out, sweat drops
        thick(cx - 8 * s, cy - 3 * s, cx - 3 * s, cy - 1 * s, w, ink);
        thick(cx + 3 * s, cy - 1 * s, cx + 8 * s, cy - 3 * s, w, ink);
        oval(cx, cy + 6 * s, 7 * s, 3 * s, ink);
        oval(cx + 1.5f * s, cy + 9 * s, 6 * s, 5 * s, tongue);
        disc(cx - 15 * s, cy + 7 * s, 2.2f * s, alpha(RGB(80, 210, 240), a));
        disc(cx - 18 * s, cy + 13 * s, 1.8f * s, alpha(RGB(80, 210, 240), a));
        disc(cx - 12 * s, cy + 14 * s, 1.6f * s, alpha(RGB(80, 210, 240), a));
        break;
    case EM_LOVE:                                          // heart eyes and a big open mouth
        for (int side = -1; side <= 1; side += 2) {
            float hx = cx + side * 5.5f * s, hy = cy - 3 * s;
            u32 hc = alpha(RGB(255, 84, 60), a);
            disc(hx - 2.2f * s, hy - 1 * s, 3 * s, hc);
            disc(hx + 2.2f * s, hy - 1 * s, 3 * s, hc);
            tri(hx - 5 * s, hy + 0.2f * s, hx + 5 * s, hy + 0.2f * s, hx, hy + 5.5f * s, hc);
        }
        oval(cx, cy + 7.5f * s, 6 * s, 9 * s, alpha(RGB(255, 90, 70), a));
        break;
    case EM_LAUGH:                                         // > < eyes and a wide open mouth
        thick(cx - 8 * s, cy - 6 * s, cx - 4 * s, cy - 3.5f * s, w, ink);
        thick(cx - 8 * s, cy - 1 * s, cx - 4 * s, cy - 3.5f * s, w, ink);
        thick(cx + 8 * s, cy - 6 * s, cx + 4 * s, cy - 3.5f * s, w, ink);
        thick(cx + 8 * s, cy - 1 * s, cx + 4 * s, cy - 3.5f * s, w, ink);
        oval(cx, cy + 6.5f * s, 15 * s, 10 * s, mouth);
        oval(cx, cy + 9.5f * s, 9 * s, 4 * s, tongue);
        break;
    default:                                               // crying: closed eyes, streams of tears, wailing mouth
        thick(cx - 8 * s, cy - 3 * s, cx - 3 * s, cy - 3 * s, w, ink);
        thick(cx + 3 * s, cy - 3 * s, cx + 8 * s, cy - 3 * s, w, ink);
        for (int side = -1; side <= 1; side += 2) {
            tri(cx + side * 4 * s, cy - 2 * s, cx + side * 21 * s, cy - 6 * s, cx + side * 16 * s, cy + 6 * s, alpha(RGB(70, 170, 240), a));
            tri(cx + side * 6 * s, cy - 2 * s, cx + side * 17 * s, cy - 4 * s, cx + side * 14 * s, cy + 2 * s, alpha(RGB(150, 220, 255), a));
        }
        oval(cx, cy + 7.5f * s, 9 * s, 10 * s, mouth);
        oval(cx, cy + 10.5f * s, 6 * s, 4 * s, tongue);
        break;
    }
}

// bushes are drawn over the units, so a hero standing in one is half hidden (and an enemy there is not drawn at all)
static void drawBushes(float ox, float oy) {
	const float S = BUSH_TILES * TILE;
	for (int b = 0; b < BUSHES; b++) {
		float x = BUSH[b][0] * TILE - ox, y = BUSH[b][1] * TILE - oy;
		if (x < -S - 10 || x > SCR_W + 10 || y < -S - 30 || y > SCR_H + 10) continue;
		oval(x + S / 2, y + S / 2 + 6, S + 6, S - 4, RGBA(10, 50, 40, 120));            // dark patch on the ground under the grass
		for (int row = 0; row < 12; row++)                                           // tall grass blades, back row first
			for (int col = 0; col < 16; col++) {
				unsigned h = hash2(b * 400 + row * 16 + col, 93);
				if (h % 7 == 0) continue;
				float bx = x + 3 + col * 4.4f + (int)(h % 5) - 2, by = y + 10 + row * 5.6f + (int)((h >> 5) % 4);
				float hgt = 15 + (h >> 9) % 12 + row * 0.5f;
				float lean = ((int)((h >> 14) % 9) - 4) * 0.9f + sinf(frame * 0.05f + col * 0.7f + b) * 1.6f;
				u32 c = row < 4 ? RGBA(26, 86, 62, 235) : row < 8 ? RGBA(36, 116, 70, 235) : RGBA(62, 150, 84, 235);
				tri(bx - 2.2f, by, bx + 2.2f, by, bx + lean, by - hgt, c);
				if ((h >> 18) % 4 == 0) tri(bx - 0.6f, by, bx + 1.6f, by, bx + lean + 0.6f, by - hgt, RGBA(120, 196, 120, 200));   // light edge
			}
	}
}

static void drawTop(void) {
	float ox = camx, oy = camy;
	drawMap(ox, oy);

	int order[MAX_ENT], n = 0;                           // draw from back to front, skipping what is off screen or hiding
	for (int i = 0; i < nE; i++) {
		const Ent *e = &E[i];
		if (e->hid & 1) continue;                        // an enemy hero hiding in a bush
		if (e->x - ox < -60 || e->x - ox > SCR_W + 60 || e->y - oy < -70 || e->y - oy > SCR_H + 70) continue;
		int j = n++;
		while (j > 0 && E[order[j - 1]].y > e->y) { order[j] = order[j - 1]; j--; }
		order[j] = i;
	}
	for (int i = 0; i < n; i++) drawEnt(&E[order[i]], ox, oy);
	drawBushes(ox, oy);
	for (int i = HERO0; i < HERO0 + 2 * TEAM_SIZE; i++) {      // emotes pop up above the heroes
		const Ent *e = &E[i];
		if (e->emoteT <= 0 || e->hp <= 0 || (e->hid & 1)) continue;
		float ex = e->x - ox, ey = e->y - oy;
		if (ex < -30 || ex > SCR_W + 30 || ey < -90 || ey > SCR_H + 40) continue;
		float age = EMOTE_TIME - e->emoteT;
		float s = age < 0.2f ? 1.25f * (age / 0.2f) : 1.0f + 0.25f * fmaxf(0, 1 - (age - 0.2f) / 0.2f);   // pops up, then settles
		int a = (int)(255 * clampf(e->emoteT * 3, 0, 1));
		drawEmote(e->emote, ex, ey - 84 + sinf(frame * 0.12f) * 1.5f, s * 0.9f, a, true);
	}

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
	float buffY = 34;
	if (me->buffB > 0) { snprintf(buf, sizeof(buf), "Blue Buff %ds", (int)me->buffB + 1); text(6, buffY, 0.38f, RGB(120, 190, 255), buf); buffY += 12; }
	if (me->buffR > 0) { snprintf(buf, sizeof(buf), "Red Buff %ds", (int)me->buffR + 1); text(6, buffY, 0.38f, RGB(255, 130, 110), buf); }
	snprintf(buf, sizeof(buf), "%d:%02d   kills %d", (int)T / 60, (int)T % 60, kills);
	textC(SCR_W / 2, 3, 0.45f, RGB(255, 255, 255), buf);
	if (noticeT > 0) textC(SCR_W / 2, 20, 0.42f, RGBA(255, 220, 130, (int)(255 * clampf(noticeT, 0, 1))), notice);

	if (state == ST_OVER) {
		fillr(0, 0, SCR_W, SCR_H, RGBA(11, 30, 43, 190));
		textC(SCR_W / 2, 66, 1.1f, won ? RGB(124, 220, 160) : RGB(255, 138, 150), won ? "Victory" : "Defeat");
		snprintf(buf, sizeof(buf), "Time %d:%02d   kills %d   level %d", (int)T / 60, (int)T % 60, kills, me->lvl);
		textC(SCR_W / 2, 122, 0.5f, RGB(214, 232, 245), buf);
		snprintf(buf, sizeof(buf), "Rank: %s  %d RP  (%+d)", RANK_NAME[rankTier(rankPts)], rankPts, rankDelta);
		textC(SCR_W / 2, 142, 0.45f, rankDelta >= 0 ? RGB(124, 220, 160) : RGB(255, 138, 150), buf);
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

// ---- hero portraits for the selection screen: the very same sprites that are used in the match ----
// "on" is the highlighted card: that hero plays his attack animation every few seconds.
static void drawHeroArt(int type, bool on) {
	const Look *L = &LOOK[type];
	Pose p;
	memset(&p, 0, sizeof(p));
	p.team = 0;
	p.blink = frame % 190 < 6;
	p.bob = sinf(frame * 0.08f + type) * 0.7f;
	int t = frame % 150;
	if (on && t < 42) { p.atk = true; p.ap = t / 42.0f; }
	switch (type) {
	case 0:  heroKnight(&p, L); break;
	case 1:  heroLyra(&p, L);   break;
	case 2:  heroGrom(&p, L);   break;
	default: heroVex(&p, L);    break;
	}
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
		C2D_ViewTranslate(x + cw / 2 + 13, y + 128);      // feet of the hero (a bit right: hair and scarves trail to the left)
		float s = on ? 1.7f : 1.55f;
		C2D_ViewScale(s, s);
		drawHeroArt(i, on);
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

// a round profile photo: a coloured badge with an emblem (id 0-3 match the four heroes)
static void drawAvatar(float cx, float cy, float r, int id) {
	static const u32 BG[AVATARS] = {
		RGB(56, 110, 220), RGB(36, 130, 90), RGB(176, 84, 36), RGB(100, 56, 190),
		RGB(200, 150, 40), RGB(190, 60, 80), RGB(40, 150, 170), RGB(70, 76, 120),
	};
	float k = r / 20;
	u32 white = RGB(255, 255, 255);
	if (id == PHOTO_ID) {                                            // your camera photo (or a camera icon until you take one)
		if (photoOK) {
			float s = r * 2;
			fillr(cx - r - 2, cy - r - 2, s + 4, s + 4, RGB(255, 204, 92));
			C2D_DrawImageAt(photoImg, cx - r, cy - r, 0, NULL, s / PHOTO_N, s / PHOTO_N);
		} else {
			disc(cx, cy, r + 2, RGB(255, 204, 92));
			disc(cx, cy, r, RGB(70, 76, 120));
			fillr(cx - 9 * k, cy - 5 * k, 18 * k, 12 * k, white);
			fillr(cx - 4 * k, cy - 8 * k, 8 * k, 3 * k, white);
			disc(cx, cy + 1 * k, 4.5f * k, RGB(70, 76, 120));
			disc(cx, cy + 1 * k, 2.5f * k, RGB(120, 200, 255));
		}
		return;
	}
	disc(cx, cy, r + 2, RGB(255, 204, 92));
	disc(cx, cy, r, BG[id]);
	disc(cx, cy - r * 0.35f, r * 0.62f, RGBA(255, 255, 255, 28));
	switch (id) {
	case 0:   // Blue Knight: a shield
		quad(cx - 8 * k, cy - 10 * k, cx + 8 * k, cy - 10 * k, cx + 8 * k, cy + 1 * k, cx - 8 * k, cy + 1 * k, white);
		tri(cx - 8 * k, cy + 1 * k, cx + 8 * k, cy + 1 * k, cx, cy + 12 * k, white);
		quad(cx - 5 * k, cy - 7 * k, cx + 5 * k, cy - 7 * k, cx + 5 * k, cy, cx - 5 * k, cy, BG[0]);
		tri(cx - 5 * k, cy, cx + 5 * k, cy, cx, cy + 8 * k, BG[0]);
		break;
	case 1:   // Lyra: an arrow
		thick(cx - 10 * k, cy + 10 * k, cx + 7 * k, cy - 7 * k, 2.2f * k, white);
		tri(cx + 12 * k, cy - 12 * k, cx + 3 * k, cy - 8 * k, cx + 8 * k, cy - 3 * k, white);
		tri(cx - 10 * k, cy + 10 * k, cx - 12 * k, cy + 4 * k, cx - 5 * k, cy + 8 * k, RGB(190, 255, 210));
		break;
	case 2:   // Grom: a castle tower
		fillr(cx - 8 * k, cy - 2 * k, 16 * k, 13 * k, white);
		for (int i = 0; i < 3; i++) fillr(cx - 8 * k + i * 6.5f * k, cy - 8 * k, 3 * k, 7 * k, white);
		fillr(cx - 2.5f * k, cy + 4 * k, 5 * k, 7 * k, BG[2]);
		break;
	case 3:   // Vex: a dagger
		quad(cx, cy - 12 * k, cx + 4.5f * k, cy - 1 * k, cx, cy + 5 * k, cx - 4.5f * k, cy - 1 * k, white);
		fillr(cx - 8 * k, cy + 4 * k, 16 * k, 2.6f * k, RGB(255, 214, 120));
		fillr(cx - 1.5f * k, cy + 6 * k, 3 * k, 6 * k, RGB(255, 214, 120));
		break;
	case 4:   // a crown
		quad(cx - 10 * k, cy + 6 * k, cx + 10 * k, cy + 6 * k, cx + 10 * k, cy + 10 * k, cx - 10 * k, cy + 10 * k, white);
		tri(cx - 10 * k, cy + 6 * k, cx - 10 * k, cy - 8 * k, cx - 3 * k, cy + 1 * k, white);
		tri(cx - 6 * k, cy + 6 * k, cx, cy - 11 * k, cx + 6 * k, cy + 6 * k, white);
		tri(cx + 10 * k, cy + 6 * k, cx + 10 * k, cy - 8 * k, cx + 3 * k, cy + 1 * k, white);
		break;
	case 5:   // a flame
		disc(cx, cy + 4 * k, 8 * k, RGB(255, 150, 50));
		tri(cx - 8 * k, cy + 2 * k, cx + 8 * k, cy + 2 * k, cx, cy - 13 * k, RGB(255, 150, 50));
		disc(cx, cy + 6 * k, 4.5f * k, RGB(255, 232, 120));
		tri(cx - 4 * k, cy + 5 * k, cx + 4 * k, cy + 5 * k, cx, cy - 4 * k, RGB(255, 232, 120));
		break;
	case 6:   // a star
		sparkle(cx, cy, 13 * k, white);
		sparkle(cx, cy, 6 * k, RGB(255, 232, 120));
		break;
	default:  // the moon
		disc(cx, cy, 9 * k, white);
		disc(cx + 4.5f * k, cy - 3 * k, 8 * k, BG[7]);
		sparkle(cx + 8 * k, cy + 7 * k, 3 * k, white);
		break;
	}
}

// a small gem for a rank tier: more pips under it for a higher tier
static void drawRankBadge(float cx, float cy, float r, int tier) {
	u32 c = RANK_COL[tier];
	glow(cx, cy, r * 1.6f, c & 255, (c >> 8) & 255, (c >> 16) & 255);
	quad(cx, cy - r, cx + r * 0.8f, cy, cx, cy + r, cx - r * 0.8f, cy, RGB(255, 214, 120));
	quad(cx, cy - r + 2, cx + r * 0.8f - 2, cy, cx, cy + r - 2, cx - r * 0.8f + 2, cy, c);
	tri(cx, cy - r + 2, cx + r * 0.8f - 2, cy, cx, cy, RGBA(255, 255, 255, 70));
	sparkle(cx, cy, r * 0.5f, RGB(255, 255, 255));
	for (int i = 0; i <= tier; i++) disc(cx - tier * 2.5f + i * 5, cy + r + 4, 1.6f, RGB(255, 214, 120));
}

// top screen: the profile card
static void drawProfileTop(void) {
	char buf[64];
	grad(0, 0, SCR_W, 140, RGB(10, 12, 44), RGB(62, 46, 120));
	grad(0, 140, SCR_W, 100, RGB(62, 46, 120), RGB(14, 14, 40));
	for (int i = 0; i < 30; i++) {
		int a = 110 + (int)(100 * sinf(frame * 0.05f + i * 1.7f));
		sparkle((i * 97) % SCR_W, (i * 53) % 130, i % 7 == 0 ? 3.5f : 1.5f, RGBA(255, 255, 255, a));
	}
	shadowText(SCR_W / 2, 4, 0.7f, RGB(255, 214, 120), "MY PROFILE");
	fillr(60, 24, 80, 1, RGB(255, 214, 120));
	fillr(260, 24, 80, 1, RGB(255, 214, 120));
	frameBox(14, 32, 372, 196);

	float ax = 76, ay = 92;                                          // photo
	glow(ax, ay, 62, 255, 214, 120);
	drawAvatar(ax, ay, 38, pAvatar);
	textC(ax, 138, 0.36f, RGB(150, 175, 220), "Main hero");
	textC(ax, 150, 0.45f, HEROES[myHero].acc, HEROES[myHero].name);

	int tier = rankTier(rankPts);                                    // username and rank
	text(134, 38, 0.8f, RGB(255, 255, 255), pName);
	drawRankBadge(150, 94, 13, tier);
	text(172, 76, 0.6f, RANK_COL[tier], RANK_NAME[tier]);
	float frac = 1;
	if (tier < RANKS - 1) {
		snprintf(buf, sizeof(buf), "%d / %d RP", rankPts, RANK_AT[tier + 1]);
		frac = (rankPts - RANK_AT[tier]) / (float)(RANK_AT[tier + 1] - RANK_AT[tier]);
	} else snprintf(buf, sizeof(buf), "%d RP  -  top rank!", rankPts);
	text(172, 96, 0.4f, RGB(190, 215, 255), buf);
	fillr(134, 116, 240, 8, RGBA(0, 0, 0, 170));
	fillr(134, 116, 240 * clampf(frac, 0.02f, 1), 8, RANK_COL[tier]);
	fillr(134, 116, 240 * clampf(frac, 0.02f, 1), 2, RGBA(255, 255, 255, 120));

	text(134, 130, 0.38f, RGB(255, 214, 120), "BIO");                // bio
	if (bioN == 0) text(134, 144, BIO_SCALE, RGB(130, 150, 185), "(no bio yet)");
	for (int i = 0; i < bioN; i++) text(134, 144 + i * 14, BIO_SCALE, RGB(224, 236, 255), bioLines[i]);

	fillr(26, 198, 348, 1, RGBA(255, 214, 120, 120));                // record
	int rate = played ? (victories * 100 + played / 2) / played : 0;
	snprintf(buf, sizeof(buf), "Wins %d     Played %d     Win rate %d%%", victories, played, rate);
	textC(SCR_W / 2, 205, 0.45f, RGB(214, 232, 250), buf);
}

static void profileButton(float x, float y, float w, float h, const char *label, u32 top, u32 bottom) {
	grad(x, y, w, h, top, bottom);
	fillr(x, y, w, 2, RGBA(255, 255, 255, 150));
	fillr(x, y + h - 2, w, 2, RGBA(0, 0, 0, 90));
	shadowText(x + w / 2, y + (h - 33 * 0.55f) / 2, 0.55f, RGB(255, 255, 255), label);
}

// bottom screen: take a photo or pick an avatar, change the username, edit the bio
static void drawProfileBottom(void) {
	grad(0, 0, BOT_W, SCR_H, RGB(30, 28, 80), RGB(7, 9, 26));
	for (int i = 0; i < 18; i++)
		sparkle((i * 71) % BOT_W, (i * 43) % SCR_H, 2, RGBA(150, 200, 255, 70 + (int)(50 * sinf(frame * 0.04f + i))));
	shadowText(160, 2, 0.75f, RGB(255, 214, 120), "PROFILE");
	if (profMsgT > 0) { profMsgT--; textC(160, 26, 0.42f, RGB(255, 138, 150), profMsg); }
	else textC(160, 26, 0.4f, RGB(190, 215, 255), "Tap a photo (or press L / R)");
	for (int i = 0; i <= AVATARS; i++) {                             // the last tile is your camera photo
		float x = 8 + i * 34, y = 42;
		bool on = i == pAvatar;
		if (on) fillr(x - 3, y - 3, 36, 36, RGBA(255, 224, 140, 150 + (int)(90 * sinf(frame * 0.1f))));
		fillr(x - 1, y - 1, 32, 32, on ? RGB(255, 214, 120) : RGB(90, 96, 140));
		grad(x, y, 30, 30, RGB(20, 24, 60), RGB(10, 14, 40));
		drawAvatar(x + 15, y + 15, 11, i);
	}
	profileButton(20, 82, 280, 30, photoOK ? "Retake photo (A)" : "Take photo (A)", RGB(255, 192, 62), RGB(176, 92, 16));
	profileButton(20, 118, 280, 30, "Change username (X)", RGB(86, 134, 220), RGB(36, 62, 130));
	profileButton(20, 154, 280, 30, "Edit bio (Y)", RGB(70, 200, 170), RGB(24, 110, 110));
	profileButton(90, 194, 140, 36, "Back (B)", RGB(120, 130, 170), RGB(50, 56, 90));
}

// top screen while taking a photo: the live camera picture
static void drawCameraTop(void) {
	fillr(0, 0, SCR_W, SCR_H, RGB(8, 10, 24));
	if (camFrames > 0 && prevOK) C2D_DrawImageAt(prevImg, (SCR_W - CAM_SQ) / 2, 0, 0, NULL, 1, 1);
	else textC(SCR_W / 2, 104, 0.6f, RGB(190, 215, 255), "Starting camera...");
	fillr((SCR_W - CAM_SQ) / 2 - 1, 0, 1, SCR_H, RGB(255, 214, 120));
	fillr((SCR_W + CAM_SQ) / 2, 0, 1, SCR_H, RGB(255, 214, 120));
	shadowText(SCR_W / 2, 214, 0.45f, RGB(255, 255, 255), "Center your face, then take the photo");
}

static void drawCameraBottom(void) {
	grad(0, 0, BOT_W, SCR_H, RGB(30, 28, 80), RGB(7, 9, 26));
	for (int i = 0; i < 18; i++)
		sparkle((i * 71) % BOT_W, (i * 43) % SCR_H, 2, RGBA(150, 200, 255, 70 + (int)(50 * sinf(frame * 0.04f + i))));
	shadowText(160, 6, 0.75f, RGB(255, 214, 120), "CAMERA");
	textC(160, 34, 0.42f, RGB(190, 215, 255), camInner ? "Selfie camera" : "Outer camera");
	profileButton(20, 60, 280, 56, "TAKE PHOTO (A)", RGB(255, 192, 62), RGB(176, 92, 16));
	profileButton(20, 128, 134, 40, "Switch (X)", RGB(86, 134, 220), RGB(36, 62, 130));
	profileButton(166, 128, 134, 40, "Cancel (B)", RGB(120, 130, 170), RGB(50, 56, 90));
	profileButton(20, 176, 280, 30, "Upside down? Turn it (Y)", RGB(70, 200, 170), RGB(24, 110, 110));
	textC(160, 214, 0.38f, RGB(150, 175, 220), "The middle of the picture becomes your photo.");
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

	frameBox(8, 8, 132, 46);                                              // profile
	drawAvatar(28, 31, 16, pAvatar);
	text(50, 9, 0.5f, RGB(255, 255, 255), pName);
	text(50, 25, 0.38f, RANK_COL[rankTier(rankPts)], RANK_NAME[rankTier(rankPts)]);
	snprintf(buf, sizeof(buf), "Wins %d / %d", victories, played);
	text(50, 37, 0.34f, RGB(190, 220, 255), buf);
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
			"Y (or ATTACK): hit nearby enemies.",
			"A: dash   B: burst   X: ultimate (Lv 4)",
			"SELECT: auto-attack   L / R: emotes",
			"Bushes hide you. Jungle monsters give buffs.",
			"Break a lane's 3 towers, then the nexus.",
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
	menuButton(44, 56, "BATTLE", M_BATTLE, RGB(255, 192, 62), RGB(176, 92, 16));
	menuButton(112, 38, "Profile", M_PROFILE, RGB(70, 200, 170), RGB(24, 110, 110));
	menuButton(162, 38, "How to play", M_HELP, RGB(86, 134, 220), RGB(36, 62, 130));
	textC(160, 210, 0.42f, RGB(150, 175, 220), "A: select   Y: knight attack   START: quit");
}

static void startMatch(void) {
	enemyHero = (myHero + 1 + rand() % (NHEROES - 1)) % NHEROES;   // the red team plays a different hero
	resetGame();
	state = ST_PLAY;
	playWelcome();                       // the announcer: "Welcome to ..."
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
	} else if (state == ST_CAMERA) {
		bool shot = kDown & KEY_A, flip = kDown & KEY_X, back = kDown & KEY_B, turn = kDown & KEY_Y;
		if (touched) {
			if (tp.px >= 20 && tp.px < 300 && tp.py >= 60 && tp.py < 116) shot = true;
			else if (tp.py >= 128 && tp.py < 168) {
				if (tp.px >= 20 && tp.px < 154) flip = true;
				else if (tp.px >= 166 && tp.px < 300) back = true;
			} else if (tp.px >= 20 && tp.px < 300 && tp.py >= 176 && tp.py < 206) turn = true;
		}
		if (turn) { texFlip = !texFlip; saveCamCfg(); photoToTexture(); }
		else if (back) { camStop(); state = ST_PROFILE; }
		else if (flip) {
			camStop();
			if (!camStart(!camInner)) { profMsg = "Camera not available."; profMsgT = 180; state = ST_PROFILE; }
		} else if (shot && camFrames > 0) camShot = true;       // the photo is taken from the next picture the camera sends
	} else if (state == ST_PROFILE) {
		int before = pAvatar, slots = photoOK ? AVATARS + 1 : AVATARS;
		if (kDown & (KEY_L | KEY_DLEFT)) pAvatar = (pAvatar + slots - 1) % slots;
		if (kDown & (KEY_R | KEY_DRIGHT)) pAvatar = (pAvatar + 1) % slots;
		bool cam = kDown & KEY_A, name = kDown & KEY_X, bio = kDown & KEY_Y, back = kDown & KEY_B;
		if (touched) {
			if (tp.py >= 42 && tp.py < 72)
				for (int i = 0; i <= AVATARS; i++)
					if (tp.px >= 8 + i * 34 && tp.px < 8 + i * 34 + 30) {
						if (i == PHOTO_ID && !photoOK) cam = true;      // no photo yet: this tile opens the camera
						else pAvatar = i;
					}
			if (tp.px >= 20 && tp.px < 300) {
				if (tp.py >= 82 && tp.py < 112) cam = true;
				else if (tp.py >= 118 && tp.py < 148) name = true;
				else if (tp.py >= 154 && tp.py < 184) bio = true;
			}
			if (tp.py >= 194 && tp.py < 230 && tp.px >= 90 && tp.px < 230) back = true;
		}
		if (pAvatar != before) saveProfile();
		if (cam) {
			if (camStart(true)) state = ST_CAMERA;
			else { profMsg = "Camera not available."; profMsgT = 180; }
		}
		else if (name) editName();
		else if (bio) editBio();
		else if (back) state = ST_LOBBY;
	} else if (state == ST_HELP) {
		if ((kDown & (KEY_A | KEY_B)) || (touched && tp.px >= 90 && tp.px < 230 && tp.py >= 195 && tp.py < 229))
			state = ST_LOBBY;
	} else {
		if ((kDown & KEY_Y) && (knightClock < ANIM_RAISE || knightClock >= ANIM_END)) knightClock = ANIM_RAISE;
		if (kDown & KEY_DOWN) menuSel = (menuSel + 1) % M_COUNT;
		if (kDown & KEY_UP) menuSel = (menuSel + M_COUNT - 1) % M_COUNT;
		int choice = (kDown & KEY_A) ? menuSel : -1;
		if (touched && tp.px >= 40 && tp.px < 280) {
			if (tp.py >= 44 && tp.py < 100) choice = M_BATTLE;
			else if (tp.py >= 112 && tp.py < 150) choice = M_PROFILE;
			else if (tp.py >= 162 && tp.py < 200) choice = M_HELP;
		}
		if (choice >= 0) menuSel = choice;
		if (choice == M_BATTLE) { pickSel = myHero; state = ST_PICK; }   // go and choose a hero first
		else if (choice == M_PROFILE) state = ST_PROFILE;
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
	fillr(mx0, my0, size, size, RGB(34, 92, 64));
	for (int i = 0; i < 6; i++)                  // a few meadow patches so it is not one flat colour
		disc(mx0 + ((i * 37 + 19) % 100) * 1.4f, my0 + ((i * 61 + 7) % 100) * 1.4f, 14 + i * 2, RGBA(70, 140, 90, 60));
	{
		float x1 = mx0 + 240 * k, y1 = my0 + 240 * k, x2 = mx0 + 960 * k, y2 = my0 + 960 * k;
		C2D_DrawLine(x1, y1, RGB(98, 92, 66), x2, y2, RGB(98, 92, 66), 11, 0);
		C2D_DrawLine(x1, y1, RGB(54, 140, 170), x2, y2, RGB(54, 140, 170), 8, 0);
		C2D_DrawLine(x1, y1, RGB(36, 106, 150), x2, y2, RGB(36, 106, 150), 4, 0);
	}
	for (int t = 0; t < 2; t++) {                // the two bases
		float bx = mx0 + PATH[1][t ? PATH_PTS - 1 : 0][0] * k, by = my0 + PATH[1][t ? PATH_PTS - 1 : 0][1] * k;
		disc(bx, by, 17, t ? RGB(92, 60, 68) : RGB(52, 72, 112));
		disc(bx, by, 14, t ? RGB(136, 94, 100) : RGB(88, 112, 150));
	}
	for (int pass = 0; pass < 2; pass++)         // lanes: dark edge, then dirt, with round corners
		for (int l = 0; l < LANES; l++)
			for (int i = 0; i + 1 < PATH_PTS; i++) {
				float x1 = mx0 + PATH[l][i][0] * k, y1 = my0 + PATH[l][i][1] * k;
				float x2 = mx0 + PATH[l][i + 1][0] * k, y2 = my0 + PATH[l][i + 1][1] * k;
				u32 c = pass ? RGB(150, 158, 142) : RGB(60, 66, 60);
				float w = pass ? 4.5f : 7;
				C2D_DrawLine(x1, y1, c, x2, y2, c, w, 0);
				disc(x1, y1, w / 2, c);
				disc(x2, y2, w / 2, c);
			}
	for (int b = 0; b < BUSHES; b++)            // the 12 bushes
		fillr(mx0 + BUSH[b][0] * TILE * k, my0 + BUSH[b][1] * TILE * k, BUSH_TILES * TILE * k, BUSH_TILES * TILE * k, RGBA(60, 200, 100, 200));
	fillr(mx0 + 600 * k - 4, my0 + 600 * k - 4, 8, 8, RGB(150, 108, 66));   // the bridge
	for (int c = 0; c < CREEPS; c++)             // camps: a dim dot while empty, the monster's own colour once it is there
		disc(mx0 + CAMP[c].x * k, my0 + CAMP[c].y * k, CAMP[c].type >= C_TURTLE ? 4.5f : 3, RGBA(120, 104, 76, 200));
	for (int i = 0; i < nE; i++) {               // every living unit is a dot
		const Ent *e = &E[i];
		if (e->hp <= 0 || (e->hid & 1)) continue;    // fallen units, and enemy heroes hiding in a bush
		float x = mx0 + e->x * k, y = my0 + e->y * k;
		if (e->kind >= K_TOWER) {
			float s = e->kind == K_NEXUS ? 5 : 3;
			fillr(x - s, y - s, s * 2, s * 2, TEAM_COL[e->team]);
		} else if (e->kind == K_HERO) {
			disc(x, y, 4, i == HERO0 ? RGB(255, 214, 100) : RGB(255, 255, 255));
			disc(x, y, 2.6f, TEAM_COL[e->team]);
		} else if (e->kind == K_CREEP) disc(x, y, e->type >= C_TURTLE ? 4.5f : 3, CDEF[e->type].col);
		else disc(x, y, 1.5f, TEAM_COL[e->team]);
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
	text(156, 76, 0.4f, RGB(214, 232, 245), laneOpen(1) ? "Red nexus is open!" : "Break all 3 towers of");
	text(156, 88, 0.4f, RGB(214, 232, 245), laneOpen(1) ? "Attack it now!" : "a lane, then the nexus.");
	char ob[40];
	for (int o = 0; o < 2; o++) {                // objective timers
		const Ent *m = &E[o ? LORD_SLOT : TURTLE_SLOT];
		const char *nm = o ? "Lord" : "Turtle";
		int secs = (int)ceilf(m->rt);
		if (m->hp > 0) snprintf(ob, sizeof(ob), "%s: alive", nm);
		else snprintf(ob, sizeof(ob), "%s: %d:%02d", nm, secs / 60, secs % 60);
		text(156, 106 + o * 12, 0.4f, m->hp > 0 ? CDEF[m->type].col : RGB(150, 165, 185), ob);
	}
	snprintf(ob, sizeof(ob), "Next minion wave: %ds", (int)ceilf(nextWave - T));
	text(156, 132, 0.4f, RGB(190, 205, 225), ob);

	drawButton(6, "DASH (A)", E[HERO0].s1, 5, RGB(58, 120, 200));
	drawButton(85, "BURST (B)", E[HERO0].s2, 9, RGB(200, 90, 80));
	if (E[HERO0].lvl < ULT_LEVEL) drawButton(164, "LV 4", 0, ULT_CD, RGB(70, 75, 85));
	else drawButton(164, "ULT (X)", E[HERO0].s3, ULT_CD, RGB(140, 80, 190));
	drawButton(243, "ATTACK (Y)", E[HERO0].cd, E[HERO0].acd, RGB(230, 150, 40));      // hold it to keep hitting
	text(6, 224, 0.36f, RGB(150, 175, 220), autoAtk ? "SELECT: auto ON" : "SELECT: auto OFF");
	text(113, 224, 0.36f, RGB(255, 214, 120), "L");        // the emotes are sent with L and R; the last one sent is lit
	text(309, 224, 0.36f, RGB(255, 214, 120), "R");
	for (int i = 0; i < EMOTES; i++) {
		float bx = 126 + i * 46;
		fillr(bx, 221, 40, 17, i == emoteSel ? RGB(255, 204, 92) : RGB(11, 30, 51));
		fillr(bx + 1, 222, 38, 15, RGB(34, 58, 92));
		drawEmote(i + 1, bx + 20, 229.5f, 0.5f, 255, false);
	}
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
	measBuf = C2D_TextBufNew(512);
	loadProfile();                       // photo, username, bio and rank from the SD card (if saved before)
	wrapBio();
	initPhotoTextures();
	loadCamCfg();
	loadPhoto();                         // your camera photo, if you took one before
	initAudio();                         // loads the announcer voice clip
	if (pAvatar == PHOTO_ID && !photoOK) pAvatar = 0;
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
		if (state == ST_CAMERA) camPoll();
		frame++;
		knightClock = (knightClock + 1) % ANIM_LOOP;

		C2D_TextBufClear(textBuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, RGB(15, 42, 51));
		if (smooth) beginSmooth(top);
		else C2D_SceneBegin(top);
		bool lobby = state == ST_LOBBY || state == ST_HELP;
		if (state == ST_PICK) drawPickTop();
		else if (state == ST_CAMERA) drawCameraTop();
		else if (state == ST_PROFILE) drawProfileTop();
		else if (lobby) drawLobbyTop();
		else drawTop();
		C2D_TargetClear(bot, RGB(14, 33, 51));
		if (smooth) beginSmooth(bot);
		else C2D_SceneBegin(bot);
		if (state == ST_PICK) drawPickBottom();
		else if (state == ST_CAMERA) drawCameraBottom();
		else if (state == ST_PROFILE) drawProfileBottom();
		else if (lobby) drawLobbyBottom();
		else drawBottom();
		C3D_FrameEnd(0);
	}

	camStop();
	exitAudio();
	saveProfile();
	C2D_TextBufDelete(textBuf);
	C2D_TextBufDelete(measBuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
