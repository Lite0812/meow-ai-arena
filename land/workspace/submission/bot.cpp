// 穹界喵: bounded loop search, safe return alternatives, and tactical interception.
// Prefix-return search combined with the audited master strategy.
// N4 expansion and interception share a first-place area potential.
// Home rescue: exact simultaneous one-turn guarantees for collision-blocked returns.
// Tactical respawn interception: legal self-elimination preserves territory.
// Derived from mohen2006's MIT "墨痕喵" / GPT's Appetite, referencing the repository's
// Sonnet, Astra and Opus bots. Fixed-work search by the current Arena work.
// Added here: exact own-trail suicide simulation and committed respawn cuts.
// Strategy references are recorded in the submission's meta.json and NOTES.md.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef RETURN_ANCHOR_TRACE
#define RETURN_ANCHOR_TRACE 0
#endif
#ifndef PREFIX_RETURN_TRACE
#define PREFIX_RETURN_TRACE 1
#endif
#ifndef TELEPORT_TRACE
#define TELEPORT_TRACE 1
#endif
#ifndef TELEPORT_SCORE_MULT
#define TELEPORT_SCORE_MULT 1.10
#endif
#ifndef ENCLOSURE_PENALTY
#define ENCLOSURE_PENALTY 1.8
#endif
#ifndef GATE_PENALTY
#define GATE_PENALTY 0.65
#endif
#ifndef HOME_RESCUE_TRACE
#define HOME_RESCUE_TRACE 1
#endif
#ifndef MELEE_ENCLOSURE_DISCOUNT
#define MELEE_ENCLOSURE_DISCOUNT 0.5
#endif
#ifndef ENDGAME_TRACE
#define ENDGAME_TRACE 1
#endif
namespace land {
constexpr int MAX_CELLS = 48 * 48;
constexpr int INF = 10000;
constexpr int DX[4] = {0, 0, -1, 1};
constexpr int DY[4] = {-1, 1, 0, 0};
constexpr char DIRS[] = "UDLR";
using Rows = std::array<std::uint64_t, 48>;
using Path = std::vector<int>;

struct Player {
    int pos = 0, dir = 0, length = 0, area = 0, deaths = 0;
};

struct Evaluation {
    Path path;
    double gain = 0, score = -1;
    int margin = -INF, length = 0;
    bool closed = false;
};

struct Leg { int dir, length; };
struct Candidate { double estimate; std::array<Leg, 5> legs; int count; };

class Bot {
public:
    int w, h, cells, turns, players, me, turn = 0;
    std::array<Player, 4> p;
    std::array<int, 4> spawn{}, idle{}, lastPos{}, lastDeaths{};
    int owner[MAX_CELLS], trail[MAX_CELLS], protect[MAX_CELLS];
    int next[MAX_CELLS][4], enemy[4][MAX_CELLS], danger[MAX_CELLS];
    int enemyHome[4], enemyParent[4][MAX_CELLS];
    int home[MAX_CELLS], distance[MAX_CELLS], parent[MAX_CELLS], parentDir[MAX_CELLS];
    int seen[MAX_CELLS]{}, stamp = 0;
    int expected = -1, previousDeaths = 0, previousLength = 0;
    int trailDanger = INF;
    Rows ownRows{}, trailRows{}, valueRows[5]{};
    Rows enemyOwned[4]{};
    int oldVulnerable[4]{};
    Rows forecastClaim[4]{};
    int oldGateVulnerable[4]{};
    double weights[5]{}, prefix[49][49]{};
    double denied[4]{};
    double rivalImportance[4]{};
    Rows meleeCovered[4]{};
    std::array<int, 3> meleeRivalOrder{};
    double meleeScale = 70.0, meleeOldRisk = 0;
    std::uint64_t full;
    Path plan;
    int initDirection = -1;
    int spawnDistance[MAX_CELLS], spawnParent[MAX_CELLS], spawnParentDir[MAX_CELLS];
    Rows protectedOwn{}, protectedOther{};
    int teleportPhase = 0, teleportEnemy = -1, teleportEnemyDeaths = 0;
    int teleportOwnDeaths = 0, teleportStarted = 0;
    Path teleportReset;
    struct ResetOption { Path path; double secured = -1; };
    std::uint64_t rng = 0x91e10da5c79e7b1dULL;
    std::chrono::steady_clock::time_point started;

    Bot(int width, int height, int count, int id, int maxTurns,
        const std::array<int, 4>& spawns)
        : w(width), h(height), cells(w * h), turns(maxTurns), players(count),
          me(id), spawn(spawns), full((std::uint64_t{1} << w) - 1) {
        std::fill(owner, owner + cells, -1);
        std::fill(trail, trail + cells, -1);
        std::fill(protect, protect + cells, -1);
        lastPos.fill(-1);
        for (int k = 0; k < cells; ++k) {
            int x = k % w, y = k / w;
            for (int d = 0; d < 4; ++d) {
                int xx = x + DX[d], yy = y + DY[d];
                next[k][d] = xx < 0 || xx >= w || yy < 0 || yy >= h ? -1 : yy * w + xx;
            }
            for (int e = 0; e < players; ++e)
                if (std::abs(x - spawn[e] % w) <= 1 && std::abs(y - spawn[e] / w) <= 1)
                    protect[k] = e;
            if (protect[k] >= 0) {
                Rows& mask = protect[k] == me ? protectedOwn : protectedOther;
                mask[y] |= std::uint64_t{1} << x;
            }
        }
    }

    int manhattan(int a, int b) const {
        return std::abs(a % w - b % w) + std::abs(a / w - b / w);
    }

    bool expired(double milliseconds = 7.0) const {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count() >= 30.0;
    }

    unsigned random() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return static_cast<unsigned>(rng >> 16);
    }

    // Spread through each free horizontal interval, then exchange rows.
    Rows enclosed(const Rows& walls) const {
        Rows reached{};
        std::uint64_t edges = 1 | (std::uint64_t{1} << (w - 1));
        for (int y = 0; y < h; ++y)
            reached[y] = (y == 0 || y + 1 == h ? full : edges) & ~walls[y];
        bool changed = true;
        while (changed) {
            changed = false;
            for (int sweep = 0; sweep < 2; ++sweep) {
                for (int j = 0; j < h; ++j) {
                    int y = sweep ? h - 1 - j : j;
                    std::uint64_t free = full & ~walls[y];
                    std::uint64_t seeds = reached[y];
                    if (y) seeds |= reached[y - 1];
                    if (y + 1 < h) seeds |= reached[y + 1];
                    seeds &= free;
                    std::uint64_t left = seeds, right = seeds, a = free, b = free;
                    for (int shift = 1; shift < w; shift *= 2) {
                        left |= a & (left << shift); a &= a << shift;
                        right |= b & (right >> shift); b &= b >> shift;
                    }
                    std::uint64_t result = (left | right) & free;
                    changed |= result != reached[y];
                    reached[y] = result;
                }
            }
        }
        Rows captured{};
        for (int y = 0; y < h; ++y) captured[y] = full & ~reached[y];
        return captured;
    }

    Rows widened(const Rows& walls) const {
        Rows out{};
        for (int y = 0; y < h; ++y) {
            out[y] = walls[y] | (walls[y] << 1) | (walls[y] >> 1);
            if (y) out[y] |= walls[y - 1];
            if (y + 1 < h) out[y] |= walls[y + 1];
            out[y] &= full;
        }
        return out;
    }

    bool collision(int d) const {
        int start = p[me].pos, target = next[start][d];
        if (target < 0) return true;
        for (int e = 0; e < players; ++e) if (e != me && idle[e] < 3) {
            for (int z = 0; z < 4; ++z) if (z != (p[e].dir ^ 1)) {
                int v = next[p[e].pos][z];
                if (v >= 0 && (v == target || (v == start && target == p[e].pos))) return true;
            }
        }
        return false;
    }

