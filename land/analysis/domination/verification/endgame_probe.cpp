// Adapter to the selected endgame kernel. verify_endgame.py computes expected
// state transitions and pure maximin values using the original Game.step.
#define LAND_TEST
#define TELEPORT_TRACE 0
#define HOME_RESCUE_TRACE 0
#define ENDGAME_TRACE 0
#ifndef BOT_SOURCE
#error Compile with BOT_SOURCE pointing to the single-file bot under test.
#endif
#include BOT_SOURCE

static void stateJSON(const land::Bot& bot, const land::Bot::EndState& state) {
    std::cout << "{\"owner\":\"";
    for (int k = 0; k < bot.cells; ++k) {
        int y = k / bot.w; auto bit = std::uint64_t{1} << (k % bot.w);
        bool a = state.owned[0][y] & bit, b = state.owned[1][y] & bit;
        std::cout << (a ? b ? 'X' : '0' : b ? '1' : '.');
    }
    std::cout << "\",\"trail\":\"";
    for (int k = 0; k < bot.cells; ++k) {
        int y = k / bot.w; auto bit = std::uint64_t{1} << (k % bot.w);
        bool a = state.drawn[0][y] & bit, b = state.drawn[1][y] & bit;
        std::cout << (a ? b ? 'X' : '0' : b ? '1' : '.');
    }
    std::cout << "\",\"pos\":[" << state.pos[0] << ',' << state.pos[1]
              << "],\"dir\":[\"" << land::DIRS[state.dir[0]] << "\",\"" << land::DIRS[state.dir[1]]
              << "\"],\"outside\":[" << state.outside[0] << ',' << state.outside[1]
              << "],\"area\":[" << state.area[0] << ',' << state.area[1] << "]}";
}

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    int cases;
    if (!(std::cin >> cases)) return 1;
    for (int n = 0; n < cases; ++n) {
        std::string token, id;
        int flags, depth, w, h, turns, players, me, moveMs, initMs;
        std::cin >> token >> id >> flags >> depth;
        std::cin >> token >> w >> h >> turns >> players >> me >> moveMs >> initMs;
        if (players != 2) return 2;
        std::array<int, 4> spawn{};
        for (int i = 0; i < players; ++i) {
            int player, x, y; std::cin >> token >> player >> x >> y;
            spawn[player] = y * w + x;
        }
        std::cin >> token;
        std::array<int, 4> initial{};
        for (int i = 0; i < players; ++i) {
            char d; std::cin >> d;
            initial[i] = int(std::string(land::DIRS).find(d));
        }
        auto bot = std::make_unique<land::Bot>(w, h, players, me, turns, spawn);
        bot->p[me].dir = initial[me];
        bot->prepareSpawn();
        bot->endInitialDirections = initial;
        if (!bot->read()) return 3;
        auto state = bot->endState();
        std::cout << "{\"id\":\"" << id << '\"';
        if (flags & 1) {
            std::cout << ",\"matrix\":[";
            bool comma = false;
            for (int d = 0; d < 4; ++d) if (d != (state.dir[me] ^ 1))
                for (int z = 0; z < 4; ++z) if (z != (state.dir[1 - me] ^ 1)) {
                    if (comma) std::cout << ',';
                    comma = true;
                    std::cout << "{\"mine\":\"" << land::DIRS[d] << "\",\"enemy\":\"" << land::DIRS[z]
                              << "\",\"after\":";
                    stateJSON(*bot, bot->endStep(state, d, z));
                    std::cout << '}';
                }
            std::cout << ']';
        }
        if (flags & 2) {
            // Small-depth value checks disable the production deadline through
            // the public clock anchor; the separate tests below force aborts.
            bot->started = std::chrono::steady_clock::now() + std::chrono::hours(1);
            bot->endNodes = 0; bot->endAborted = false;
            int value = bot->endSolve(state, depth, -land::INF, land::INF);
            std::cout << ",\"depth\":" << depth << ",\"value\":" << value
                      << ",\"nodes\":" << bot->endNodes << ",\"aborted\":" << bot->endAborted
                      << ",\"choices\":{";
            bot->turn = bot->turns - depth;
            bool comma = false;
            for (int d = 0; d < 4; ++d) if (d != (state.dir[me] ^ 1)) {
                if (comma) std::cout << ',';
                comma = true;
                int chosen = bot->exactEndgame(d);
                std::cout << '\"' << land::DIRS[d] << "\":{\"chosen\":\"" << land::DIRS[chosen]
                          << "\",\"aborted\":" << bot->endAborted << '}';
            }
            std::cout << '}';
        }
        if (flags & 4) {
            // Force the existing recursive timer guard at its next checkpoint.
            bot->started = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            bot->endNodes = 63; bot->endAborted = false;
            (void)bot->endSolve(state, 2, -land::INF, land::INF);
            std::cout << ",\"recursive_abort\":" << bot->endAborted << ",\"expired_choices\":[";
            bool comma = false;
            for (int testDepth : {1, 3, 8}) {
                bot->turn = bot->turns - testDepth;
                for (int d = 0; d < 4; ++d) if (d != (state.dir[me] ^ 1)) {
                    if (comma) std::cout << ',';
                    comma = true;
                    bot->endAborted = false;
                    int chosen = bot->exactEndgame(d);
                    std::cout << "{\"depth\":" << testDepth << ",\"original\":\"" << land::DIRS[d]
                              << "\",\"chosen\":\"" << land::DIRS[chosen] << "\",\"aborted\":" << bot->endAborted << '}';
                }
            }
            std::cout << "],\"midsearch_choices\":[";
            comma = false;
            bot->turn = bot->turns - 8;
            for (int d = 0; d < 4; ++d) if (d != (state.dir[me] ^ 1)) {
                if (comma) std::cout << ',';
                comma = true;
                bot->started = std::chrono::steady_clock::now() - std::chrono::milliseconds(41);
                bot->endNodes = 0; bot->endAborted = false;
                int chosen = bot->exactEndgame(d);
                std::cout << "{\"original\":\"" << land::DIRS[d] << "\",\"chosen\":\"" << land::DIRS[chosen]
                          << "\",\"aborted\":" << bot->endAborted << ",\"nodes\":" << bot->endNodes << '}';
            }
            std::cout << ']';
        }
        std::cout << "}\n";
    }
}
