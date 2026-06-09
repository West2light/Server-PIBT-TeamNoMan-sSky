#include "PlannerSession.h"
#include "UnityStartKitAdapter.h"
#include "planner.h"   // DefaultPlanner::initialize / plan
#include <iostream>
#include <chrono>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

PlannerSession::PlannerSession(const std::string& session_id)
    : session_id_(session_id)
{
}

bool PlannerSession::Initialize(const json& hello_msg, int preprocess_time_limit_ms)
{
    try
    {
        // 1. Parse map from hello DTO
        if (!hello_msg.contains("map") || !hello_msg["map"].is_object())
        {
            std::cerr << "[PlannerSession] Initialize: missing 'map' in hello\n";
            return false;
        }
        UnityAdapter::ApplyMap(env_, hello_msg["map"]);

        const int n = hello_msg.value("teamSize", 0);
        if (n <= 0)
        {
            std::cerr << "[PlannerSession] Initialize: teamSize must be > 0\n";
            return false;
        }
        env_.num_of_agents = n;
        env_.curr_states.assign(n, State(0, 0, 0));
        env_.goal_locations.assign(n, {{0, 0}});
        env_.curr_timestep = 0;

        // 2. Initialize DefaultPlanner (preprocessing)
        env_.plan_start_time = Clock::now();
        DefaultPlanner::initialize(preprocess_time_limit_ms, &env_);

        initialized_ = true;
        std::cout << "[PlannerSession] Initialized session=" << session_id_
                  << " agents=" << n
                  << " map=" << env_.cols << "x" << env_.rows << std::endl;
        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[PlannerSession] Initialize exception: " << ex.what() << "\n";
        return false;
    }
}

json PlannerSession::Plan(const json& plan_step_msg, int time_limit_ms)
{
    const std::string session_id = plan_step_msg.value("sessionId", "");
    const int request_id = plan_step_msg.value("requestId", ++request_counter_);
    const int timestep   = plan_step_msg.value("timestep",  0);

    json result;
    result["type"]      = "plan_result";
    result["sessionId"] = session_id;
    result["requestId"] = request_id;
    result["timestep"]  = timestep;
    result["timeout"]   = false;
    result["errors"]    = json::array();
    result["actions"]   = json::array();

    if (!initialized_)
    {
        result["errors"].push_back("session not initialized");
        result["computeMs"] = 0.0;
        return result;
    }

    if (!plan_step_msg.contains("agents") || !plan_step_msg["agents"].is_array())
    {
        result["errors"].push_back("missing agents array");
        result["computeMs"] = 0.0;
        return result;
    }

    try
    {
        // 1. Load agent states into env
        UnityAdapter::ApplyAgents(env_, plan_step_msg["agents"], timestep);

        // 2. Run planner
        const auto t0 = Clock::now();
        env_.plan_start_time = t0;

        std::vector<Action> actions(env_.num_of_agents, Action::W);
        DefaultPlanner::plan(time_limit_ms, actions, &env_);

        const double compute_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        // 3. Check timeout
        if (compute_ms >= static_cast<double>(time_limit_ms))
        {
            result["timeout"] = true;
            std::cerr << "[PlannerSession] plan timeout: " << compute_ms << "ms\n";
        }

        // 4. Build actions array
        for (int i = 0; i < env_.num_of_agents; ++i)
        {
            const State& s = env_.curr_states[i];
            const Action  raw_action = (i < (int)actions.size()) ? actions[i] : Action::W;
            std::string   sanitize_reason;
            const Action  a = UnityAdapter::SanitizeAction(env_, s, raw_action, &sanitize_reason);
            const int     raw_next_loc = UnityAdapter::NextLoc(s, raw_action, env_.cols);

            if (a != raw_action)
            {
                std::cerr << "[PlannerSession] sanitize req=" << request_id
                          << " agent=" << i
                          << " loc=" << s.location
                          << " ori=" << s.orientation
                          << " rawAction=" << UnityAdapter::ActionToString(raw_action)
                          << " rawNextLoc=" << raw_next_loc
                          << " reason=" << sanitize_reason
                          << std::endl;
            }

            json entry;
            entry["id"]      = i;
            entry["action"]  = UnityAdapter::ActionToString(a);
            entry["nextLoc"] = UnityAdapter::NextLoc(s, a, env_.cols);
            result["actions"].push_back(entry);
        }

        result["computeMs"] = compute_ms;

        std::cout << "[PlannerSession] plan req=" << request_id
                  << " t=" << timestep
                  << " agents=" << env_.num_of_agents
                  << " computeMs=" << compute_ms << std::endl;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[PlannerSession] Plan exception: " << ex.what() << "\n";
        result["errors"].push_back(ex.what());
        result["computeMs"] = 0.0;

        // Fall back: all Wait actions
        result["actions"] = json::array();
        for (int i = 0; i < env_.num_of_agents; ++i)
        {
            json entry;
            entry["id"]      = i;
            entry["action"]  = "W";
            entry["nextLoc"] = (i < (int)env_.curr_states.size())
                                   ? env_.curr_states[i].location : 0;
            result["actions"].push_back(entry);
        }
    }

    return result;
}