    void fields() {
        std::fill(danger, danger + cells, INF);
        if (players == 4) {
            double sum = 0;
            meleeScale = std::max(70.0, (turns - turn) * 0.3);
            int maxRival = 0, count = 0;
            for (int e = 0; e < players; ++e) if (e != me) maxRival = std::max(maxRival, p[e].area);
            for (int e = 0; e < players; ++e) if (e != me) {
                rivalImportance[e] = std::exp((p[e].area - maxRival) / meleeScale);
                sum += rivalImportance[e];
                meleeRivalOrder[count++] = e;
            }
            for (int e = 0; e < players; ++e) if (e != me) rivalImportance[e] /= sum;
            std::sort(meleeRivalOrder.begin(), meleeRivalOrder.end(), [&](int a, int b) {
                return rivalImportance[a] > rivalImportance[b];
            });
        }
        for (int e = 0; e < players; ++e) if (e != me) {
            idle[e] = p[e].pos == lastPos[e] && p[e].deaths == lastDeaths[e]
                      && p[e].pos == spawn[e] ? idle[e] + 1 : 0;
            lastPos[e] = p[e].pos; lastDeaths[e] = p[e].deaths;
            enemyHome[e] = p[e].length ? INF : 0;
            denied[e] = 0;
            forecastClaim[e].fill(0);
            std::fill(enemy[e], enemy[e] + cells, INF);
            if (idle[e] >= 3) continue;

            // Mode 1 permits crossing old trail after the opponent returns home.
            int dist[2 * MAX_CELLS], q[2 * MAX_CELLS], head = 0, tail = 0;
            std::fill(dist, dist + 2 * cells, INF);
            int start = p[e].pos, mode = p[e].length == 0;
            dist[start + mode * cells] = 0;
            for (int d = 0; d < 4; ++d) if (d != (p[e].dir ^ 1)) {
                int v = next[start][d];
                if (v < 0 || (!mode && trail[v] == e)) continue;
                int state = v + (mode || owner[v] == e) * cells;
                if (dist[state] != INF) continue;
                dist[state] = 1; q[tail++] = state; enemyParent[e][v] = start;
            }
            int goal = -1;
            while (head < tail) {
                int state = q[head++], u = state % cells, clear = state >= cells;
                if (goal < 0 && p[e].length && owner[u] == e) {
                    goal = u; enemyHome[e] = dist[state];
                }
                for (int d = 0; d < 4; ++d) {
                    int v = next[u][d];
                    if (v < 0 || (!clear && trail[v] == e)) continue;
                    int ns = v + (clear || owner[v] == e) * cells;
                    if (dist[ns] != INF) continue;
                    dist[ns] = dist[state] + 1; q[tail++] = ns;
                    if (!clear) enemyParent[e][v] = u;
                }
            }
            for (int k = 0; k < cells; ++k) {
                enemy[e][k] = std::min(dist[k], dist[k + cells]);
                danger[k] = std::min(danger[k], enemy[e][k]);
            }
            if (goal >= 0) {
                Rows walls{};
                for (int k = 0; k < cells; ++k)
                    if (owner[k] == e || trail[k] == e) walls[k / w] |= std::uint64_t{1} << (k % w);
                int steps = 0;
                for (int k = goal; k != start && steps++ < cells; k = enemyParent[e][k])
                    walls[k / w] |= std::uint64_t{1} << (k % w);
                if (steps >= cells) continue;
                Rows captured = enclosed(walls);
                forecastClaim[e] = captured;
                for (int k = 0; k < cells; ++k) {
                    if (!(captured[k / w] & (std::uint64_t{1} << (k % w))) || owner[k] == e
                        || (protect[k] >= 0 && protect[k] != e)) continue;
                    if (players == 2)
                        denied[e] += owner[k] == me ? 2.2 : owner[k] < 0 ? 0.8 : 0.25;
                    else {
                        double value = rivalImportance[e];
                        if (owner[k] == me) value += 1;
                        else if (owner[k] >= 0) value -= rivalImportance[owner[k]];
                        denied[e] += value;
                    }
                }
            }
        }
        trailDanger = INF;
        ownRows.fill(0); trailRows.fill(0);
        for (auto& rows : valueRows) rows.fill(0);
        int leader = me;
        for (int e = 0; e < players; ++e) if (p[e].area > p[leader].area) leader = e;
        weights[0] = 1;
        for (int e = 0; e < players; ++e)
            weights[e + 1] = e == me ? 0 : players == 2 ? 2.4 : 1.0 + rivalImportance[e];
        for (int k = 0; k < cells; ++k) {
            std::uint64_t bit = std::uint64_t{1} << (k % w);
            if (owner[k] == me) ownRows[k / w] |= bit;
            if (trail[k] == me) {
                trailRows[k / w] |= bit; trailDanger = std::min(trailDanger, danger[k]);
            }
            if (owner[k] != me && (protect[k] < 0 || protect[k] == me))
                valueRows[owner[k] + 1][k / w] |= bit;
        }
        for (auto& rows : enemyOwned) rows.fill(0);
        for (int k = 0; k < cells; ++k) if (owner[k] >= 0 && owner[k] != me)
            enemyOwned[owner[k]][k / w] |= std::uint64_t{1} << (k % w);
        for (int e = 0; e < players; ++e) if (e != me) {
            Rows reach = enclosed(enemyOwned[e]);
            if (players == 4) { meleeCovered[e] = reach; continue; }
            oldVulnerable[e] = 0;
            for (int y = 0; y < h; ++y) oldVulnerable[e] += std::popcount(ownRows[y] & ~protectedOwn[y] & reach[y]);
            Rows wide = enclosed(widened(enemyOwned[e]));
            oldGateVulnerable[e] = 0;
            for (int y = 0; y < h; ++y)
                oldGateVulnerable[e] += std::popcount(ownRows[y] & ~protectedOwn[y] & wide[y] & ~reach[y]);
        }
        if (players == 4) meleeOldRisk = meleeCaptureRisk(ownRows, false);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int k = y * w + x;
            double value = protect[k] < 0 || protect[k] == me ? weights[owner[k] + 1] : 0;
            prefix[y + 1][x + 1] = value + prefix[y][x + 1] + prefix[y + 1][x] - prefix[y][x];
        }
        int q[MAX_CELLS], a = 0, b = 0;
        std::fill(home, home + cells, INF);
        for (int k = 0; k < cells; ++k) if (owner[k] == me && trail[k] != me) {
            home[k] = 0; q[b++] = k;
        }
        while (a < b) {
            int u = q[a++];
            for (int d = 0; d < 4; ++d) {
                int v = next[u][d];
                if (v < 0 || trail[v] == me || home[v] != INF) continue;
                home[v] = home[u] + 1; q[b++] = v;
            }
        }
    }

    // F = A_me - tau * log(sum(exp(A_enemy / tau))). Its cell gradients
    // are 1 for neutral land and 1 + rivalImportance[owner] for a capture.
    // Evaluate finite captures exactly so a large hit cannot keep receiving
    // the old leader's full marginal weight after another rival overtakes it.
    double meleeCaptureValue(const Rows& captured) const {
        int added = 0;
        for (int y = 0; y < h; ++y) added += std::popcount(captured[y] & valueRows[0][y]);
        double residual = 0;
        for (int e = 0; e < players; ++e) if (e != me) {
            int lost = 0;
            for (int y = 0; y < h; ++y) lost += std::popcount(captured[y] & valueRows[e + 1][y]);
            added += lost;
            residual += rivalImportance[e] * std::exp(-lost / meleeScale);
        }
        return added - meleeScale * std::log(residual);
    }

    // One cell can be reclaimed only once: among enemies whose existing
    // territory encloses it, use the largest current first-place loss weight.
    // Removing enemy walls can only shrink their enclosed masks, allowing the
    // cached before-capture flood to exclude irrelevant post-capture floods.
    double meleeCaptureRisk(const Rows& territory, bool afterCapture) const {
        Rows counted{};
        double risk = 0;
        for (int e : meleeRivalOrder) if (idle[e] < 3) {
            bool relevant = false, removed = false;
            for (int y = 0; y < h; ++y) {
                relevant |= (territory[y] & ~protectedOwn[y] & ~counted[y] & meleeCovered[e][y]) != 0;
                removed |= (territory[y] & enemyOwned[e][y]) != 0;
            }
            if (!relevant) continue;
            Rows covered = meleeCovered[e];
            if (afterCapture && removed) {
                Rows remaining{};
                for (int y = 0; y < h; ++y) remaining[y] = enemyOwned[e][y] & ~territory[y];
                covered = enclosed(remaining);
            }
            int threatened = 0;
            for (int y = 0; y < h; ++y) {
                std::uint64_t mask = territory[y] & ~protectedOwn[y] & ~counted[y] & covered[y];
                threatened += std::popcount(mask); counted[y] |= mask;
            }
            risk += (1.0 + rivalImportance[e]) * threatened;
        }
        return risk;
    }

    double gain(const Rows& walls, int duration = 0) const {
        Rows captured = enclosed(walls);
        if (players == 4) {
            double result = meleeCaptureValue(captured);
            if (MELEE_ENCLOSURE_DISCOUNT > 0 && duration < turns - turn) {
                Rows territory{};
                for (int y = 0; y < h; ++y) territory[y] = captured[y] & ~protectedOther[y];
                // With discount 0.5 and 1 + pi <= 2, new-cell risk costs at
                // most its unit acquisition value. Breaking a ring releases
                // old risk. Simultaneous future enemy closures remain unknown.
                result -= MELEE_ENCLOSURE_DISCOUNT * (meleeCaptureRisk(territory, true) - meleeOldRisk);
            }
            return result;
        }
        double result = 0;
        for (int e = 0; e <= players; ++e) if (weights[e])
            for (int y = 0; y < h; ++y)
                result += weights[e] * std::popcount(captured[y] & valueRows[e][y]);
        if (players == 2 && ENCLOSURE_PENALTY > 0) {
            int e = 1 - me;
            Rows remaining{};
            for (int y = 0; y < h; ++y)
                remaining[y] = enemyOwned[e][y] & ~(captured[y] & ~protectedOther[y]);
            Rows covered = enclosed(remaining);
            int vulnerable = 0;
            for (int y = 0; y < h; ++y)
                vulnerable += std::popcount(captured[y] & ~protectedOther[y] & ~protectedOwn[y] & covered[y]);
            result -= ENCLOSURE_PENALTY * (vulnerable - oldVulnerable[e]);
            Rows wide = enclosed(widened(remaining));
            int narrow = 0;
            for (int y = 0; y < h; ++y)
                narrow += std::popcount(captured[y] & ~protectedOther[y] & ~protectedOwn[y] & wide[y] & ~covered[y]);
            result -= GATE_PENALTY * (narrow - oldGateVulnerable[e]);
        }
        return result;
    }

    // An opponent's earlier close can remove an existing wall of our loop.
    // Only apply this while already outside: then our new trace cannot contain
    // an interior navigation prefix whose ownership changes before departure.
    double guardLoopWalls(const Rows& walls, const Path& path, int length, double value) const {
        if (players != 2 || !p[me].length || turns - turn > 70) return value;
        int foe = 1 - me;
        if (!p[foe].length || enemyHome[foe] >= length || enemyHome[foe] < 1) return value;
        int u = p[me].pos, step = 0;
        for (int d : path) {
            u = next[u][d];
            if (++step > enemyHome[foe]) break;
            // A guaranteed old-trail cut prevents the forecast close.
            if (trail[u] == foe) return value;
        }
        Rows remainingWalls{}; bool changed = false;
        for (int y = 0; y < h; ++y) {
            std::uint64_t removed = ownRows[y] & forecastClaim[foe][y] & ~protectedOwn[y];
            changed |= removed != 0;
            remainingWalls[y] = (walls[y] & ~removed) | trailRows[y];
        }
        if (!changed) return value;
        Rows present = enclosed(walls), after = enclosed(remainingWalls);
        double lostOpportunity = 0;
        for (int i = 0; i <= players; ++i) if (weights[i])
            for (int y = 0; y < h; ++y)
                lostOpportunity += weights[i] * std::popcount(present[y] & ~after[y] & valueRows[i][y]);
        return value - lostOpportunity;
    }

    double rate(double value, int length, int margin) const {
        double probability = margin >= 1 ? 1 : margin == 0 ? 0.65 : margin >= -2 ? 0.20 : 0.01;
        if (turns - turn <= 25) return value * probability / (1 + length * 0.008);
        return value * probability / (length + 18.0);
    }

    Evaluation evaluate(const Path& path, bool exact = true) {
        Evaluation e;
        if (path.empty()) return e;
        if (exact) e.path.reserve(path.size());
        ++stamp;
        Rows walls{};
        for (int y = 0; y < h; ++y) walls[y] = ownRows[y] | trailRows[y];
        int u = p[me].pos, heading = p[me].dir, risk = trailDanger;
        bool outside = p[me].length > 0;
        int minx = u % w, maxx = minx, miny = u / w, maxy = miny;
        for (int d : path) {
            if (d == (heading ^ 1) || next[u][d] < 0) return e;
            u = next[u][d]; heading = d;
            if (trail[u] == me || seen[u] == stamp) return e;
            if (exact) e.path.push_back(d);
            ++e.length;
            if (e.length > turns - turn || e.length > 180) return e;
            if (e.length == 1 && collision(d)) return e;
            minx = std::min(minx, u % w); maxx = std::max(maxx, u % w);
            miny = std::min(miny, u / w); maxy = std::max(maxy, u / w);
            if (owner[u] == me) {
                if (outside) {
                    if (players == 2) {
                        int foe = 1 - me;
                        if (p[foe].length && enemyHome[foe] < e.length && protect[u] != me
                            && (forecastClaim[foe][u / w] & (std::uint64_t{1} << (u % w)))) return e;
                    }
                    e.closed = true; break;
                }
            } else {
                outside = true; seen[u] = stamp;
                walls[u / w] |= std::uint64_t{1} << (u % w);
                risk = std::min(risk, danger[u]);
            }
        }
        if (!e.closed) return e;
        e.margin = risk - e.length;
        e.gain = exact ? gain(walls, e.length) : prefix[maxy + 1][maxx + 1] - prefix[miny][maxx + 1]
                     - prefix[maxy + 1][minx] + prefix[miny][minx];
        if (exact) e.gain = guardLoopWalls(walls, e.path, e.length, e.gain);
        e.score = rate(e.gain, e.length, e.margin);
        return e;
    }

    Path pathFromLegs(const Candidate& c) const {
        Path path;
        path.reserve(160);
        int u = p[me].pos;
        bool outside = p[me].length > 0;
        for (int i = 0; i < c.count; ++i) {
            int d = c.legs[i].dir, count = c.legs[i].length;
            for (int j = 0; j < (count < 0 ? std::max(w, h) : count); ++j) {
                u = next[u][d];
                if (u < 0) return {};
                path.push_back(d);
                if (owner[u] == me && outside) return path;
                if (owner[u] != me) outside = true;
            }
        }
        return path;
    }

    Path returnPath(const Path& prefixPath = {}, bool guardHome = false) {
        ++stamp;
        int u = p[me].pos, heading = p[me].dir;
        bool outside = p[me].length > 0;
        int prefixLength = 0, filteredHomes = 0;
        auto unsafeHome = [&](int cell, int arrival) {
            if (!guardHome || !outside || players != 2 || protect[cell] == me) return false;
            int foe = 1 - me;
            return p[foe].length && enemyHome[foe] < arrival
                && (forecastClaim[foe][cell / w] & (std::uint64_t{1} << (cell % w)));
        };
        for (int d : prefixPath) {
            if (d == (heading ^ 1) || next[u][d] < 0) return {};
            u = next[u][d]; heading = d; ++prefixLength;
            if (trail[u] == me || seen[u] == stamp) return {};
            if (owner[u] == me && outside) {
                if (unsafeHome(u, prefixLength)) return {};
                return prefixPath;
            }
            if (owner[u] != me) seen[u] = stamp, outside = true;
        }
        int dist[MAX_CELLS], par[MAX_CELLS], pd[MAX_CELLS], q[MAX_CELLS], a = 0, b = 0;
        std::fill(dist, dist + cells, INF);
        int start = u;
        dist[start] = 0;
        for (int d = 0; d < 4; ++d) if (d != (heading ^ 1)) {
            int v = next[start][d];
            if (v < 0 || trail[v] == me || seen[v] == stamp || (prefixPath.empty() && collision(d))) continue;
            dist[v] = 1; par[v] = start; pd[v] = d; q[b++] = v;
        }
        int goal = -1;
        while (a < b) {
            u = q[a++];
            if (owner[u] == me) {
                // A current owned cell is a first-return terminal. If its
                // forecast is unsafe, block it; never search through it.
                if (unsafeHome(u, prefixLength + dist[u])) { ++filteredHomes; continue; }
                goal = u; break;
            }
            int dirs[4] = {0, 1, 2, 3};
            std::sort(dirs, dirs + 4, [&](int d, int e) {
                int v = next[u][d], z = next[u][e];
                return (v < 0 ? -INF : danger[v]) > (z < 0 ? -INF : danger[z]);
            });
            for (int d : dirs) {
                int v = next[u][d];
                if (v < 0 || dist[v] != INF || trail[v] == me || seen[v] == stamp) continue;
                dist[v] = dist[u] + 1; par[v] = u; pd[v] = d; q[b++] = v;
            }
        }
        if (goal < 0) return {};
#if RETURN_ANCHOR_TRACE
        if (guardHome && filteredHomes)
            std::cerr << "RETURN anchor t=" << turn << " skipped=" << filteredHomes
                      << " length=" << prefixLength + dist[goal] << " target=" << goal << '\n';
#endif
        Path suffix;
        for (int k = goal; k != start; k = par[k]) suffix.push_back(pd[k]);
        std::reverse(suffix.begin(), suffix.end());
        Path result = prefixPath;
        result.insert(result.end(), suffix.begin(), suffix.end());
        return result;
    }

    // Add a bounded family after the complete original expansion search.
    // The prefix estimate only orders work. Existing exact evaluation remains
    // authoritative for closure, trail safety, future home and stable gain.
    void improvePrefixReturns(Evaluation& best) {
        if (players != 2 || expired() || turns - turn < 3) return;
        constexpr int LIMIT = 96;
        const int lengths[] = {1, 2, 3, 5, 8, 12, 16, 22, 28, 36};
        const int thirdLengths[] = {1, 2, 3, 5, 8, 12, 16, 22};
        int origin = p[me].pos;
        int oldMinX = origin % w, oldMaxX = oldMinX;
        int oldMinY = origin / w, oldMaxY = oldMinY;
        // Existing exposed trail also contributes to the closure's footprint.
        for (int k = 0; k < cells; ++k) if (trail[k] == me) {
            oldMinX = std::min(oldMinX, k % w); oldMaxX = std::max(oldMaxX, k % w);
            oldMinY = std::min(oldMinY, k / w); oldMaxY = std::max(oldMaxY, k / w);
        }
        std::vector<Candidate> ranked;
        ranked.reserve(LIMIT);
        auto lowestFirst = [](const Candidate& a, const Candidate& b) {
            return a.estimate > b.estimate;
        };
        int screened = 0, eligible = 0;
        auto considerPrefix = [&](Candidate c) {
            ++screened; ++stamp;
            int u = origin, heading = p[me].dir, length = 0, risk = trailDanger;
            int minx = oldMinX, maxx = oldMaxX, miny = oldMinY, maxy = oldMaxY;
            bool outside = p[me].length > 0;
            for (int leg = 0; leg < c.count; ++leg) {
                int d = c.legs[leg].dir;
                if (d == (heading ^ 1)) return;
                for (int step = 0; step < c.legs[leg].length; ++step) {
                    u = next[u][d]; ++length;
                    if (u < 0 || trail[u] == me || seen[u] == stamp) return;
                    if (length > 180 || length > turns - turn) return;
                    minx = std::min(minx, u % w); maxx = std::max(maxx, u % w);
                    miny = std::min(miny, u / w); maxy = std::max(maxy, u / w);
                    if (owner[u] == me) {
                        if (outside) return; // Already closed: no BFS extension needed.
                    } else {
                        outside = true; seen[u] = stamp;
                        risk = std::min(risk, danger[u]);
                    }
                }
                heading = d;
            }
            if (!outside || home[u] >= INF) return;
            // home ignores the new prefix and heading, so it is a lower bound.
            int optimisticLength = length + home[u];
            if (optimisticLength > 180 || optimisticLength > turns - turn
                || risk - optimisticLength < 1) return;
            double value = prefix[maxy + 1][maxx + 1] - prefix[miny][maxx + 1]
                         - prefix[maxy + 1][minx] + prefix[miny][minx];
            if (value <= 0) return;
            c.estimate = rate(value, optimisticLength, risk - optimisticLength);
            ++eligible;
            if (ranked.size() < LIMIT) {
                ranked.push_back(c);
                std::push_heap(ranked.begin(), ranked.end(), lowestFirst);
            } else if (c.estimate > ranked.front().estimate) {
                std::pop_heap(ranked.begin(), ranked.end(), lowestFirst);
                ranked.back() = c;
                std::push_heap(ranked.begin(), ranked.end(), lowestFirst);
            }
        };
        for (int a : lengths) {
            for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1) && !collision(d)) {
                int x = origin % w + DX[d] * a, y = origin / w + DY[d] * a;
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                for (int side = 0; side < 4; ++side) if ((d < 2) != (side < 2)) {
                    for (int b : lengths) {
                        if (expired()) break;
                        int xx = x + DX[side] * b, yy = y + DY[side] * b;
                        if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                        Candidate c{}; c.count = 2;
                        c.legs[0] = {d, a}; c.legs[1] = {side, b};
                        considerPrefix(c);
                        c.count = 3;
                        for (int z : thirdLengths) for (int last : {d, d ^ 1}) {
                            int xxx = xx + DX[last] * z, yyy = yy + DY[last] * z;
                            if (xxx < 0 || xxx >= w || yyy < 0 || yyy >= h) continue;
                            c.legs[2] = {last, z};
                            considerPrefix(c);
                        }
                    }
                    if (expired()) break;
                }
            }
            if (expired()) break;
        }
        std::sort(ranked.begin(), ranked.end(), lowestFirst);
        std::unordered_set<std::uint64_t> unique;
        unique.reserve(LIMIT * 2);
        int attempted = 0, exact = 0;
        double before = best.score;
        for (const Candidate& c : ranked) {
            if (expired()) break;
            Path prefixPath = pathFromLegs(c);
            if (prefixPath.empty()) continue;
            ++attempted;
            Path complete = returnPath(prefixPath, true);
            if (complete.empty()) continue;
            std::uint64_t hash = 14695981039346656037ULL;
            for (int d : complete) hash = (hash ^ static_cast<unsigned>(d + 1)) * 1099511628211ULL;
            if (!unique.insert(hash).second) continue;
            Evaluation quick = evaluate(complete, false);
            if (!quick.closed || quick.margin < 1) continue;
            if (expired()) break;
            ++exact;
            Evaluation e = evaluate(complete);
            if (e.closed && e.margin >= 1 && e.score > best.score) best = std::move(e);
        }
