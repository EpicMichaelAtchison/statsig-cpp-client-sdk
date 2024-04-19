#pragma once

#include "nlohmann/json.hpp"

#include "statsig_user_json.hpp"
#include "internal/initialize_request_args.h"

namespace statsig::data_types::initialize_request_args {

std::string Serialize(const internal::InitializeRequestArgs &args) {
  auto j = nlohmann::json{
      {"hash", args.hash},
      {"user", statsig_user::ToJson(args.user)},
      {"statsigMetadata", args.statsig_metadata},
      {"sinceTime", args.since_time},
  };

  return j.dump();
}

}