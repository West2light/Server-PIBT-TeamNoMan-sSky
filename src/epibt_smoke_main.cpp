#include "SharedEnv.h"
#include "epibt.h"

#include <iostream>
#include <vector>

namespace
{

SharedEnvironment BuildEnv(
    int rows,
    int cols,
    const std::vector<int>& map,
    const std::vector<State>& states,
    const std::vector<int>& goals)
{
    SharedEnvironment env;
    env.rows = rows;
    env.cols = cols;
    env.map = map;
    env.num_of_agents = static_cast<int>(states.size());
    env.curr_states = states;
    env.goal_locations.resize(states.size());
    for (std::size_t agent_id = 0; agent_id < goals.size(); ++agent_id)
    {
        env.goal_locations[agent_id].push_back({goals[agent_id], 0});
    }
    return env;
}

bool TestEdgeSwap()
{
    SharedEnvironment env = BuildEnv(
        1,
        2,
        {0, 0},
        {State(0, 0, 0), State(1, 0, 2)},
        {1, 0});

    std::vector<Action> actions;
    EpibtPlanner::reset();
    EpibtPlanner::initialize(0, &env);
    EpibtPlanner::plan(50, actions, &env);
    return !(actions.size() == 2 && actions[0] == Action::FW && actions[1] == Action::FW);
}

bool TestVertexConflict()
{
    SharedEnvironment env = BuildEnv(
        1,
        3,
        {0, 0, 0},
        {State(0, 0, 0), State(2, 0, 2)},
        {1, 1});

    std::vector<Action> actions;
    EpibtPlanner::reset();
    EpibtPlanner::initialize(0, &env);
    EpibtPlanner::plan(50, actions, &env);
    return !(actions.size() == 2 && actions[0] == Action::FW && actions[1] == Action::FW);
}

bool TestInheritedOperation()
{
    SharedEnvironment env = BuildEnv(
        1,
        3,
        {0, 0, 0},
        {State(0, 0, 0)},
        {2});

    std::vector<Action> actions;
    EpibtPlanner::reset();
    EpibtPlanner::initialize(0, &env);
    EpibtPlanner::plan(50, actions, &env);
    if (actions.empty() || actions[0] != Action::FW)
    {
        return false;
    }

    env.curr_states[0] = State(1, 1, 0);
    EpibtPlanner::plan(50, actions, &env);
    return !actions.empty() && actions[0] == Action::FW;
}

} // namespace

int main()
{
    const bool edge_swap_ok = TestEdgeSwap();
    const bool vertex_ok = TestVertexConflict();
    const bool inherited_ok = TestInheritedOperation();

    if (!edge_swap_ok)
    {
        std::cerr << "edge swap smoke test failed\n";
        return 1;
    }
    if (!vertex_ok)
    {
        std::cerr << "vertex conflict smoke test failed\n";
        return 1;
    }
    if (!inherited_ok)
    {
        std::cerr << "inherited operation smoke test failed\n";
        return 1;
    }

    std::cout << "epibt smoke ok\n";
    return 0;
}
