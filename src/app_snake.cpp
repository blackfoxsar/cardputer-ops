#include "app_snake.h"

#include <esp_system.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "config.h"
#include "ui_common.h"

namespace {

constexpr int kCell = 10;
constexpr int kGridW = 24;
constexpr int kGridH = 11;
constexpr int kPlayY = 22;

struct Pt {
    int8_t x;
    int8_t y;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
};

bool spawnFood(const std::vector<Pt>& snake, Pt& out_food) {
    std::vector<Pt> empty;
    empty.reserve(kGridW * kGridH);
    for (int8_t y = 0; y < kGridH; ++y) {
        for (int8_t x = 0; x < kGridW; ++x) {
            Pt p{x, y};
            if (std::find(snake.begin(), snake.end(), p) == snake.end()) {
                empty.push_back(p);
            }
        }
    }
    if (empty.empty()) return false;
    out_food = empty[esp_random() % empty.size()];
    return true;
}

void drawSnakeFrame(const std::vector<Pt>& snake, const Pt& food, int score) {
    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    std::string sc = "score:" + std::to_string(score);
    ui::drawHeader("Snake", sc.c_str(), cfg::COL_CREAM);

    for (const auto& seg : snake) {
        cv.fillRect(seg.x * kCell, kPlayY + seg.y * kCell, kCell - 1, kCell - 1, cfg::COL_ORANGE);
    }
    cv.fillRect(food.x * kCell, kPlayY + food.y * kCell, kCell - 1, kCell - 1, cfg::COL_CREAM);
    cv.pushSprite(0, 0);
}

bool gameOverScreen(int score, bool won) {
    auto& cv = ui::canvas();
    ui::waitKeyRelease();
    while (true) {
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Snake");
        cv.setTextDatum(top_center);
        cv.setTextColor(won ? cfg::COL_GREEN : cfg::COL_RED, cfg::COL_BG);
        cv.drawString(won ? "You Win!" : "Game Over", cfg::SCREEN_W / 2, 36);
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        std::string sc = "Score: " + std::to_string(score);
        cv.drawString(sc.c_str(), cfg::SCREEN_W / 2, 58);
        cv.setTextDatum(top_left);
        ui::drawHints("R / Enter again", "Q / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return false;
        }
        if (k.enter || lc == 'r') {
            ui::waitKeyRelease();
            return true;
        }
        delay(25);
    }
}

}  // namespace

void runSnakeApp() {
    ui::waitKeyRelease();

    while (true) {
        std::vector<Pt> snake = {
            Pt{kGridW / 2, kGridH / 2},
            Pt{kGridW / 2 - 1, kGridH / 2},
            Pt{kGridW / 2 - 2, kGridH / 2},
        };
        Pt dir{1, 0};
        Pt pending = dir;
        Pt food{0, 0};
        spawnFood(snake, food);
        int score = 0;

        drawSnakeFrame(snake, food, score);
        uint32_t last_step = millis();
        bool early_exit = false;
        bool won = false;

        while (true) {
            KeyInput k = ui::pollInput(90);
            ui::applyMenuNav(k);
            char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

            if (k.esc || k.btn_g0 || lc == 'q') {
                early_exit = true;
                break;
            }
            if ((k.up || lc == 'w') && dir.y != 1) pending = Pt{0, -1};
            else if ((k.down || lc == 's') && dir.y != -1) pending = Pt{0, 1};
            else if ((k.left || lc == 'a') && dir.x != 1) pending = Pt{-1, 0};
            else if ((k.right || lc == 'd') && dir.x != -1) pending = Pt{1, 0};

            if (millis() - last_step >= 160) {
                last_step = millis();
                dir = pending;
                Pt nh{static_cast<int8_t>(snake[0].x + dir.x),
                      static_cast<int8_t>(snake[0].y + dir.y)};

                if (nh.x < 0 || nh.x >= kGridW || nh.y < 0 || nh.y >= kGridH) {
                    break;
                }
                bool ate = (nh == food);
                size_t check_len = ate ? snake.size() : (snake.size() - 1);
                bool hit_self = false;
                for (size_t i = 0; i < check_len; ++i) {
                    if (snake[i] == nh) {
                        hit_self = true;
                        break;
                    }
                }
                if (hit_self) break;

                snake.insert(snake.begin(), nh);
                if (ate) {
                    ++score;
                    if (!spawnFood(snake, food)) {
                        won = true;
                        break;
                    }
                } else {
                    snake.pop_back();
                }
                drawSnakeFrame(snake, food, score);
            }
            delay(15);
        }

        if (early_exit) return;
        if (!gameOverScreen(score, won)) return;
    }
}
