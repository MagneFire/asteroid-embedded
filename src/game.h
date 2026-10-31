/* The Diamonds game (2048 on a 4 by 4 board turned 45 degrees): game_move
 * slides the tiles one way (enum ui_game_move, UI_GAME_RESET starts
 * over after a game over), game_tick settles a move 200 ms later by
 * dropping merged tiles and placing a new one, and says whether the
 * board changed since the last call. The board, score and best score
 * are in the key-value store; game_save writes them when they changed,
 * for the app's close and the watch's sleep. */
#ifndef GAME_H
#define GAME_H
#include <stdint.h>
#include <stdbool.h>

struct ui_tile;

void game_init(void);
void game_move(int dir, uint32_t now);
bool game_tick(uint32_t now);
void game_save(void);
const struct ui_tile *game_tiles(void);
uint32_t game_score(void);
uint32_t game_best(void);
bool game_over(void);
#endif
