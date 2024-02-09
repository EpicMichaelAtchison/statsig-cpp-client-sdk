#pragma once

#include "statsig.h"

namespace statsig {

class StatsigContext;

class StatsigClient {
 public:
  static StatsigClient &Shared();

  void Initialize(
      const string &sdk_key,
      const optional<StatsigUser> &user = nullopt,
      const optional<StatsigOptions> &options = nullopt
  );

  void Shutdown();

  void UpdateUser(const StatsigUser &user);

  void LogEvent(const StatsigEvent &event);

  bool CheckGate(const string &gate_name);

  FeatureGate GetFeatureGate(const string &gate_name);

  DynamicConfig GetDynamicConfig(const string &config_name);

  Experiment GetExperiment(const string &experiment_name);

  Layer GetLayer(const string &layer_name);

 private:
  unique_ptr<StatsigContext> context_;

  void SetValuesFromNetwork();
  bool EnsureInitialized(const char *caller);
  void SwitchUser(const StatsigUser &user);
};

}
