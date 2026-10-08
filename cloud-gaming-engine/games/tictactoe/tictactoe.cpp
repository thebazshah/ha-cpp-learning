// Tic-Tac-Toe for the Cloud Gaming Engine.
//
// Two players take turns placing X and O on a 3x3 board; three in a row wins.
// Player slot 0 plays X, player slot 1 plays O. Extra visitors watch as spectators.
//
// Controls (forwarded from the browser):
//   * click a square        - place your mark (only on your turn)
//   * keys 1-9              - place your mark (1 = top-left ... 9 = bottom-right)
//   * click / R after a round has ended - start the next round
//
// The whole game is this one file. It only uses the engine SDK
// (cge/GameApi.hpp): input() receives player actions, update() advances
// animations, render() draws the picture that the engine streams.

#include <cge/GameApi.hpp>

#include <array>
#include <cmath>
#include <string>

namespace {

// ---- Layout (canvas is 480 x 640 pixels) ----
constexpr int kWidth = 480;
constexpr int kHeight = 640;
constexpr int kBoardX = 24;     // top-left corner of the 3x3 board
constexpr int kBoardY = 116;
constexpr int kCell = 144;      // size of one square
constexpr int kBoard = kCell * 3;

// ---- Colours ----
constexpr cge::Color kBackground = 0x1B1F2E;
constexpr cge::Color kPanel = 0x252A3D;
constexpr cge::Color kGrid = 0x3D4462;
constexpr cge::Color kHover = 0x2F3550;
constexpr cge::Color kText = 0xE8EAF2;
constexpr cge::Color kMuted = 0x8A90A8;
constexpr cge::Color kXColor = 0xFF6B6B;
constexpr cge::Color kOColor = 0x4ECDC4;
constexpr cge::Color kXDim = 0x6B3A44;
constexpr cge::Color kODim = 0x2E5F63;
constexpr cge::Color kWinLine = 0xFFE66D;

// All eight ways to get three in a row (cell numbers 0..8, row by row).
constexpr int kLines[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6},
                              {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6}};

enum Mark { kEmpty = 0, kX = 1, kO = 2 };

std::string shortName(const std::string& name) {
    return name.size() > 10 ? name.substr(0, 9) + "." : name;
}

}  // namespace

class TicTacToe : public cge::Game {
public:
    void start(cge::GameHost& host) override {
        host_ = &host;
        host_->log("Tic-Tac-Toe started");
    }

    void playerJoined(int player, const std::string& name) override {
        if (player < 2) host_->log(std::string(player == 0 ? "X" : "O") + " is now played by " + name);
    }

    void input(const cge::InputEvent& event) override {
        if (event.player > 1) return;  // only the two players can act

        switch (event.kind) {
            case cge::InputKind::PointerMove:
                hover_[event.player] = cellAt(event.x, event.y);
                break;
            case cge::InputKind::PointerDown:
                if (roundOver()) {
                    startNextRound();
                } else {
                    play(event.player, cellAt(event.x, event.y));
                }
                break;
            case cge::InputKind::KeyDown:
                if (event.key.size() == 1 && event.key[0] >= '1' && event.key[0] <= '9') {
                    play(event.player, event.key[0] - '1');
                } else if ((event.key == "r" || event.key == "R" || event.key == "Enter") && roundOver()) {
                    startNextRound();
                }
                break;
            default:
                break;
        }
    }

    void update(double seconds) override { clock_ += seconds; }

    void render(cge::Canvas& canvas) override {
        canvas.clear(kBackground);
        drawHeader(canvas);
        drawBoard(canvas);
        drawFooter(canvas);
    }

    std::string status() const override {
        if (!bothPlayersHere()) {
            if (name(0).empty()) return "Waiting for player X to join";
            if (name(1).empty()) return "Waiting for an opponent - share the session link!";
            return "Waiting for " + (host_->playerConnected(0) ? name(1) : name(0)) + " to reconnect";
        }
        if (winner_ == kX || winner_ == kO) return name(winner_ - 1) + " wins round " + std::to_string(round_) + "!";
        if (winner_ == 3) return "Round " + std::to_string(round_) + " is a draw";
        return name(turn_) + "'s turn (" + (turn_ == 0 ? "X" : "O") + ")";
    }

