/* The Diamonds game on the host, over an in-memory key-value store:
 * slides and merges each way, the settle, the game over, the reset and
 * the saved board. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "ui/ui.h"
#include "game.h"
#include "stubs.h"

static struct {
	char key[32];
	uint8_t val[64];
	unsigned len;
	bool used;
} kvs[4];
static unsigned kv_writes;

int kv_get(const char *key, void *buf, unsigned size)
{
	for (unsigned i = 0; i < 4; i++)
		if (kvs[i].used && !strcmp(kvs[i].key, key)) {
			memcpy(buf, kvs[i].val, kvs[i].len < size ? kvs[i].len : size);
			return (int)kvs[i].len;
		}
	return -2;
}

int kv_set(const char *key, const void *val, unsigned len)
{
	kv_writes++;
	for (unsigned i = 0; i < 4; i++)
		if (!kvs[i].used || !strcmp(kvs[i].key, key)) {
			strcpy(kvs[i].key, key);
			memcpy(kvs[i].val, val, len);
			kvs[i].len = len;
			kvs[i].used = true;
			return 0;
		}
	return -1;
}

/* The board as a string of 16 values, rows top to bottom. */
static void board(char *out)
{
	uint16_t c[4][4] = { 0 };
	const struct ui_tile *t = game_tiles();
	for (int i = 0; i < UI_TILES; i++)
		if (t[i].x >= 0)
			c[t[i].x][t[i].y] = t[i].val;
	out[0] = 0;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			sprintf(out + strlen(out), "%u%s", c[x][y], x == 3 ? (y == 3 ? "" : "/") : ",");
}

/* Loads a board through the store: rows top to bottom. */
static void load(const char *s, uint32_t score, uint32_t best)
{
	struct {
		uint16_t cells[16];
		uint32_t score, best;
	} v = { .score = score, .best = best };
	int x = 0, y = 0;
	for (const char *p = s; *p; p++) {
		if (*p == ',') {
			x++;
		} else if (*p == '/') {
			y++;
			x = 0;
		} else {
			v.cells[x * 4 + y] = (uint16_t)(v.cells[x * 4 + y] * 10 + (*p - '0'));
		}
	}
	kv_set("2048", &v, sizeof v);
	kv_writes = 0;
	game_init();
}

static unsigned count(void)
{
	unsigned n = 0;
	for (int i = 0; i < UI_TILES; i++)
		n += game_tiles()[i].x >= 0;
	return n;
}

static bool board_is(const char *want)
{
	char b[80];
	board(b);
	if (strcmp(b, want))
		fprintf(stderr, "board %s, want %s\n", b, want);
	return !strcmp(b, want);
}

int main(void)
{
	char b[80];
	/* A new game: two tiles, nothing saved until asked. */
	game_init();
	CHECK(count() == 2 && game_score() == 0 && !game_over());
	CHECK(game_tick(0));
	CHECK(!game_tick(0));
	CHECK(kv_writes == 0);
	game_save();
	CHECK(kv_writes == 0);

	/* Slides and merges: the rows are y, left to right is x. */
	load("2,2,4,0/0,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_LEFT, 1000);
	CHECK(board_is("4,4,0,0/0,0,0,0/0,0,0,0/0,0,0,0"));
	CHECK(game_score() == 4);
	/* Both slots sit on the merged cell with pop until the settle. */
	unsigned pops = 0;
	for (int i = 0; i < UI_TILES; i++)
		pops += game_tiles()[i].pop;
	CHECK(pops == 2 && count() == 3);
	CHECK(game_tick(1000));
	game_move(UI_GAME_RIGHT, 1100);	/* ignored while settling */
	CHECK(board_is("4,4,0,0/0,0,0,0/0,0,0,0/0,0,0,0"));
	CHECK(!game_tick(1199));
	CHECK(game_tick(1200));
	CHECK(count() == 3);	/* the duplicate dropped, a new tile placed */
	board(b);
	CHECK(b[0] == '4' && b[2] == '4');

	load("2,2,2,2/0,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_LEFT, 0);
	CHECK(board_is("4,4,0,0/0,0,0,0/0,0,0,0/0,0,0,0") && game_score() == 8);
	load("4,4,8,8/0,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_LEFT, 0);
	CHECK(board_is("8,16,0,0/0,0,0,0/0,0,0,0/0,0,0,0") && game_score() == 24);
	load("0,2,2,4/0,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_LEFT, 0);
	CHECK(board_is("4,4,0,0/0,0,0,0/0,0,0,0/0,0,0,0"));
	load("2,2,4,0/0,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_RIGHT, 0);
	CHECK(board_is("0,0,4,4/0,0,0,0/0,0,0,0/0,0,0,0"));
	load("2,0,0,0/2,0,0,0/4,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_UP, 0);
	CHECK(board_is("4,0,0,0/4,0,0,0/0,0,0,0/0,0,0,0"));
	load("2,0,0,0/2,0,0,0/4,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_DOWN, 0);
	CHECK(board_is("0,0,0,0/0,0,0,0/4,0,0,0/4,0,0,0"));
	/* A move that moves nothing does not settle or place a tile. */
	load("2,0,0,0/4,0,0,0/0,0,0,0/0,0,0,0", 0, 0);
	game_move(UI_GAME_LEFT, 0);
	game_tick(0);
	CHECK(game_tick(5000) == false && count() == 2);

	/* Game over: the last cell filled, with neighbours no new tile can
	 * merge with. */
	load("2,4,2,4/4,2,4,2/8,4,2,16/0,64,32,8", 100, 50);
	CHECK(!game_over());
	game_move(UI_GAME_LEFT, 0);
	CHECK(board_is("2,4,2,4/4,2,4,2/8,4,2,16/64,32,8,0") && !game_over());
	game_tick(200);
	CHECK(count() == 16 && game_over() && game_best() == 100);
	/* Reset only after a game over, saves the best. */
	load("2,4,2,4/4,2,4,2/2,4,2,4/4,2,4,2", 300, 50);
	CHECK(game_over() && game_best() == 300);
	game_move(UI_GAME_LEFT, 0);
	CHECK(board_is("2,4,2,4/4,2,4,2/2,4,2,4/4,2,4,2"));
	game_move(UI_GAME_RESET, 0);
	CHECK(!game_over() && count() == 2 && game_score() == 0 && game_best() == 300);
	game_save();
	CHECK(kv_writes == 1);
	game_save();
	CHECK(kv_writes == 1);
	/* The saved board comes back. */
	board(b);
	game_init();
	CHECK(board_is(b) && game_best() == 300);
	if (stub_failures)
		fprintf(stderr, "game_test: %d failures\n", stub_failures);
	else
		printf("game_test: ok\n");
	return stub_failures != 0;
}