#if PREFIX_RETURN_TRACE
        if (best.score > before)
            std::cerr << "PREFIX return t=" << turn << " old=" << before << " score=" << best.score
                      << " gain=" << best.gain << " length=" << best.length << " margin=" << best.margin
                      << " screened=" << screened << " eligible=" << eligible
                      << " bfs=" << attempted << " exact=" << exact << '\n';
#endif
    }

    Evaluation expansion() {
        Evaluation best = evaluate(plan);
        if (best.margin < 1) best = Evaluation{};
        if (best.closed) best.score *= 1.025;
        Evaluation fast = evaluate(returnPath({}, true));
        if ((fast.margin >= 1) && fast.score > best.score) best = std::move(fast);
        std::vector<Candidate> candidates;
        candidates.reserve(5000);
        std::unordered_set<std::uint64_t> unique;
        if (players == 4) unique.reserve(4000);
        auto consider = [&](Candidate c) {
            Path path = pathFromLegs(c);
            Evaluation e = evaluate(path, false);
            if (e.closed && e.margin >= 1 && e.gain > 0) {
                if (players == 4) {
                    std::uint64_t hash = 14695981039346656037ULL;
                    for (int d : path) hash = (hash ^ static_cast<unsigned>(d + 1)) * 1099511628211ULL;
                    if (!unique.insert(hash).second) return;
                }
                c.estimate = e.score;
                candidates.push_back(c);
            }
        };
        const int depths[] = {1, 3, 5, 8, 12, 16, 22, 28, 36, 44, 2, 4, 6, 10, 14, 18, 24, 32, 40};
        for (int a : depths) {
            for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1)) {
                int x = p[me].pos % w + DX[d] * a, y = p[me].pos / w + DY[d] * a;
                if (x < 0 || x >= w || y < 0 || y >= h) continue;
                for (int side = 0; side < 4; ++side) if ((d < 2) != (side < 2)) {
                    Candidate c{}; c.count = 2; c.legs[0] = {d, a}; c.legs[1] = {side, -1};
                    consider(c);
                    for (int b = 1; b < std::max(w, h); ++b) {
                        int xx = x + DX[side] * b, yy = y + DY[side] * b;
                        if (xx < 0 || xx >= w || yy < 0 || yy >= h) break;
                        c.count = 3; c.legs[1] = {side, b}; c.legs[2] = {d ^ 1, -1};
                        consider(c);
                        // A fourth side can close on a different part of an irregular frontier.
                        if (a <= 24 && b <= 28) {
                            c.count = 4; c.legs[2] = {d ^ 1, a}; c.legs[3] = {side ^ 1, -1};
                            consider(c);
                        }
                    }
                }
            }
            if (expired(3.0)) break;
        }
        int count = std::min(280, static_cast<int>(candidates.size()));
        std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.estimate > b.estimate; });
        Candidate champion{};
        if (players == 2 && best.closed) {
            for (int d : best.path) {
                if (champion.count && champion.legs[champion.count - 1].dir == d)
                    ++champion.legs[champion.count - 1].length;
                else if (champion.count < 5)
                    champion.legs[champion.count++] = {d, 1};
                else { champion.count = 0; break; }
            }
            if (champion.count) champion.legs[champion.count - 1].length = -1;
        }
        for (int i = 0; i < count; ++i) {
            if (i > 0 && expired(5.0)) break;
            Evaluation e = evaluate(pathFromLegs(candidates[i]));
            if (e.score > best.score) { best = std::move(e); champion = candidates[i]; }
        }
        // A small bounded refinement handles staircases without searching game trees.
        for (int iteration = 0; iteration < 1200 && !expired(); ++iteration) {
            Candidate c{};
            if (champion.count > 1 && random() % 3) {
                c = champion;
                int j = random() % (c.count - 1);
                c.legs[j].length += static_cast<int>(random() % 9) - 4;
                if (c.legs[j].length < 1) continue;
            } else {
                c.count = 3 + random() % 3;
                int d = random() % 4;
                if (d == (p[me].dir ^ 1)) d = p[me].dir;
                for (int j = 0; j < c.count; ++j) {
                    c.legs[j] = {d, j + 1 == c.count ? -1 : 1 + static_cast<int>(random() % 24)};
                    d = (d < 2 ? 2 : 0) + random() % 2;
                }
            }
            Path path = pathFromLegs(c);
            Evaluation quick = evaluate(path, false);
            if (!quick.closed || quick.margin < 1 || quick.score < best.score * 0.6) continue;
            Evaluation e = evaluate(path);
            if (e.score > best.score) { best = std::move(e); champion = c; }
        }
        improvePrefixReturns(best);
        return best;
    }

    std::pair<Path, double> attack() {
        int q[MAX_CELLS], a = 0, b = 0, start = p[me].pos;
        std::fill(distance, distance + cells, INF);
        distance[start] = 0;
        for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1) && !collision(d)) {
            int v = next[start][d];
            if (v < 0 || trail[v] == me) continue;
            distance[v] = 1; parent[v] = start; parentDir[v] = d; q[b++] = v;
        }
        while (a < b) {
            int u = q[a++];
            if (distance[u] >= 24) continue;
            for (int d = 0; d < 4; ++d) {
                int v = next[u][d];
                if (v < 0 || trail[v] == me || distance[v] != INF) continue;
                distance[v] = distance[u] + 1; parent[v] = u; parentDir[v] = d; q[b++] = v;
            }
        }
        Path best;
        double bestScore = -1;
        for (int e = 0; e < players; ++e) if (e != me && p[e].length) {
            int target = -1, length = INF;
            for (int k = 0; k < cells; ++k) if (trail[k] == e && distance[k] < length) {
                target = k; length = distance[k];
            }
            int pursuit = players == 4 ? 4 : 0;
            if (target < 0 || length > enemyHome[e] + pursuit || length > 24 || length > turns - turn) continue;
            if (players == 4 && enemyHome[e] > turns - turn) continue;
            Path path;
            for (int k = target; k != start; k = parent[k]) path.push_back(parentDir[k]);
            std::reverse(path.begin(), path.end());
            Path complete = returnPath(path);
            if (complete.empty()) continue;
            bool safe = true;
            auto check = [&](int k) {
                for (int enemyId = 0; enemyId < players; ++enemyId) if (enemyId != me) {
                    int limit = enemyId == e && length <= enemyHome[e] ? length : static_cast<int>(complete.size());
                    if (enemy[enemyId][k] <= limit) return false;
                    if (enemyId == e && length + manhattan(spawn[e], k) <= static_cast<int>(complete.size())) return false;
                }
                return true;
            };
            for (int k = 0; k < cells && safe; ++k) if (trail[k] == me) safe = check(k);
            int u = start;
            for (int d : complete) {
                u = next[u][d];
                if (u < 0) { safe = false; break; }
                if (owner[u] != me && u != target) safe &= check(u);
            }
            if (!safe) continue;
            double value = players == 2 ? 10 + 0.7 * p[e].length + denied[e]
                                       : rivalImportance[e] * (10 + 0.7 * p[e].length) + std::max(0.0, denied[e]);
            double score = value / (players == 2 && turns - turn <= 25 ? 1.0 + length * 0.008 : length + 9.0);
            if (players == 4) score *= std::min(1.0, (enemyHome[e] + 5.0 - length) / 5.0);
            if (length == 1) score += players == 2 ? 4 : 1;
            if (score > bestScore) bestScore = score, best = std::move(path);
        }
        return {best, bestScore};
    }

    void prepareSpawn() {
        if (initDirection >= 0) return;
        initDirection = p[me].dir;
        int q[MAX_CELLS], a = 0, b = 0, start = spawn[me];
        std::fill(spawnDistance, spawnDistance + cells, INF);
        spawnDistance[start] = 0;
        for (int d = 0; d < 4; ++d) if (d != (initDirection ^ 1)) {
            int v = next[start][d];
            if (v < 0 || spawnDistance[v] != INF) continue;
            spawnDistance[v] = 1; spawnParent[v] = start; spawnParentDir[v] = d; q[b++] = v;
        }
        while (a < b) {
            int u = q[a++];
            for (int d = 0; d < 4; ++d) {
                int v = next[u][d];
                if (v < 0 || spawnDistance[v] != INF) continue;
                spawnDistance[v] = spawnDistance[u] + 1;
                spawnParent[v] = u; spawnParentDir[v] = d; q[b++] = v;
            }
        }
    }

    double securedValue(const Rows& territory) const {
        if (players == 4) return meleeCaptureValue(territory);
        double value = 0;
        for (int e = 0; e <= players; ++e) if (weights[e])
            for (int y = 0; y < h; ++y)
                value += weights[e] * std::popcount(territory[y] & valueRows[e][y]);
        return value;
    }

    Rows closeOwn(const Rows& territory, const Rows& line) const {
        Rows wall{};
        for (int y = 0; y < h; ++y) wall[y] = territory[y] | line[y];
        Rows result = enclosed(wall);
        for (int y = 0; y < h; ++y) result[y] &= ~protectedOther[y];
        return result;
    }

    // Returns the exact self-elimination step, or zero if the path stays alive.
    // Intermediate returns capture territory and clear the old line, just as
    // in Game.step; merely reaching a previously drawn cell is not assumed fatal.
    int simulateReset(const Path& path, double* secured = nullptr) const {
        Rows territory = ownRows, line = trailRows;
        bool activeLine = p[me].length > 0;
        int u = p[me].pos, heading = p[me].dir, count = 0;
        for (int d : path) {
            if (d == (heading ^ 1)) return 0;
            int v = next[u][d]; ++count; heading = d;
            if (v < 0 || (line[v / w] & (std::uint64_t{1} << (v % w)))) {
                if (secured) *secured = securedValue(territory);
                return count;
            }
            std::uint64_t bit = std::uint64_t{1} << (v % w);
            if (territory[v / w] & bit) {
                if (activeLine) {
                    territory = closeOwn(territory, line);
                    line.fill(0); activeLine = false;
                }
            } else {
                line[v / w] |= bit; activeLine = true;
            }
            u = v;
        }
        return 0;
    }

    std::vector<ResetOption> resetOptions() {
        std::array<ResetOption, 64> byLength;
        auto record = [&](const Path& path, double secured) {
            int n = int(path.size());
            if (n && n < 64 && secured > byLength[n].secured)
                byLength[n] = {path, secured};
        };
        Path path;
        int foreign = INF;
        for (int k = 0; k < cells; ++k) if (owner[k] != me)
            foreign = std::min(foreign, manhattan(k, p[me].pos));
        if (p[me].length || foreign <= 2) {
            int nodes = 0;
            auto search = [&](auto&& self, int u, int heading, int depth,
                              const Rows& territory, const Rows& line, bool hasLine) -> void {
                if (depth == 6 || ++nodes > 6000) return;
                for (int d = 0; d < 4; ++d) if (d != (heading ^ 1)) {
                    int v = next[u][d];
                    path.push_back(d);
                    if (v < 0 || (line[v / w] & (std::uint64_t{1} << (v % w)))) {
                        record(path, securedValue(territory)); path.pop_back(); continue;
                    }
                    std::uint64_t bit = std::uint64_t{1} << (v % w);
                    if (territory[v / w] & bit) {
                        if (hasLine) {
                            Rows claimed = closeOwn(territory, line), cleared{};
                            self(self, v, d, depth + 1, claimed, cleared, false);
                        } else self(self, v, d, depth + 1, territory, line, false);
                    } else {
                        Rows extended = line; extended[v / w] |= bit;
                        self(self, v, d, depth + 1, territory, extended, true);
                    }
                    path.pop_back();
                }
            };
            search(search, p[me].pos, p[me].dir, 0, ownRows, trailRows, p[me].length > 0);
        }
        auto wallRoutes = [&](const Path& prefixPath) {
            int position = p[me].pos, heading = p[me].dir;
            for (int d : prefixPath) { position = next[position][d]; heading = d; if (position < 0) return; }
            for (int first = 0; first < 4; ++first) if (first != (heading ^ 1)) {
                // Straight to a wall, or one perpendicular step before taking
                // the wall behind the original heading.
                for (int bend = 0; bend < 2; ++bend) {
                    Path route = prefixPath;
                    int u = position, d = first;
                    if (bend) {
                        int v = next[u][d]; route.push_back(d);
                        if (v < 0) continue;
                        u = v; d = heading ^ 1;
                        if (d == (first ^ 1)) continue;
                    }
                    for (int steps = 0; steps < std::max(w, h) + 1; ++steps) {
                        route.push_back(d); u = next[u][d];
                        if (u < 0) break;
                    }
                    double secured = 0;
                    int deathAt = simulateReset(route, &secured);
                    if (deathAt) { route.resize(deathAt); record(route, secured); }
                }
            }
        };
        wallRoutes({});
        if (p[me].length) {
            Path back = returnPath();
            if (!back.empty() && back.size() <= 8) wallRoutes(back);
        }
        std::vector<ResetOption> result;
        for (auto& x : byLength) if (!x.path.empty()) result.push_back(std::move(x));
        return result;
    }

    Path forcedCutPath(int victim) {
        if (victim < 0 || victim >= players || victim == me || !p[victim].length) return {};
        const bool duelOwnApproach = players == 2 && teleportPhase == 2
            && !p[me].length && idle[victim] < 3 && enemyHome[victim] >= 2
            && turns - turn >= 2;
        int q[MAX_CELLS], a = 0, b = 0, start = p[me].pos;
        int dist[MAX_CELLS], par[MAX_CELLS], pd[MAX_CELLS];
        std::fill(dist, dist + cells, INF); dist[start] = 0;
        int goal = -1;
        for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1)) {
            int v = next[start][d];
            if (v < 0 || trail[v] == me) continue;
            if (trail[v] == victim) return Path{d};
            if (collision(d)) {
                bool finishNext = false;
                if (duelOwnApproach && owner[v] == me) {
                    // A first-step head collision kills the duel victim too.
                    // Otherwise we stay home with no exposed line; the victim
                    // cannot close yet, and a legal next step cuts its old line.
                    for (int z = 0; z < 4; ++z) if (z != (d ^ 1)) {
                        int k = next[v][z];
                        if (k >= 0 && trail[k] == victim) finishNext = true;
                    }
                }
                if (!finishNext) continue;
            }
            dist[v] = 1; par[v] = start; pd[v] = d; q[b++] = v;
        }
        while (a < b) {
            int u = q[a++];
            if (trail[u] == victim) { goal = u; break; }
            if (dist[u] >= enemyHome[victim]) continue;
            for (int d = 0; d < 4; ++d) {
                int v = next[u][d];
                if (v < 0 || trail[v] == me || dist[v] != INF) continue;
                dist[v] = dist[u] + 1; par[v] = u; pd[v] = d; q[b++] = v;
            }
        }
        if (goal < 0 || dist[goal] > enemyHome[victim]) return {};
        Path result;
        for (int k = goal; k != start; k = par[k]) result.push_back(pd[k]);
        std::reverse(result.begin(), result.end()); return result;
    }

    int continueTeleport() {
        if (!teleportPhase) return -1;
        int e = teleportEnemy;
        if (p[e].deaths != teleportEnemyDeaths || !p[e].length || turn - teleportStarted > 65) {
#if TELEPORT_TRACE
            std::cerr << "TP done t=" << turn << " victim=" << e << " enemy_death="
                      << (p[e].deaths != teleportEnemyDeaths) << '\n';
#endif
            teleportPhase = 0; teleportReset.clear(); return -1;
        }
        if (p[me].deaths > teleportOwnDeaths) teleportPhase = 2;
        if (teleportPhase == 1) {
            int cost = simulateReset(teleportReset);
            int approach = INF;
            for (int k = 0; k < cells; ++k) if (trail[k] == e)
                approach = std::min(approach, spawnDistance[k]);
            if (cost && cost + approach <= enemyHome[e]) {
                int d = teleportReset.front(); teleportReset.erase(teleportReset.begin()); return d;
            }
            teleportPhase = 0; teleportReset.clear(); return -1;
        }
        Path cut = forcedCutPath(e);
        if (cut.empty()) { teleportPhase = 0; return -1; }
        return cut.front();
    }

    int chooseTeleport(const Evaluation& expansionChoice, double ordinaryAttackScore) {
        bool possible = false;
        for (int e = 0; e < players; ++e) if (e != me && p[e].length && idle[e] < 3) {
            int closest = INF;
            for (int k = 0; k < cells; ++k) if (trail[k] == e) closest = std::min(closest, spawnDistance[k]);
            if (closest + 1 <= enemyHome[e] && closest + 1 <= turns - turn) possible = true;
        }
        if (!possible) return -1;
        auto resets = resetOptions();
        if (resets.empty()) return -1;
        Evaluation back = evaluate(returnPath());
        double pending = p[me].length ? std::max(double(p[me].length), back.gain) : 0;
        double bestScore = std::max(expansionChoice.score, ordinaryAttackScore) * TELEPORT_SCORE_MULT + 0.25;
        int chosenEnemy = -1, chosenTarget = -1;
        Path chosenReset;
        for (int e = 0; e < players; ++e) if (e != me && p[e].length && idle[e] < 3) {
            std::vector<int> targets;
            for (int k = 0; k < cells; ++k) if (trail[k] == e) targets.push_back(k);
            std::sort(targets.begin(), targets.end(), [&](int x, int y) { return spawnDistance[x] < spawnDistance[y]; });
            if (targets.size() > 12) targets.resize(12);
            for (const auto& reset : resets) {
                int cost = int(reset.path.size());
                for (int target : targets) {
                    if (spawnDistance[target] <= 0) continue; // Respawn itself does not cut a line.
                    int total = cost + spawnDistance[target];
                    if (total > enemyHome[e] || total > turns - turn) continue;
                    bool safe = true;
                    for (int k = spawnParent[target]; k != spawn[me] && safe; k = spawnParent[k]) {
                        if (owner[k] == me) continue;
                        for (int foe = 0; foe < players; ++foe) if (foe != me && idle[foe] < 3)
                            if (enemy[foe][k] < total) safe = false;
                    }
                    if (!safe) continue;
                    double importance = players == 2 ? 1.0 : rivalImportance[e];
                    double value = denied[e] + importance * (10 + 0.7 * p[e].length
                        + 0.4 * std::min(24, manhattan(spawn[e], p[e].pos)));
                    double lost = std::max(0.0, pending - reset.secured);
                    double score = (value + reset.secured - lost) / (players == 2 && turns - turn <= 25 ? 1.0 + total * 0.008 : total + 9.0);
                    if (score > bestScore) {
                        bestScore = score; chosenEnemy = e; chosenTarget = target; chosenReset = reset.path;
                    }
                }
            }
        }
        if (chosenEnemy < 0) return -1;
        teleportPhase = 1; teleportEnemy = chosenEnemy;
        teleportOwnDeaths = p[me].deaths; teleportEnemyDeaths = p[chosenEnemy].deaths;
        teleportStarted = turn; teleportReset = std::move(chosenReset);
#if TELEPORT_TRACE
        std::cerr << "TP start t=" << turn << " victim=" << chosenEnemy << " reset=" << teleportReset.size()
                  << " cut=" << spawnDistance[chosenTarget] << " deadline=" << enemyHome[chosenEnemy]
                  << " denied=" << denied[chosenEnemy] << " pending=" << pending << " score=" << bestScore << '\n';
#endif
        int d = teleportReset.front(); teleportReset.erase(teleportReset.begin()); return d;
    }

    struct DuelCaptureMaps {
        Rows claims[2]{};
    };

    struct DuelResolution {
        std::array<int, 2> area{};
        std::array<bool, 2> dead{}, captured{};
    };

    // Capturing this turn returns onto existing territory, so a capturer's
    // flood walls are exactly its pre-move territory plus its old trail.
    // Claims exclude those walls: owning a wall is not a competing claim.
    DuelCaptureMaps duelCaptureMaps() const {
        DuelCaptureMaps maps;
        Rows walls[2]{};
        for (int k = 0; k < cells; ++k) {
            std::uint64_t bit = std::uint64_t{1} << (k % w);
            if (owner[k] >= 0) walls[owner[k]][k / w] |= bit;
            if (trail[k] >= 0) walls[trail[k]][k / w] |= bit;
        }
        for (int i = 0; i < 2; ++i) if (p[i].length) {
            maps.claims[i] = enclosed(walls[i]);
            for (int y = 0; y < h; ++y) maps.claims[i][y] &= ~walls[i][y];
        }
        return maps;
    }

    // Exact one-turn ownership/death resolution for two active players.
    // The optional owner output is only used by the engine differential probe.
    DuelResolution resolveDuel(int ownDirection, int enemyDirection,
                               const DuelCaptureMaps& maps, int* afterOwner = nullptr) const {
        DuelResolution outcome;
        outcome.area = {p[0].area, p[1].area};
        int directions[2], destination[2];
        directions[me] = ownDirection;
        directions[1 - me] = enemyDirection;
        for (int i = 0; i < 2; ++i) {
            int d = directions[i];
            if (d < 0 || d >= 4 || d == (p[i].dir ^ 1)) d = p[i].dir;
            destination[i] = next[p[i].pos][d];
            outcome.dead[i] = destination[i] < 0;
        }
        if (destination[0] >= 0 && destination[1] >= 0
            && (destination[0] == destination[1]
                || (destination[0] == p[1].pos && destination[1] == p[0].pos))) {
            outcome.dead[0] = outcome.dead[1] = true;
        }
        // Intended moves still cut old trails even if their mover also dies.
        for (int i = 0; i < 2; ++i) if (destination[i] >= 0) {
            int victim = trail[destination[i]];
            if (victim >= 0) outcome.dead[victim] = true;
        }
        for (int i = 0; i < 2; ++i)
            outcome.captured[i] = !outcome.dead[i] && p[i].length
                                  && owner[destination[i]] == i;
        if (!outcome.captured[0] && !outcome.captured[1]) {
            if (afterOwner) std::copy(owner, owner + cells, afterOwner);
            return outcome;
        }
        for (int k = 0; k < cells; ++k) {
            int claimant = -1, oldOwner = owner[k], newOwner = oldOwner;
            if (trail[k] >= 0 && outcome.captured[trail[k]]) {
                // Capturers' old trail cells take precedence over all enclosures.
                claimant = trail[k];
            } else {
                std::uint64_t bit = std::uint64_t{1} << (k % w);
                bool first = outcome.captured[0] && (maps.claims[0][k / w] & bit);
                bool second = outcome.captured[1] && (maps.claims[1][k / w] & bit);
                if (first != second) claimant = second ? 1 : 0;
            }
            if (claimant >= 0 && (protect[k] < 0 || protect[k] == claimant)) newOwner = claimant;
            if (newOwner != oldOwner) {
                if (oldOwner >= 0) --outcome.area[oldOwner];
                ++outcome.area[newOwner];
            }
            if (afterOwner) afterOwner[k] = newOwner;
        }
        return outcome;
    }

    int rescueHome(int original) const {
        if (players != 2 || !p[me].length || idle[1 - me] >= 3) return original;
        int choices[3], count = 0;
        for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1)) {
            int v = next[p[me].pos][d];
            if (v >= 0 && owner[v] == me && trail[v] != me && collision(d)) choices[count++] = d;
        }
        if (!count) return original;
        DuelCaptureMaps maps = duelCaptureMaps();
        auto assess = [&](int d, bool* admissible = nullptr) {
            int worst = INF;
            if (admissible) *admissible = true;
            for (int z = 0; z < 4; ++z) if (z != (p[1 - me].dir ^ 1)) {
                DuelResolution outcome = resolveDuel(d, z, maps);
                worst = std::min(worst, outcome.area[me] - outcome.area[1 - me]);
                if (admissible && outcome.dead[me] && !outcome.dead[1 - me]) *admissible = false;
            }
            return worst;
        };
        int originalWorst = assess(original), bestWorst = originalWorst, result = original;
        for (int j = 0; j < count; ++j) {
            bool admissible;
            int worst = assess(choices[j], &admissible);
            if (admissible && worst > bestWorst) bestWorst = worst, result = choices[j];
        }
