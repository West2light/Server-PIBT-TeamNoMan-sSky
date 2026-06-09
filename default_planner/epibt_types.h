#pragma once

#include <array>
#include <string>
#include <vector>

#include "ActionModel.h"
#include "States.h"

namespace EpibtPlanner
{

struct Operation
{
    std::array<Action, 3> actions {Action::W, Action::W, Action::W};
    int beta = 0;

    bool operator==(const Operation& other) const
    {
        return actions == other.actions;
    }
};

struct OperationPath
{
    std::array<State, 4> states;
};

struct EpibtStats
{
    int revisitLimit = 0;
    int revisitCount = 0;
    int fallbackInherited = 0;
    int acceptedInherited = 0;
    int multiConflictSkipped = 0;
};

struct AgentPlanTrace
{
    int agentId = -1;
    Operation operation;
    int opIndex = 0;
    std::string debugReason;
};

} // namespace EpibtPlanner
