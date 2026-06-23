/**
 * dual_width_smoke_main.cpp  (G-1)
 *
 * Regression guard: run two maps with different widths in the same process and
 * assert that NextLoc returns the correct stride for EACH map.  The `static delta`
 * bug (pre-F-S1) would bake the first map's cols forever, causing the second map's
 * south/north strides to be wrong.
 *
 * Build target: dual_width_smoke_test  (added to CMakeLists.txt)
 * Run: ./build/dual_width_smoke_test  (exit 0 = pass, exit 1 = fail)
 * compile.sh runs this after make so a re-introduced `static` fails the build.
 */

#include "UnityStartKitAdapter.h"

#include <cassert>
#include <iostream>

// Convenience: expected nextLoc for FW in a given orientation
static int ExpectedFW(int loc, int orientation, int cols)
{
    // east=+1, south=+cols, west=-1, north=-cols
    static const int stride[4] = {1, 0, -1, 0};
    static const int vst[4]    = {0, 1,  0, -1};
    return loc + stride[orientation] + vst[orientation] * cols;
}

static bool TestWidth(int cols, const char* label)
{
    bool ok = true;
    constexpr int loc = 20; // arbitrary interior cell (row 0 col 20, for cols > 20)

    for (int ori = 0; ori < 4; ++ori)
    {
        int got      = UnityAdapter::NextLoc(State(loc, 0, ori), Action::FW, cols);
        int expected = ExpectedFW(loc, ori, cols);
        if (got != expected)
        {
            std::cerr << "[dual_width_smoke] FAIL map=" << label
                      << " cols=" << cols
                      << " ori=" << ori
                      << " expected=" << expected
                      << " got=" << got
                      << (got == ExpectedFW(loc, ori, 32)
                              ? " (static delta from first map cols=32 still cached!)"
                              : "")
                      << "\n";
            ok = false;
        }
    }
    return ok;
}

// W/CR/CCR must always return `loc` unchanged (no movement, just rotation or wait)
static bool TestNonFWActionsStayInPlace(int cols, const char* label)
{
    constexpr int loc = 10;
    bool ok = true;
    for (int ori = 0; ori < 4; ++ori)
    {
        for (Action a : {Action::W, Action::CR, Action::CCR})
        {
            int got = UnityAdapter::NextLoc(State(loc, 0, ori), a, cols);
            if (got != loc)
            {
                std::cerr << "[dual_width_smoke] FAIL map=" << label
                          << " non-FW action should stay at " << loc
                          << " but got " << got << "\n";
                ok = false;
            }
        }
    }
    return ok;
}

int main()
{
    // Map A: 32 columns (e.g. random-32-32-10)
    bool a = TestWidth(32, "map_A_cols32");
    // Map B: 251 columns (e.g. lt_gallowstemplar_n) — would fail with static delta
    bool b = TestWidth(251, "map_B_cols251");
    // Map C: 128 columns (e.g. ht_maze)
    bool c = TestWidth(128, "map_C_cols128");

    bool nonfw_a = TestNonFWActionsStayInPlace(32,  "map_A_cols32");
    bool nonfw_b = TestNonFWActionsStayInPlace(251, "map_B_cols251");

    if (!a || !b || !c || !nonfw_a || !nonfw_b)
    {
        std::cerr << "[dual_width_smoke] FAILED — possible static delta regression\n";
        return 1;
    }

    std::cout << "[dual_width_smoke] ok\n";
    return 0;
}
