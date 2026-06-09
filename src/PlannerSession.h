#pragma once
/**
 * PlannerSession
 *
 * Owns a SharedEnvironment + DefaultPlanner lifecycle for one Unity TCP session.
 * One PlannerSession is created per hello handshake and destroyed on shutdown.
 *
 * Thread-safety: NOT thread-safe. PibtTcpServer calls Plan() synchronously
 * from within the client-handling thread.
 */
#include <string>
#include <vector>
#include "SharedEnv.h"
#include "ActionModel.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

enum class PlannerMode
{
    DefaultPlanner,
    Epibt
};

class PlannerSession
{
public:
    explicit PlannerSession(const std::string& session_id);
    ~PlannerSession() = default;

    // Called once on hello message. Returns true on success.
    bool Initialize(const json& hello_msg, int preprocess_time_limit_ms = 3000);

    /**
     * Run one planning step and return the plan_result JSON.
     * Fills in sessionId, requestId, timestep, computeMs, actions[].
     * Falls back to all-Wait on timeout or planner error.
     */
    json Plan(const json& plan_step_msg, int time_limit_ms = 90);

    bool IsInitialized() const { return initialized_; }
    const std::string& SessionId() const { return session_id_; }
    PlannerMode GetPlannerMode() const { return planner_mode_; }
    const std::string& PlannerName() const { return planner_name_; }

private:
    struct ActionTrace
    {
        std::string operation;
        int opIndex = 0;
        std::string debugReason;
    };

    ActionTrace BuildDefaultTrace(Action action) const;

    std::string         session_id_;
    bool                initialized_ = false;
    PlannerMode         planner_mode_ = PlannerMode::DefaultPlanner;
    std::string         planner_name_ = "DefaultPlanner";
    SharedEnvironment   env_;
    int                 request_counter_ = 0;
};
