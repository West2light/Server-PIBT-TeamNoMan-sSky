#include "epibt.h"

#include "heuristics.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>

namespace EpibtPlanner
{
namespace
{

using Clock = std::chrono::steady_clock;

struct Reservation
{
    int agentId = -1;
    Operation op;
    OperationPath path;
};

std::vector<Operation> g_operations;
std::vector<std::optional<Operation>> g_prev_operations;
std::vector<AgentPlanTrace> g_last_agent_traces;
EpibtStats g_last_stats;
bool g_initialized = false;

void ResetState()
{
    g_operations.clear();
    g_prev_operations.clear();
    g_last_agent_traces.clear();
    g_last_stats = {};
    g_initialized = false;
}

int GetGoalLocation(const SharedEnvironment* env, int agent_id)
{
    if (agent_id < 0 || agent_id >= env->num_of_agents)
    {
        return 0;
    }

    if (agent_id < static_cast<int>(env->goal_locations.size()) &&
        !env->goal_locations[agent_id].empty())
    {
        return env->goal_locations[agent_id].front().first;
    }

    return env->curr_states[agent_id].location;
}

bool IsLocationValid(int location, const SharedEnvironment* env)
{
    return location >= 0 &&
           location < static_cast<int>(env->map.size()) &&
           env->map[location] == 0;
}

State ApplyAction(State state, Action action, SharedEnvironment* env)
{
    State next = state;
    next.timestep = state.timestep + 1;

    if (action == Action::FW)
    {
        if (state.orientation == 0)
        {
            next.location += 1;
        }
        else if (state.orientation == 1)
        {
            next.location += env->cols;
        }
        else if (state.orientation == 2)
        {
            next.location -= 1;
        }
        else if (state.orientation == 3)
        {
            next.location -= env->cols;
        }
    }
    else if (action == Action::CR)
    {
        next.orientation = (state.orientation + 1) % 4;
    }
    else if (action == Action::CCR)
    {
        next.orientation = (state.orientation + 3) % 4;
    }

    return next;
}

OperationPath BuildPath(State start, const Operation& operation, SharedEnvironment* env)
{
    OperationPath path;
    path.states[0] = start;
    for (std::size_t step = 0; step < operation.actions.size(); ++step)
    {
        path.states[step + 1] = ApplyAction(path.states[step], operation.actions[step], env);
    }
    return path;
}

bool IsPathValid(const OperationPath& path, const Operation& operation, SharedEnvironment* env)
{
    if (path.states[0].orientation < 0 || path.states[0].orientation > 3)
    {
        return false;
    }
    if (!IsLocationValid(path.states[0].location, env))
    {
        return false;
    }

    for (std::size_t step = 0; step < operation.actions.size(); ++step)
    {
        const State& prev = path.states[step];
        const State& next = path.states[step + 1];

        if (next.orientation < 0 || next.orientation > 3)
        {
            return false;
        }

        if (operation.actions[step] == Action::FW)
        {
            if (!DefaultPlanner::validateMove(prev.location, next.location, env))
            {
                return false;
            }
        }
        else if (next.location != prev.location)
        {
            return false;
        }

        if (!IsLocationValid(next.location, env))
        {
            return false;
        }
    }

    return true;
}

bool HasVertexConflict(const OperationPath& lhs, const OperationPath& rhs)
{
    for (std::size_t step = 1; step < lhs.states.size(); ++step)
    {
        if (lhs.states[step].location == rhs.states[step].location)
        {
            return true;
        }
    }
    return false;
}

bool HasEdgeSwapConflict(const OperationPath& lhs, const OperationPath& rhs)
{
    for (std::size_t step = 1; step < lhs.states.size(); ++step)
    {
        if (lhs.states[step - 1].location == rhs.states[step].location &&
            lhs.states[step].location == rhs.states[step - 1].location)
        {
            return true;
        }
    }
    return false;
}

std::vector<int> GetUsedAgents(const OperationPath& path, const std::vector<Reservation>& reservations)
{
    std::vector<int> used_agents;
    for (const Reservation& reservation : reservations)
    {
        if (HasVertexConflict(path, reservation.path) || HasEdgeSwapConflict(path, reservation.path))
        {
            used_agents.push_back(reservation.agentId);
        }
    }
    return used_agents;
}

void ReservePath(int agent_id, const Operation& op, const OperationPath& path, std::vector<Reservation>& reservations)
{
    reservations.push_back(Reservation {agent_id, op, path});
}

void RemovePath(int agent_id, std::vector<Reservation>& reservations)
{
    reservations.erase(
        std::remove_if(
            reservations.begin(),
            reservations.end(),
            [agent_id](const Reservation& reservation)
            {
                return reservation.agentId == agent_id;
            }),
        reservations.end());
}

int ComputeHeuristic(const SharedEnvironment* env, int location, int goal)
{
    if (location == goal)
    {
        return 0;
    }

    if (!IsLocationValid(location, env) || !IsLocationValid(goal, env))
    {
        return DefaultPlanner::manhattanDistance(location, goal, env);
    }

    const int h = DefaultPlanner::get_h(const_cast<SharedEnvironment*>(env), location, goal);
    if (h >= std::numeric_limits<int>::max() / 4)
    {
        return DefaultPlanner::manhattanDistance(location, goal, env);
    }
    return h;
}

int CountAction(const Operation& op, Action action)
{
    return static_cast<int>(std::count(op.actions.begin(), op.actions.end(), action));
}

int ScoreOperation(
    int agent_id,
    const Operation& operation,
    const OperationPath& path,
    SharedEnvironment* env,
    bool inherited_priority)
{
    const int goal = GetGoalLocation(env, agent_id);
    const int start_h = ComputeHeuristic(env, path.states.front().location, goal);
    const int end_h = ComputeHeuristic(env, path.states.back().location, goal);
    const int wait_penalty = CountAction(operation, Action::W) * 4;
    const int turn_penalty = (CountAction(operation, Action::CR) + CountAction(operation, Action::CCR)) * 2;
    const int progress_bonus = std::max(0, start_h - end_h) * 6;
    const int inherited_bonus = inherited_priority ? 3 : 0;
    return end_h * 16 + wait_penalty + turn_penalty - progress_bonus - inherited_bonus;
}

Operation ShiftInheritedOperation(const Operation& operation)
{
    Operation shifted;
    shifted.actions[0] = operation.actions[1];
    shifted.actions[1] = operation.actions[2];
    shifted.actions[2] = Action::W;
    return shifted;
}

std::vector<Operation> BuildOperationsLen3()
{
    std::vector<Operation> operations;
    static const std::array<Action, 4> base_actions {Action::FW, Action::CR, Action::CCR, Action::W};

    operations.reserve(64);
    for (Action a0 : base_actions)
    {
        for (Action a1 : base_actions)
        {
            for (Action a2 : base_actions)
            {
                Operation op;
                op.actions = {a0, a1, a2};
                operations.push_back(op);
            }
        }
    }

    return operations;
}

Operation SelectOperation(
    int agent_id,
    SharedEnvironment* env,
    const std::vector<Reservation>& reservations,
    bool inherited_priority,
    std::string* debug_reason)
{
    struct Candidate
    {
        Operation op;
        OperationPath path;
        bool inherited = false;
        int score = std::numeric_limits<int>::max();
        int conflicts = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(g_operations.size() + 1);

    const State start = env->curr_states[agent_id];

    if (agent_id < static_cast<int>(g_prev_operations.size()) &&
        g_prev_operations[agent_id].has_value())
    {
        Operation inherited = ShiftInheritedOperation(*g_prev_operations[agent_id]);
        OperationPath inherited_path = BuildPath(start, inherited, env);
        if (IsPathValid(inherited_path, inherited, env))
        {
            const std::vector<int> used = GetUsedAgents(inherited_path, reservations);
            if (used.empty())
            {
                inherited.beta = ScoreOperation(agent_id, inherited, inherited_path, env, true);
                candidates.push_back(Candidate {inherited, inherited_path, true, inherited.beta, 0});
            }
            else
            {
                g_last_stats.fallbackInherited++;
                if (debug_reason != nullptr)
                {
                    *debug_reason = "fallback_inherited_conflict";
                }
            }
        }
        else
        {
            g_last_stats.fallbackInherited++;
            if (debug_reason != nullptr)
            {
                *debug_reason = "fallback_inherited_invalid";
            }
        }
    }

    for (const Operation& base_op : g_operations)
    {
        Operation op = base_op;
        OperationPath path = BuildPath(start, op, env);
        if (!IsPathValid(path, op, env))
        {
            continue;
        }

        const std::vector<int> used = GetUsedAgents(path, reservations);
        if (used.size() > 1U)
        {
            g_last_stats.multiConflictSkipped++;
            continue;
        }
        if (!used.empty())
        {
            continue;
        }

        op.beta = ScoreOperation(agent_id, op, path, env, inherited_priority);
        candidates.push_back(Candidate {op, path, false, op.beta, 0});
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& lhs, const Candidate& rhs)
        {
            if (lhs.inherited != rhs.inherited)
            {
                return lhs.inherited && !rhs.inherited;
            }
            if (lhs.score != rhs.score)
            {
                return lhs.score < rhs.score;
            }
            return lhs.op.actions < rhs.op.actions;
        });

    if (!candidates.empty())
    {
        if (candidates.front().inherited)
        {
            g_last_stats.acceptedInherited++;
            if (debug_reason != nullptr)
            {
                *debug_reason = "selected_inherited";
            }
        }
        else if (debug_reason != nullptr && debug_reason->empty())
        {
            *debug_reason = "selected_candidate";
        }
        return candidates.front().op;
    }

    Operation wait_op;
    wait_op.actions = {Action::W, Action::W, Action::W};
    wait_op.beta = std::numeric_limits<int>::max() / 4;
    if (debug_reason != nullptr)
    {
        *debug_reason = "fallback_wait";
    }
    return wait_op;
}

bool HasInheritedTail(int agent_id)
{
    if (agent_id >= static_cast<int>(g_prev_operations.size()) ||
        !g_prev_operations[agent_id].has_value())
    {
        return false;
    }

    const Operation& previous = *g_prev_operations[agent_id];
    return previous.actions[1] != Action::W || previous.actions[2] != Action::W;
}

} // namespace

void reset()
{
    ResetState();
}

void initialize(int preprocess_time_limit, SharedEnvironment* env)
{
    (void) preprocess_time_limit;

    ResetState();
    DefaultPlanner::init_heuristics(env);
    g_operations = BuildOperationsLen3();
    g_prev_operations.resize(env->num_of_agents);
    g_initialized = true;
}

void plan(int time_limit_ms, std::vector<Action>& actions, SharedEnvironment* env)
{
    if (!g_initialized || static_cast<int>(g_prev_operations.size()) != env->num_of_agents)
    {
        initialize(time_limit_ms, env);
    }

    g_last_stats = {};
    g_last_stats.revisitLimit = 0;
    actions.assign(env->num_of_agents, Action::W);
    g_last_agent_traces.assign(env->num_of_agents, AgentPlanTrace {});

    std::vector<int> priorities(env->num_of_agents, 0);
    std::vector<int> agent_ids(env->num_of_agents, 0);
    for (int agent_id = 0; agent_id < env->num_of_agents; ++agent_id)
    {
        agent_ids[agent_id] = agent_id;
        const int goal = GetGoalLocation(env, agent_id);
        priorities[agent_id] = ComputeHeuristic(env, env->curr_states[agent_id].location, goal);
    }

    std::sort(
        agent_ids.begin(),
        agent_ids.end(),
        [&](int lhs, int rhs)
        {
            const bool lhs_inherited = HasInheritedTail(lhs);
            const bool rhs_inherited = HasInheritedTail(rhs);
            if (lhs_inherited != rhs_inherited)
            {
                return lhs_inherited && !rhs_inherited;
            }
            if (priorities[lhs] != priorities[rhs])
            {
                return priorities[lhs] < priorities[rhs];
            }
            return lhs < rhs;
        });

    const auto deadline = Clock::now() + std::chrono::milliseconds(time_limit_ms);
    std::vector<Reservation> reservations;
    reservations.reserve(env->num_of_agents);
    std::vector<std::optional<Operation>> next_prev_operations(env->num_of_agents);

    for (int agent_id : agent_ids)
    {
        if (Clock::now() >= deadline)
        {
            AgentPlanTrace trace;
            trace.agentId = agent_id;
            trace.operation.actions = {Action::W, Action::W, Action::W};
            trace.opIndex = 0;
            trace.debugReason = "time_budget_exhausted";
            g_last_agent_traces[agent_id] = trace;
            break;
        }

        const bool inherited_priority = HasInheritedTail(agent_id);
        std::string debug_reason;
        Operation selected = SelectOperation(agent_id, env, reservations, inherited_priority, &debug_reason);
        OperationPath path = BuildPath(env->curr_states[agent_id], selected, env);

        if (!IsPathValid(path, selected, env))
        {
            selected.actions = {Action::W, Action::W, Action::W};
            selected.beta = std::numeric_limits<int>::max() / 4;
            path = BuildPath(env->curr_states[agent_id], selected, env);
            debug_reason = "fallback_invalid_path";
        }

        RemovePath(agent_id, reservations);
        ReservePath(agent_id, selected, path, reservations);
        next_prev_operations[agent_id] = selected;
        actions[agent_id] = selected.actions[0];

        AgentPlanTrace trace;
        trace.agentId = agent_id;
        trace.operation = selected;
        trace.opIndex = 0;
        trace.debugReason = debug_reason.empty() ? "selected_candidate" : debug_reason;
        g_last_agent_traces[agent_id] = trace;
    }

    g_prev_operations = std::move(next_prev_operations);
}

const EpibtStats& last_stats()
{
    return g_last_stats;
}

const std::vector<AgentPlanTrace>& last_agent_traces()
{
    return g_last_agent_traces;
}

} // namespace EpibtPlanner