#if HOME_RESCUE_TRACE
        if (result != original)
            std::cerr << "HOME rescue t=" << turn << " from=" << DIRS[original] << " to=" << DIRS[result]
                      << " worst_before=" << originalWorst << " worst_after=" << bestWorst << '\n';
#endif
        return result;
    }

    std::array<int, 4> endInitialDirections{{-1, -1, -1, -1}};

    struct EndState {
        Rows owned[2]{}, drawn[2]{};
        int pos[2]{}, dir[2]{}, area[2]{};
        bool outside[2]{};
    };

    EndState endState() const {
        EndState state;
        for (int i = 0; i < 2; ++i) {
            state.pos[i] = p[i].pos; state.dir[i] = p[i].dir;
            state.area[i] = p[i].area; state.outside[i] = p[i].length > 0;
        }
        for (int k = 0; k < cells; ++k) {
            std::uint64_t bit = std::uint64_t{1} << (k % w);
            if (owner[k] >= 0) state.owned[owner[k]][k / w] |= bit;
            if (trail[k] >= 0) state.drawn[trail[k]][k / w] |= bit;
        }
        return state;
    }

    // This state transition uses the same simultaneous order as the referee:
    // old-trail cuts, reset, move, then conflicting capture claims.
    EndState endStep(const EndState& before, int ownDir, int enemyDir) const {
        EndState after = before;
        int directions[2], target[2]; bool dead[2]{}, capture[2]{};
        directions[me] = ownDir; directions[1 - me] = enemyDir;
        for (int i = 0; i < 2; ++i) {
            if (directions[i] < 0 || directions[i] > 3 || directions[i] == (before.dir[i] ^ 1))
                directions[i] = before.dir[i];
            target[i] = next[before.pos[i]][directions[i]];
            dead[i] = target[i] < 0;
        }
        if (target[0] >= 0 && target[1] >= 0
            && (target[0] == target[1] || (target[0] == before.pos[1] && target[1] == before.pos[0])))
            dead[0] = dead[1] = true;
        for (int i = 0; i < 2; ++i) if (target[i] >= 0) {
            int y = target[i] / w; std::uint64_t bit = std::uint64_t{1} << (target[i] % w);
            for (int j = 0; j < 2; ++j) if (before.drawn[j][y] & bit) dead[j] = true;
        }
        for (int i = 0; i < 2; ++i) {
            if (dead[i]) {
                after.pos[i] = spawn[i];
                after.dir[i] = endInitialDirections[i] >= 0 ? endInitialDirections[i]
                    : (i == me ? initDirection : initDirection ^ 1);
                after.drawn[i].fill(0); after.outside[i] = false;
                continue;
            }
            after.pos[i] = target[i]; after.dir[i] = directions[i];
            int y = target[i] / w; std::uint64_t bit = std::uint64_t{1} << (target[i] % w);
            if (before.owned[i][y] & bit) {
                capture[i] = before.outside[i];
                if (capture[i]) { after.drawn[i].fill(0); after.outside[i] = false; }
            } else {
                after.drawn[i][y] |= bit; after.outside[i] = true;
            }
        }
        if (!capture[0] && !capture[1]) return after;
        Rows claim[2]{}, stroke[2]{};
        for (int i = 0; i < 2; ++i) if (capture[i]) {
            Rows walls{};
            for (int y = 0; y < h; ++y) walls[y] = before.owned[i][y] | before.drawn[i][y];
            claim[i] = enclosed(walls); stroke[i] = before.drawn[i];
            for (int y = 0; y < h; ++y) claim[i][y] &= ~walls[y];
        }
        after.area[0] = after.area[1] = 0;
        for (int y = 0; y < h; ++y) {
            std::uint64_t first = stroke[0][y] | (claim[0][y] & ~claim[1][y] & ~stroke[1][y]);
            std::uint64_t second = stroke[1][y] | (claim[1][y] & ~claim[0][y] & ~stroke[0][y]);
            first &= ~(me == 0 ? protectedOther[y] : protectedOwn[y]);
            second &= ~(me == 1 ? protectedOther[y] : protectedOwn[y]);
            after.owned[0][y] = (before.owned[0][y] & ~second) | first;
            after.owned[1][y] = (before.owned[1][y] & ~first) | second;
            after.area[0] += std::popcount(after.owned[0][y]);
            after.area[1] += std::popcount(after.owned[1][y]);
        }
        return after;
    }

    int endNodes = 0;
    bool endAborted = false;

    int endSolve(const EndState& state, int left, int alpha, int beta) {
        if ((++endNodes & 63) == 0 && std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() >= 42.0) {
            endAborted = true; return 0;
        }
        if (!left) return state.area[me] - state.area[1 - me];
        int best = -INF;
        for (int d = 0; d < 4; ++d) if (d != (state.dir[me] ^ 1)) {
            int worst = INF;
            for (int z = 0; z < 4; ++z) if (z != (state.dir[1 - me] ^ 1)) {
                worst = std::min(worst, endSolve(endStep(state, d, z), left - 1,
                                                std::max(alpha, best), std::min(beta, worst)));
                if (endAborted) return 0;
                if (worst <= std::max(alpha, best)) break;
            }
            best = std::max(best, worst);
            if (best >= beta) break;
        }
        return best;
    }

    // Solve the pure maximin continuation against every legal response.
    // The value assumes this continuation policy; a later time-budget fallback
    // may leave that policy. No prediction of the enemy's hidden move is used.
    int exactEndgame(int original) {
        if (endInitialDirections[me] < 0)
            for (int i = 0; i < players; ++i) endInitialDirections[i] = p[i].dir;
        int left = turns - turn;
        if (players != 2 || left < 1 || left > 8 || idle[1 - me] >= 3) return original;
        endNodes = 0; endAborted = false;
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() >= 42.0) {
            endAborted = true; return original;
        }
        EndState state = endState();
        int best = -INF, chosen = original, baseline = -INF;
        int order[4] = {original, 0, 1, 2};
        int count = 1;
        for (int d = 0; d < 4; ++d)
            if (d != original && d != (state.dir[me] ^ 1)) order[count++] = d;
        for (int j = 0; j < count; ++j) {
            int d = order[j], worst = INF;
            for (int z = 0; z < 4; ++z) if (z != (state.dir[1 - me] ^ 1)) {
                worst = std::min(worst, endSolve(endStep(state, d, z), left - 1,
                                                best, worst));
                if (endAborted) return original;
                if (worst <= best) break;
            }
            if (!j) baseline = worst;
            if (worst > best) { best = worst; chosen = d; }
        }
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() >= 42.0) {
            endAborted = true; return original;
        }
