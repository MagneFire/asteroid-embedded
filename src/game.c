/* The game follows asteroid-diamonds: each tile has a slot that keeps
 * its place while it slides, so the UI can animate it; a tile sliding
 * onto one of the same value doubles it, both slots sit on the cell
 * with pop set until the move settles, when the later slot is freed
 * and a 2 (or a 4, one time in ten) appears on an empty cell. The game
 * is over with the board full and no two neighbours alike. */
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"
#include "storage/storage.h"
#include "game.h"

#define KV_GAME		"2048"
#define SETTLE_MS	200
#define N		4

struct saved {
	uint16_t cells[N * N];
	uint32_t score, best;
};

static uint16_t cells[N][N];	/* [column][row] */
static struct ui_tile tiles[UI_TILES];
static uint32_t score, best, settle_t, seed;
static bool over, changed, dirty, settling;

static uint32_t rnd(void)
{
	if (!seed)
		seed = time_us() | 1;
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static int free_slot(void)
{
	for (int i = 0; i < UI_TILES; i++)
		if (tiles[i].x < 0)
			return i;
	return -1;
}

static bool move_possible(void)
{
	for (int x = 0; x < N; x++)
		for (int y = 0; y < N; y++)
			if ((y < N - 1 && cells[x][y] == cells[x][y + 1]) || (x < N - 1 && cells[x][y] == cells[x + 1][y]))
				return true;
	return false;
}

static void place_random(void)
{
	int empty[N * N][2], n = 0;
	for (int x = 0; x < N; x++)
		for (int y = 0; y < N; y++)
			if (!cells[x][y]) {
				empty[n][0] = x;
				empty[n][1] = y;
				n++;
			}
	if (!n)
		return;
	int k = (int)(rnd() % (uint32_t)n), x = empty[k][0], y = empty[k][1];
	cells[x][y] = rnd() % 10 < 9 ? 2 : 4;
	int s = free_slot();
	if (s >= 0)
		tiles[s] = (struct ui_tile){ (int8_t)x, (int8_t)y, cells[x][y], false, false };
	if (n <= 1 && !move_possible()) {
		if (score > best)
			best = score;
		over = true;
	}
	changed = dirty = true;
}

/* A tile from (x1, y1) onto (x2, y2): false when another value is in
 * the way or the two merged, which ends the run along that line. */
static bool move_cell(int x1, int y1, int x2, int y2)
{
	uint16_t c1 = cells[x1][y1], c2 = cells[x2][y2];
	if (c1 && c2 && c1 != c2)
		return false;
	if (!c1)
		return true;
	cells[x1][y1] = 0;
	for (int i = 0; i < UI_TILES; i++)
		if (tiles[i].x == x1 && tiles[i].y == y1) {
			tiles[i].x = (int8_t)x2;
			tiles[i].y = (int8_t)y2;
			tiles[i].anim = true;
			break;
		}
	settling = true;
	if (c2) {
		cells[x2][y2] = (uint16_t)(c2 * 2);
		score += cells[x2][y2];
		for (int i = 0; i < UI_TILES; i++)
			if (tiles[i].x == x2 && tiles[i].y == y2) {
				tiles[i].val = cells[x2][y2];
				tiles[i].pop = true;
			}
		return false;
	}
	cells[x2][y2] = c1;
	return true;
}

static void reset(void)
{
	memset(cells, 0, sizeof cells);
	for (int i = 0; i < UI_TILES; i++)
		tiles[i] = (struct ui_tile){ -1, -1, 0, false, false };
	score = 0;
	over = false;
	settling = false;
	place_random();
	place_random();
}

static bool restore(void)
{
	struct saved v;
	if (kv_get(KV_GAME, &v, sizeof v) != (int)sizeof v)
		return false;
	best = v.best;
	bool any = false;
	for (int i = 0; i < N * N; i++)
		any |= v.cells[i] != 0;
	if (!any)
		return false;
	for (int i = 0; i < UI_TILES; i++)
		tiles[i] = (struct ui_tile){ -1, -1, 0, false, false };
	int empty = 0, s = 0;
	for (int x = 0; x < N; x++)
		for (int y = 0; y < N; y++) {
			cells[x][y] = v.cells[x * N + y];
			if (cells[x][y])
				tiles[s++] = (struct ui_tile){ (int8_t)x, (int8_t)y, cells[x][y], false, false };
			else
				empty++;
		}
	score = v.score;
	settling = false;
	over = !empty && !move_possible();
	if (over && score > best)
		best = score;
	return true;
}

void game_init(void)
{
	if (!restore())
		reset();
	changed = true;
	dirty = false;
}

void game_move(int dir, uint32_t now)
{
	if (dir == UI_GAME_RESET) {
		if (over) {
			reset();
			changed = dirty = true;
		}
		return;
	}
	if (settling || over)
		return;
	for (int i = 0; i < UI_TILES; i++)
		tiles[i].anim = false;
	bool forward = dir == UI_GAME_LEFT || dir == UI_GAME_UP, along_x = dir == UI_GAME_LEFT || dir == UI_GAME_RIGHT;
	for (int x = 0; x < N; x++) {
		for (int k = 0; k < N; k++) {
			int y = forward ? k : N - 1 - k;
			for (int m = 1; m < N; m++) {
				int j = forward ? y + m : y - m;
				if (j < 0 || j >= N)
					break;
				bool on = along_x ? move_cell(j, x, y, x) : move_cell(x, j, x, y);
				if (!on)
					break;
			}
		}
	}
	if (settling) {
		settle_t = now + SETTLE_MS;
		changed = true;
	}
}

bool game_tick(uint32_t now)
{
	if (settling && (int32_t)(now - settle_t) >= 0) {
		settling = false;
		for (int i = 0; i < UI_TILES; i++) {
			tiles[i].pop = false;
			if (tiles[i].x < 0)
				continue;
			for (int j = i + 1; j < UI_TILES; j++)
				if (tiles[j].x == tiles[i].x && tiles[j].y == tiles[i].y)
					tiles[j] = (struct ui_tile){ -1, -1, 0, false, false };
		}
		place_random();
	}
	bool r = changed;
	changed = false;
	return r;
}

void game_save(void)
{
	if (!dirty)
		return;
	struct saved v = { .score = score, .best = best };
	for (int x = 0; x < N; x++)
		for (int y = 0; y < N; y++)
			v.cells[x * N + y] = cells[x][y];
	if (!kv_set(KV_GAME, &v, sizeof v))
		dirty = false;
}

const struct ui_tile *game_tiles(void) { return tiles; }
uint32_t game_score(void) { return score; }
uint32_t game_best(void) { return best; }
bool game_over(void) { return over; }
