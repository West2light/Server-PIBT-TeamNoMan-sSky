#include "PlannerSession.h"
#include "FileLogger.h"
#include "UnityStartKitAdapter.h"
#include "epibt.h"
#include "planner.h"   // DefaultPlanner::initialize / plan

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

namespace
{

std::string ToLowerCopy(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

PlannerMode ParsePlannerMode(const std::string& raw, std::string* planner_name)
{
    const std::string value = ToLowerCopy(raw);
    if (value == "epibt" || value == "epibt3")
    {
        if (planner_name != nullptr)
        {
            *planner_name = "EPIBT3";
        }
        return PlannerMode::Epibt;
    }

    if (planner_name != nullptr)
    {
        *planner_name = "DefaultPlanner";
    }
    return PlannerMode::DefaultPlanner;
}

PlannerMode ResolvePlannerMode(const json& hello_msg, std::string* planner_name)
{
    if (hello_msg.contains("plannerMode") && hello_msg["plannerMode"].is_string())
    {
        return ParsePlannerMode(hello_msg["plannerMode"].get<std::string>(), planner_name);
    }

    if (const char* env_value = std::getenv("PIBT_TCP_PLANNER"))
    {
        return ParsePlannerMode(env_value, planner_name);
    }

    return ParsePlannerMode("default", planner_name);
}

} // namespace

PlannerSession::ActionTrace PlannerSession::BuildDefaultTrace(Action action) const
{
    ActionTrace trace;
    trace.operation =
        std::string(UnityAdapter::ActionToString(action)) + ",W,W";
    trace.opIndex = 0;
    trace.debugReason = "default_single_step";
    return trace;
}

PlannerSession::PlannerSession(const std::string& session_id)
    : session_id_(session_id)
{
}

bool PlannerSession::Initialize(const json& hello_msg, int preprocess_time_limit_ms)
{
    try
    {
        FileLogger::Info(
            "[PlannerSession] initialize begin session=" + session_id_ +
            " preprocessMs=" + std::to_string(preprocess_time_limit_ms));

        // 1. Parse map from hello DTO
        if (!hello_msg.contains("map") || !hello_msg["map"].is_object())
        {
            std::cerr << "[PlannerSession] Initialize: missing 'map' in hello\n";
            FileLogger::Error("[PlannerSession] initialize failed session=" + session_id_ + " reason=missing map");
            return false;
        }
        UnityAdapter::ApplyMap(env_, hello_msg["map"]);

        const int n = hello_msg.value("teamSize", 0);
        if (n <= 0)
        {
            std::cerr << "[PlannerSession] Initialize: teamSize must be > 0\n";
            FileLogger::Error("[PlannerSession] initialize failed session=" + session_id_ + " reason=invalid teamSize");
            return false;
        }
        env_.num_of_agents = n;
        env_.curr_states.assign(n, State(0, 0, 0));
        env_.goal_locations.assign(n, {{0, 0}});
        env_.curr_timestep = 0;

        planner_mode_ = ResolvePlannerMode(hello_msg, &planner_name_);

        // 2. Initialize selected planner (preprocessing)
        env_.plan_start_time = Clock::now();
        if (planner_mode_ == PlannerMode::Epibt)
        {
            EpibtPlanner::reset();
            EpibtPlanner::initialize(preprocess_time_limit_ms, &env_);
        }
        else
        {
            DefaultPlanner::reset();
            DefaultPlanner::initialize(preprocess_time_limit_ms, &env_);
        }

        initialized_ = true;
        std::cout << "[PlannerSession] Initialized session=" << session_id_
                  << " agents=" << n
                  << " map=" << env_.cols << "x" << env_.rows
                  << " planner=" << planner_name_ << std::endl;
        FileLogger::Info(
            "[PlannerSession] initialize ok session=" + session_id_ +
            " agents=" + std::to_string(n) +
            " map=" + std::to_string(env_.cols) + "x" + std::to_string(env_.rows) +
            " planner=" + planner_name_);
        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[PlannerSession] Initialize exception: " << ex.what() << "\n";
        FileLogger::Error(
            "[PlannerSession] initialize exception session=" + session_id_ +
            " error=" + ex.what());
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
    result["planner"]   = planner_name_;
    result["opLen"]     = (planner_mode_ == PlannerMode::Epibt) ? 3 : 1;
    result["revisitLimit"] = 0;
    result["fallbackInherited"] = 0;
    result["multiConflictSkipped"] = 0;
    result["errors"]    = json::array();
    result["actions"]   = json::array();

    if (!initialized_)
    {
        result["errors"].push_back("session not initialized");
        result["computeMs"] = 0.0;
        FileLogger::Warn(
            "[PlannerSession] plan rejected session=" + session_id_ +
            " requestId=" + std::to_string(request_id) +
            " reason=session not initialized");
        return result;
    }

    if (!plan_step_msg.contains("agents") || !plan_step_msg["agents"].is_array())
    {
        result["errors"].push_back("missing agents array");
        result["computeMs"] = 0.0;
        FileLogger::Warn(
            "[PlannerSession] plan rejected session=" + session_id_ +
            " requestId=" + std::to_string(request_id) +
            " reason=missing agents array");
        return result;
    }

    try
    {
        FileLogger::Info(
            "[PlannerSession] plan begin session=" + session_id_ +
            " requestId=" + std::to_string(request_id) +
            " timestep=" + std::to_string(timestep));

        // 1. Load agent states into env
        UnityAdapter::ApplyAgents(env_, plan_step_msg["agents"], timestep);

        // 2. Run planner
        const auto t0 = Clock::now();
        env_.plan_start_time = t0;

        std::vector<Action> actions(env_.num_of_agents, Action::W);
        std::vector<ActionTrace> traces(env_.num_of_agents);
        if (planner_mode_ == PlannerMode::Epibt)
        {
            EpibtPlanner::plan(time_limit_ms, actions, &env_);
            const EpibtPlanner::EpibtStats& stats = EpibtPlanner::last_stats();
            result["opLen"] = 3;
            result["revisitLimit"] = stats.revisitLimit;
            result["fallbackInherited"] = stats.fallbackInherited;
            result["multiConflictSkipped"] = stats.multiConflictSkipped;

            const auto& agent_traces = EpibtPlanner::last_agent_traces();
            for (std::size_t i = 0; i < traces.size(); ++i)
            {
                traces[i] = BuildDefaultTrace((i < actions.size()) ? actions[i] : Action::W);
                traces[i].debugReason = "epibt_trace_missing";
            }
            for (const auto& agent_trace : agent_traces)
            {
                if (agent_trace.agentId < 0 ||
                    agent_trace.agentId >= static_cast<int>(traces.size()))
                {
                    continue;
                }

                ActionTrace trace;
                trace.operation =
                    std::string(UnityAdapter::ActionToString(agent_trace.operation.actions[0])) + "," +
                    UnityAdapter::ActionToString(agent_trace.operation.actions[1]) + "," +
                    UnityAdapter::ActionToString(agent_trace.operation.actions[2]);
                trace.opIndex = agent_trace.opIndex;
                trace.debugReason = agent_trace.debugReason;
                traces[agent_trace.agentId] = trace;
            }
        }
        else
        {
            DefaultPlanner::plan(time_limit_ms, actions, &env_);
            for (std::size_t i = 0; i < traces.size(); ++i)
            {
                traces[i] = BuildDefaultTrace((i < actions.size()) ? actions[i] : Action::W);
            }
        }

        const double compute_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        // 3. Check timeout
        if (compute_ms >= static_cast<double>(time_limit_ms))
        {
            result["timeout"] = true;
            std::cerr << "[PlannerSession] plan timeout: " << compute_ms << "ms\n";
            FileLogger::Warn(
                "[PlannerSession] timeout session=" + session_id_ +
                " requestId=" + std::to_string(request_id) +
                " timestep=" + std::to_string(timestep) +
                " computeMs=" + std::to_string(compute_ms));
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
                FileLogger::Warn(
                    "[PlannerSession] sanitize session=" + session_id_ +
                    " requestId=" + std::to_string(request_id) +
                    " agent=" + std::to_string(i) +
                    " loc=" + std::to_string(s.location) +
                    " ori=" + std::to_string(s.orientation) +
                    " rawAction=" + UnityAdapter::ActionToString(raw_action) +
                    " rawNextLoc=" + std::to_string(raw_next_loc) +
                    " reason=" + sanitize_reason);
            }

            json entry;
            entry["id"]      = i;
            entry["action"]  = UnityAdapter::ActionToString(a);
            entry["nextLoc"] = UnityAdapter::NextLoc(s, a, env_.cols);
            entry["planner"] = planner_name_;
            entry["operation"] = traces[i].operation;
            entry["opIndex"] = traces[i].opIndex;
            entry["debugReason"] = traces[i].debugReason;
            if (a != raw_action)
            {
                if (!entry["debugReason"].get<std::string>().empty())
                {
                    entry["debugReason"] =
                        entry["debugReason"].get<std::string>() + ";sanitize:" + sanitize_reason;
                }
                else
                {
                    entry["debugReason"] = "sanitize:" + sanitize_reason;
                }
            }
            result["actions"].push_back(entry);
        }

        result["computeMs"] = compute_ms;

        std::cout << "[PlannerSession] plan req=" << request_id
                  << " t=" << timestep
                  << " agents=" << env_.num_of_agents
                  << " computeMs=" << compute_ms
                  << " planner=" << planner_name_ << std::endl;
        FileLogger::Info(
            "[PlannerSession] plan ok session=" + session_id_ +
            " requestId=" + std::to_string(request_id) +
            " timestep=" + std::to_string(timestep) +
            " agents=" + std::to_string(env_.num_of_agents) +
            " computeMs=" + std::to_string(compute_ms) +
            " planner=" + planner_name_ +
            " timeout=" + std::string(result["timeout"].get<bool>() ? "true" : "false"));
    }
    catch (const std::exception& ex)
    {
        // F-S3: include map dims to distinguish geometry errors (wrong dims) from other failures
        std::cerr << "[PlannerSession] Plan exception: " << ex.what()
                  << " map=" << env_.cols << "x" << env_.rows << "\n";
        FileLogger::Error(
            "[PlannerSession] exception session=" + session_id_ +
            " requestId=" + std::to_string(request_id) +
            " timestep=" + std::to_string(timestep) +
            " map=" + std::to_string(env_.cols) + "x" + std::to_string(env_.rows) +
            " error=" + ex.what());
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
