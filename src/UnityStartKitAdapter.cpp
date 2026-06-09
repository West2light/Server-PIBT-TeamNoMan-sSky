#include "UnityStartKitAdapter.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>

namespace UnityAdapter
{

void ApplyMap(SharedEnvironment& env, const json& map_dto)
{
    const int width  = map_dto.value("width",  0);
    const int height = map_dto.value("height", 0);
    const std::string symbols = map_dto.value("symbols", "");

    if (width <= 0 || height <= 0)
        throw std::runtime_error("[UnityAdapter] ApplyMap: invalid map dimensions");
    if ((int)symbols.size() != width * height)
        throw std::runtime_error("[UnityAdapter] ApplyMap: symbols length mismatch");

    env.cols = width;
    env.rows = height;
    env.map.resize(width * height);

    // '.' = 0 (walkable), '@' or anything else = 1 (obstacle)
    for (int i = 0; i < width * height; ++i)
        env.map[i] = (symbols[i] == '.') ? 0 : 1;

    std::cout << "[UnityAdapter] ApplyMap: " << width << "x" << height
              << " (" << symbols.size() << " cells)" << std::endl;
}

bool IsLocInMap(const SharedEnvironment& env, int loc)
{
    return env.cols > 0
        && env.rows > 0
        && loc >= 0
        && loc < env.cols * env.rows
        && loc < static_cast<int>(env.map.size());
}

bool IsLocWalkable(const SharedEnvironment& env, int loc)
{
    return IsLocInMap(env, loc) && env.map[loc] == 0;
}

bool IsAdjacentNoWrap(int from_loc, int to_loc, int cols)
{
    if (cols <= 0)
        return false;

    const int from_x = from_loc % cols;
    const int from_y = from_loc / cols;
    const int to_x   = to_loc % cols;
    const int to_y   = to_loc / cols;
    return std::abs(from_x - to_x) + std::abs(from_y - to_y) == 1;
}

void ApplyAgents(SharedEnvironment& env, const json& agents_array, int timestep)
{
    if (!agents_array.is_array())
        throw std::runtime_error("[UnityAdapter] ApplyAgents: agents is not an array");

    const int n = static_cast<int>(agents_array.size());
    env.num_of_agents = n;
    env.curr_timestep = timestep;
    env.plan_start_time = std::chrono::steady_clock::now();

    // Resize containers to fit exactly n agents
    env.curr_states.resize(n);
    env.goal_locations.resize(n);

    for (int i = 0; i < n; ++i)
    {
        const json& a = agents_array[i];
        const int agent_id  = a.value("id",          i);
        const int loc       = a.value("loc",          0);
        const int orient    = a.value("orientation",  0);
        const int goal_loc  = a.value("goalLoc",     loc);

        if (orient < 0 || orient > 3)
        {
            throw std::runtime_error("[UnityAdapter] ApplyAgents: invalid orientation for agent " + std::to_string(agent_id));
        }
        if (!IsLocInMap(env, loc) || !IsLocWalkable(env, loc))
        {
            throw std::runtime_error("[UnityAdapter] ApplyAgents: invalid loc for agent " + std::to_string(agent_id));
        }
        if (!IsLocInMap(env, goal_loc) || !IsLocWalkable(env, goal_loc))
        {
            throw std::runtime_error("[UnityAdapter] ApplyAgents: invalid goalLoc for agent " + std::to_string(agent_id));
        }

        // Sanity-check agent ordering: Unity sends agents in ascending id order
        if (agent_id != i)
        {
            std::cerr << "[UnityAdapter] ApplyAgents: agent id " << agent_id
                      << " at index " << i << " (expected " << i << ")" << std::endl;
        }

        env.curr_states[i] = State(loc, timestep, orient);

        // goal_locations[i] is a list of <loc, reveal_time> pairs
        // For Unity mode we use a single goal with reveal_time = 0
        env.goal_locations[i] = { {goal_loc, 0} };
    }
}

const char* ActionToString(Action a)
{
    switch (a)
    {
        case Action::FW:  return "FW";
        case Action::CR:  return "CR";
        case Action::CCR: return "CCR";
        case Action::W:   return "W";
        default:          return "W";
    }
}

int NextLoc(const State& s, Action a, int cols)
{
    // Orientation moves: east(0)=+1, south(1)=+cols, west(2)=-1, north(3)=-cols
    static const int delta[4] = {1, cols, -1, -cols};

    switch (a)
    {
        case Action::FW:
            if (s.orientation >= 0 && s.orientation < 4)
                return s.location + delta[s.orientation];
            return s.location;
        case Action::CR:
        case Action::CCR:
        case Action::W:
        default:
            return s.location;
    }
}

Action SanitizeAction(const SharedEnvironment& env, const State& s, Action a, std::string* reason)
{
    if (reason != nullptr)
        reason->clear();

    if (!IsLocInMap(env, s.location) || s.orientation < 0 || s.orientation > 3)
    {
        if (reason != nullptr) *reason = "invalid_state";
        return Action::W;
    }

    if (a != Action::FW)
        return a;

    const int next_loc = NextLoc(s, a, env.cols);
    if (!IsLocInMap(env, next_loc))
    {
        if (reason != nullptr) *reason = "fw_out_of_bounds";
        return Action::W;
    }
    if (!IsAdjacentNoWrap(s.location, next_loc, env.cols))
    {
        if (reason != nullptr) *reason = "fw_row_wrap";
        return Action::W;
    }
    if (!IsLocWalkable(env, next_loc))
    {
        if (reason != nullptr) *reason = "fw_blocked";
        return Action::W;
    }

    return a;
}

} // namespace UnityAdapter
