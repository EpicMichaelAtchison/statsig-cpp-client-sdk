#pragma once

#include <string>
#include "statsig/statsig_user.h"

namespace statsig::internal {

struct InitializeRequestArgs {
  std::string hash;
  StatsigUser user;
};

}