#if ENDGAME_TRACE
        if (chosen != original)
            std::cerr << "END t=" << turn << " from=" << DIRS[original] << " to=" << DIRS[chosen]
                      << " old_value=" << baseline << " new_value=" << best
                      << " nodes=" << endNodes << '\n';
#endif
        return chosen;
    }

    int finalizeMove(int original) {
        int result = exactEndgame(rescueHome(original));
        if (result != original) {
            plan.clear();
            teleportPhase = 0; teleportEnemy = -1; teleportReset.clear();
        }
        expected = next[p[me].pos][result];
        return result;
    }

    int fallback() {
        int start = p[me].pos, result = p[me].dir;
        double best = -1e9;
        for (int d = 0; d < 4; ++d) if (d != (p[me].dir ^ 1)) {
            int v = next[start][d];
            if (v < 0 || trail[v] == me) continue;
            double value = (owner[v] == me ? 100 : 0) - (collision(d) ? 1000 : 0);
            value += std::min(danger[v], 60) * 0.3 - std::min(home[v], 100) * 3;
            for (int z = 0; z < 4; ++z) if (z != (d ^ 1)) {
                int k = next[v][z];
                if (k >= 0 && trail[k] != me) value += owner[k] == me ? 3 : 0.2;
            }
            if (d == p[me].dir) value += 0.05;
            if (value > best) best = value, result = d;
        }
        return result;
    }

    int decide() {
        started = std::chrono::steady_clock::now();
        if (expected != p[me].pos || previousDeaths != p[me].deaths
            || (previousLength && !p[me].length)) plan.clear();
        previousDeaths = p[me].deaths; previousLength = p[me].length;
        fields();
        prepareSpawn();
        int forced = continueTeleport();
        if (forced >= 0) {
            plan.clear(); return finalizeMove(forced);
        }
        auto [hunting, huntScore] = attack();
        Evaluation best = expansion();
        int resetMove = chooseTeleport(best, huntScore);
        if (resetMove >= 0) {
            plan.clear(); return finalizeMove(resetMove);
        }
        if (!hunting.empty() && (huntScore > best.score * 1.15 + 0.3 || (players == 2 && hunting.size() == 1)))
            plan = std::move(hunting);
        else if (best.closed && best.margin >= 1)
            plan = std::move(best.path);
        else
            plan.clear();
        int result = fallback();
        if (!plan.empty()) {
            result = plan.front(); plan.erase(plan.begin());
        }
        return finalizeMove(result);
    }

    bool read() {
        std::string token;
        if (!(std::cin >> token >> turn)) return false;
        for (int i = 0; i < players; ++i) {
            int id, x, y; char d; Player q;
            std::cin >> token >> id >> x >> y >> d >> q.length >> q.area >> q.deaths;
            q.pos = y * w + x; q.dir = static_cast<int>(std::string(DIRS).find(d)); p[id] = q;
        }
        std::cin >> token;
        for (int y = 0; y < h; ++y) {
            std::cin >> token;
            for (int x = 0; x < w; ++x) owner[y * w + x] = token[x] == '.' ? -1 : token[x] - '0';
        }
        std::cin >> token;
        for (int y = 0; y < h; ++y) {
            std::cin >> token;
            for (int x = 0; x < w; ++x) trail[y * w + x] = token[x] == '.' ? -1 : token[x] - '0';
        }
        std::cin >> token;
        return static_cast<bool>(std::cin);
    }
};
} // namespace land

#ifndef LAND_TEST
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::string token;
    int w, h, turns, players, me, moveMs, initMs;
    if (!(std::cin >> token >> w >> h >> turns >> players >> me >> moveMs >> initMs)) return 0;
    std::array<int, 4> spawn{};
    for (int i = 0; i < players; ++i) {
        int id, x, y;
        std::cin >> token >> id >> x >> y; spawn[id] = y * w + x;
    }
    auto bot = std::make_unique<land::Bot>(w, h, players, me, turns, spawn);
    while (bot->read()) std::cout << bot->turn << ' ' << land::DIRS[bot->decide()] << std::endl;
}
#endif
