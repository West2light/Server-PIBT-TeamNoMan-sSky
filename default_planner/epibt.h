#pragma once

#include <vector>

#include "ActionModel.h"
#include "SharedEnv.h"
#include "epibt_types.h"

namespace EpibtPlanner
{

void reset();

void initialize(int preprocess_time_limit, SharedEnvironment* env);

void plan(int time_limit_ms, std::vector<Action>& actions, SharedEnvironment* env);

const EpibtStats& last_stats();
const std::vector<AgentPlanTrace>& last_agent_traces();

} // namespace EpibtPlanner