    std::vector<std::pair<std::string, std::string>> metrics() const override {
        return {
            {"Round", std::to_string(round_)},
            {"Moves this round", std::to_string(movesThisRound_)},
            {"Total moves", std::to_string(totalMoves_)},
            {"X wins", std::to_string(score_[0])},
            {"O wins", std::to_string(score_[1])},
            {"Draws", std::to_string(score_[2])},
            {"Turn", roundOver() ? "-" : (turn_ == 0 ? "X" : "O")},
            {"Last move", lastMove_ < 0 ? "-" : "square " + std::to_string(lastMove_ + 1)},
        };
    }

private:
    // ------------------------------------------------------------------ rules

    std::string name(int player) const {
        const std::string value = host_ ? host_->playerName(player) : "";
        return value.empty() ? "" : shortName(value);
    }

    bool bothPlayersHere() const { return host_ && host_->playerConnected(0) && host_->playerConnected(1); }

    bool roundOver() const { return winner_ != kEmpty; }

    // Which square (0..8) is at canvas position x/y, or -1 for none.
    static int cellAt(int x, int y) {
        if (x < kBoardX || y < kBoardY || x >= kBoardX + kBoard || y >= kBoardY + kBoard) return -1;
        return ((y - kBoardY) / kCell) * 3 + (x - kBoardX) / kCell;
    }

    void play(int player, int cell) {
        if (cell < 0 || cell > 8 || roundOver() || !bothPlayersHere()) return;
        if (player != turn_ || board_[cell] != kEmpty) return;  // not your turn / square taken

        board_[cell] = (player == 0) ? kX : kO;
        placedAt_[cell] = clock_;
        lastMove_ = cell;
        ++movesThisRound_;
        ++totalMoves_;

        for (const auto& line : kLines) {
            const int a = board_[line[0]];
            if (a != kEmpty && a == board_[line[1]] && a == board_[line[2]]) {
                winner_ = a;
                winLine_ = {line[0], line[1], line[2]};
                wonAt_ = clock_;
                ++score_[a - 1];
                return;
            }
        }
        if (movesThisRound_ == 9) {
            winner_ = 3;  // draw
            ++score_[2];
            return;
        }
        turn_ = 1 - turn_;
    }

    void startNextRound() {
        if (!bothPlayersHere()) return;
        board_.fill(kEmpty);
        winner_ = kEmpty;
        movesThisRound_ = 0;
        lastMove_ = -1;
        ++round_;
        turn_ = (round_ % 2 == 1) ? 0 : 1;  // X starts odd rounds, O starts even rounds
    }

    // ------------------------------------------------------------------ drawing

    void drawHeader(cge::Canvas& canvas) const {
        canvas.drawTextCentered(kWidth / 2, 18, "TIC-TAC-TOE", 3, kText);

        // "X  Alice      vs      Bob  O"
        const std::string xName = name(0).empty() ? "(free)" : name(0);
        const std::string oName = name(1).empty() ? "(free)" : name(1);
        canvas.drawText(24, 62, "X", 3, kXColor);
        canvas.drawText(50, 66, xName, 2, host_->playerConnected(0) ? kText : kMuted);
        canvas.drawTextCentered(kWidth / 2, 66, "vs", 2, kMuted);
        canvas.drawText(kWidth - 24 - cge::Canvas::textWidth("O", 3), 62, "O", 3, kOColor);
        canvas.drawText(kWidth - 50 - cge::Canvas::textWidth(oName, 2), 66, oName, 2,
                        host_->playerConnected(1) ? kText : kMuted);

        // A pulsing bar under the player whose turn it is.
        if (bothPlayersHere() && !roundOver()) {
            const double pulse = 0.5 + 0.5 * std::sin(clock_ * 5.0);
            const int width = 60 + static_cast<int>(pulse * 60);
            const cge::Color color = turn_ == 0 ? kXColor : kOColor;
            const int x = turn_ == 0 ? 24 : kWidth - 24 - width;
            canvas.fillRoundRect(x, 92, width, 6, 3, color);
        }
    }

