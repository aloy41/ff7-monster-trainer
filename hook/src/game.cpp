#include "game.h"

namespace ff7 {

const char* mode_name(uint16_t m) {
    switch (m) {
        case FIELD: return "field";
        case BATTLE: return "battle";
        case WORLDMAP: return "worldmap";
        case MENU: return "menu";
        case BATTLE_MENU: return "battle-menu";
        case MAIN_MENU: return "main-menu";
        case INTRO: return "intro";
        case SWIRL: return "swirl";
        case GAMEOVER: return "gameover";
        case CREDITS: return "credits";
        default: return "?";
    }
}

void ff_text(const uint8_t* src, int max, char* out) {
    int n = 0;
    for (; n < max && src[n] != 0xFF; n++) out[n] = src[n] < 0x60 ? char(src[n] + 0x20) : src[n] == 0xC1 ? '-' : '?';  // 0xC1: the game's middle dot
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

}  // namespace ff7
