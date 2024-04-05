#pragma once

#include <string>

#include "statsig_user.h"
#include "statsig_options.h"

namespace statsig {

enum class ValueSource {
  Uninitialized,
  Loading,
  NoValues,
  Cache,
  Network,
  Bootstrap
};

std::string GetValueSourceString(const ValueSource &source) {
  switch (source) {
    case ValueSource::Uninitialized:return "Uninitialized";
    case ValueSource::Loading:return "Loading";
    case ValueSource::NoValues:return "NoValues";
    case ValueSource::Cache:return "Cache";
    case ValueSource::Network:return "Network";
    case ValueSource::Bootstrap:return "Bootstrap";
  }

  return "";
}

struct DataAdapterResult {
  ValueSource source;
  std::string data;
  long long receivedAt;
};

class EvaluationsDataAdapter {
 public:
  virtual void Attach(
      std::string &sdk_key,
      StatsigOptions &options
  ) = 0;

  virtual std::optional<DataAdapterResult> GetDataSync(
      const StatsigUser &user
  ) = 0;

  virtual void GetDataAsync(
      const StatsigUser &user,
      const std::optional<DataAdapterResult> &current,
      const std::function<void(std::optional<DataAdapterResult>)> &callback
  ) = 0;

  virtual void SetData(
      const StatsigUser &user,
      const std::string &data
  ) = 0;

  virtual void PrefetchData(
      const StatsigUser &user,
      const std::function<void(void)> &callback
  ) = 0;
};

}

