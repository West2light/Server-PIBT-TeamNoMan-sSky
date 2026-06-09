#pragma once
/**
 * UnityStartKitAdapter
 * Converts Unity TCP protocol DTOs <-> SharedEnvironment used by DefaultPlanner.
 *
 * Layout contract (matches PibtTcpProtocol.cs):
 *   loc        = y * width + x   (row-major, 0-indexed)
 *   orientation: 0=east  1=south  2=west  3=north
 *   symbols:   '.'=free  '@'=obstacle
 */
#include <string>
#include <vector>
#include <chrono>
#include "SharedEnv.h"
#include "ActionModel.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace UnityAdapter
{
    /**
     * Populate env from hello.map DTO.
     * Sets env.rows, env.cols, env.map.
     * Resets env.num_of_agents, curr_states, goal_locations.
     * Call once per session on hello message.
     */
    void ApplyMap(SharedEnvironment& env, const json& map_dto);

    /**
     * Update env from plan_step.agents array.
     * Sets env.curr_states[i], env.goal_locations[i], env.curr_timestep,
     * and env.plan_start_time to now().
     */
    void ApplyAgents(SharedEnvironment& env, const json& agents_array, int timestep);

    /**
     * Convert Action enum -> JSON string ("FW", "CR", "CCR", "W").
     * Returns "W" for unknown / NA.
     */
    const char* ActionToString(Action a);

    /**
     * Compute nextLoc after applying action to state s.
     * Used to fill plan_result.actions[i].nextLoc.
     */
    int NextLoc(const State& s, Action a, int cols);

    /**
     * True when loc maps to a valid index inside env.map.
     */
    bool IsLocInMap(const SharedEnvironment& env, int loc);

    /**
     * True when loc is in bounds and walkable in env.map.
     */
    bool IsLocWalkable(const SharedEnvironment& env, int loc);

    /**
     * True when to_loc is one 4-neighbor step away from from_loc without wrapping rows.
     */
    bool IsAdjacentNoWrap(int from_loc, int to_loc, int cols);

    /**
     * Sanitize a planner action before it is emitted to Unity.
     * Returns W when the action would move out of bounds or into a blocked cell.
     */
    Action SanitizeAction(const SharedEnvironment& env, const State& s, Action a, std::string* reason = nullptr);
}