    void drawBoard(cge::Canvas& canvas) const {
        canvas.fillRoundRect(kBoardX - 8, kBoardY - 8, kBoard + 16, kBoard + 16, 16, kPanel);

        // Hover preview for the player whose turn it is.
        const int hover = hover_[turn_];
        if (bothPlayersHere() && !roundOver() && hover >= 0 && board_[hover] == kEmpty) {
            const int x = kBoardX + (hover % 3) * kCell;
            const int y = kBoardY + (hover / 3) * kCell;
            canvas.fillRoundRect(x + 8, y + 8, kCell - 16, kCell - 16, 12, kHover);
            drawMark(canvas, hover, turn_ == 0 ? kX : kO, 1.0, true);
        }

        // Grid lines.
        for (int i = 1; i < 3; ++i) {
            canvas.fillRoundRect(kBoardX + i * kCell - 3, kBoardY + 6, 6, kBoard - 12, 3, kGrid);
            canvas.fillRoundRect(kBoardX + 6, kBoardY + i * kCell - 3, kBoard - 12, 6, 3, kGrid);
        }

        // Marks grow in during their first 0.15 seconds.
        for (int cell = 0; cell < 9; ++cell) {
            if (board_[cell] == kEmpty) continue;
            const double grow = std::min(1.0, (clock_ - placedAt_[cell]) / 0.15);
            drawMark(canvas, cell, board_[cell], 0.4 + 0.6 * grow, false);
        }

        // The winning line is drawn from one end to the other over 0.35 seconds.
        if (winner_ == kX || winner_ == kO) {
            const double progress = std::min(1.0, (clock_ - wonAt_) / 0.35);
            const int x0 = kBoardX + (winLine_[0] % 3) * kCell + kCell / 2;
            const int y0 = kBoardY + (winLine_[0] / 3) * kCell + kCell / 2;
            const int x2 = kBoardX + (winLine_[2] % 3) * kCell + kCell / 2;
            const int y2 = kBoardY + (winLine_[2] / 3) * kCell + kCell / 2;
            const int x1 = x0 + static_cast<int>((x2 - x0) * progress);
            const int y1 = y0 + static_cast<int>((y2 - y0) * progress);
            canvas.drawLine(x0, y0, x1, y1, 14, kWinLine);
        }
    }

    void drawMark(cge::Canvas& canvas, int cell, int mark, double scale, bool preview) const {
        const int cx = kBoardX + (cell % 3) * kCell + kCell / 2;
        const int cy = kBoardY + (cell / 3) * kCell + kCell / 2;
        const int size = static_cast<int>(44 * scale);
        if (mark == kX) {
            const cge::Color color = preview ? kXDim : kXColor;
            canvas.drawLine(cx - size, cy - size, cx + size, cy + size, 16, color);
            canvas.drawLine(cx - size, cy + size, cx + size, cy - size, 16, color);
        } else {
            canvas.strokeCircle(cx, cy, size + 6, 15, preview ? kODim : kOColor);
        }
    }

    void drawFooter(cge::Canvas& canvas) const {
        const std::string score = "X " + std::to_string(score_[0]) + " : " + std::to_string(score_[1]) + " O    draws " +
                                  std::to_string(score_[2]);
        canvas.drawTextCentered(kWidth / 2, 574, score, 2, kText);

        std::string hint;
        if (!bothPlayersHere()) {
            hint = "waiting for two players...";
        } else if (roundOver()) {
            hint = "click or press R for the next round";
        } else {
            hint = "click a square or press 1-9";
        }
        canvas.drawTextCentered(kWidth / 2, 606, hint, 2, kMuted);
    }

    // ------------------------------------------------------------------ state

    cge::GameHost* host_ = nullptr;
    std::array<int, 9> board_{};          // kEmpty / kX / kO per square
    std::array<double, 9> placedAt_{};    // when each mark was placed (for the grow animation)
    std::array<int, 2> hover_{{-1, -1}};  // square under each player's mouse pointer
    std::array<int, 3> winLine_{};
    std::array<int, 3> score_{};          // X wins, O wins, draws
    int turn_ = 0;                        // 0 = X to move, 1 = O to move
    int winner_ = kEmpty;                 // kX, kO, 3 = draw, kEmpty = still playing
    int round_ = 1;
    int movesThisRound_ = 0;
    int totalMoves_ = 0;
    int lastMove_ = -1;
    double clock_ = 0;                    // seconds since the game started
    double wonAt_ = 0;
};

CGE_EXPORT_GAME(TicTacToe, {
    "Tic-Tac-Toe",
    "The classic 3x3 game for two players. Get three in a row to win. Click a square or press 1-9.",
    kWidth, kHeight,
    2, 2,
    30
})
