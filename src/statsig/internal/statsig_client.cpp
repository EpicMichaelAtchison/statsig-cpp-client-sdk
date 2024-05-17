#include "../statsig_client.h"

#include <memory>

#include "../statsig_event.h"
#include "statsig_context.hpp"
#include "evaluation_details_internal.hpp"
#include "statsig_compat/async/async_helper.hpp"
#include "statsig_compat/primitives/string.hpp"
#include "statsig_compat/output_logger/log.hpp"
#include "macros.hpp"
#include "diagnostics.hpp"

#define INIT_GUARD(result)            \
  do                                  \
  {                                   \
    if (!EnsureInitialized(__func__)) \
    {                                 \
      return result;                  \
    }                                 \
  } while (0)

#define EB_WITH_TAG(tag, context, task, recover) context->err_boundary.Capture((tag), (task), (recover))
#define EB(context, task) EB_WITH_TAG(__func__, context, task, std::nullopt)
#define EBR(context, recover, task) EB_WITH_TAG(__func__, context, task, recover)

namespace statsig {

namespace /* private */ {
using namespace statsig::internal;
using AsyncHelper = statsig_compatibility::AsyncHelper;
using Log = statsig_compatibility::Log;

template<typename T>
std::optional<String> GetCompatGroupName(T evaluation) {
  const auto group_name_actual = UNWRAP(evaluation, group_name);
  std::optional<String> group_name = std::nullopt;
  if (group_name_actual.has_value()) {
    group_name = ToCompat(group_name_actual.value());
  }

  return group_name;
}


}

StatsigClient &StatsigClient::Shared() {
  static StatsigClient inst;
  return inst;
}

StatsigClient::StatsigClient() {
  context_.reset();
}

StatsigClient::~StatsigClient() {
  context_.reset();
}

StatsigResultCode StatsigClient::InitializeSync(
    const String &sdk_key,
    const std::optional<StatsigUser> &user,
    const std::optional<StatsigOptions> &options) {
  const auto actual_key = FromCompat(sdk_key);
  if (actual_key.empty()) {
    return InvalidSdkKey;
  }

  context_ = std::make_shared<StatsigContext>(actual_key, user, options);

  return EB(context_, ([this, &actual_key, &user, &options] {
    AsyncHelper::Get(actual_key)->Start();

    auto diag = Diagnostics::Get(actual_key);
    diag->Mark(markers::OverallStart());

    auto result = UpdateUserSync(context_->user);

    diag->Mark(markers::OverallEnd(
        result,
        context_->store.GetSourceDetails())
    );
    return result;
  }));
}

void StatsigClient::InitializeAsync(
    const String &sdk_key,
    const std::function<void(StatsigResultCode)> &callback,
    const std::optional<StatsigUser> &user,
    const std::optional<StatsigOptions> &options) {
  const auto actual_key = FromCompat(sdk_key);
  if (actual_key.empty()) {
    callback(InvalidSdkKey);
    return;
  }
  context_ = std::make_shared<StatsigContext>(actual_key, user, options);

  EBR(context_, callback, ([this, &actual_key, &callback]() {
    AsyncHelper::Get(actual_key)->Start();

    auto diag = Diagnostics::Get(actual_key);
    diag->Mark(markers::OverallStart());

    std::weak_ptr<StatsigContext> weak_ctx = context_;
    UpdateUserAsync(context_->user, [callback, diag, weak_ctx](const StatsigResultCode code) {
      auto shared_ctx = weak_ctx.lock();
      if (shared_ctx) {
        diag->Mark(markers::OverallEnd(
            code,
            shared_ctx->store.GetSourceDetails())
        );
      }

      callback(code);
    });
    return Ok;
  }));
}

StatsigResultCode StatsigClient::UpdateUserSync(const StatsigUser &user) {
  INIT_GUARD(ClientUninitialized);

  const auto tag = __func__;
  return EB(context_, ([this, &user, tag] {
    context_->user = internal::NormalizeUser(user, context_->options);
    context_->store.Reset();
    Diagnostics::Get(context_->sdk_key)->SetUser(user);

    auto result = context_->data_adapter->GetDataSync(context_->user);
    if (result.code == Ok) {
      context_->store.SetValuesFromDataAdapterResult(result.value);
    }
    context_->store.Finalize();

    context_->err_boundary.HandleBadResult(tag, result.code, result.extra);

    std::weak_ptr<StatsigContext> weak_ctx = context_;
    AsyncHelper::Get(context_->sdk_key)->RunInBackground([weak_ctx, result]() {
      USE_REF(weak_ctx, shared_ctx);

      shared_ctx->data_adapter->GetDataAsync(
          shared_ctx->user,
          result.value,
          [](auto) {}
      );
    });

    return result.code;
  }));
}

void StatsigClient::UpdateUserAsync(
    const StatsigUser &user,
    const std::function<void(StatsigResultCode)> &callback) {
  INIT_GUARD();
  const auto tag = __func__;
  EBR(context_, callback, ([this, &user, &callback, tag] {
    context_->user = internal::NormalizeUser(user, context_->options);
    context_->store.Reset();
    Diagnostics::Get(context_->sdk_key)->SetUser(user);

    auto result = context_->data_adapter->GetDataSync(context_->user);
    if (result.code == Ok) {
      context_->store.SetValuesFromDataAdapterResult(result.value);
    }

    const auto initiator = context_->user;
    std::weak_ptr<StatsigContext> weak_ctx = context_;
    AsyncHelper::Get(context_->sdk_key)->RunInBackground([weak_ctx, tag, initiator, result, callback]() {
      USE_REF(weak_ctx, shared_ctx);

      const auto inner_callback =
          [weak_ctx, tag, initiator, callback](auto result) {
            USE_REF(weak_ctx, shared_ctx);

            const auto current_user = shared_ctx->user;
            if (result.code == Ok && internal::AreUsersEqual(initiator, current_user)) {
              shared_ctx->store.SetValuesFromDataAdapterResult(result.value);
            }

            shared_ctx->store.Finalize();
            shared_ctx->err_boundary.HandleBadResult(tag, result.code, result.extra);
            callback(result.code);
          };

      shared_ctx->data_adapter->GetDataAsync(
          initiator,
          result.value,
          inner_callback
      );
    });

    return Ok;
  }));
}

StatsigResultCode StatsigClient::Shutdown(const time_t timeout_ms) {
  INIT_GUARD(ClientUninitialized);

  Log::Debug("Shutting down StatsigClient with timeout_ms: " + std::to_string(timeout_ms));
  const auto start = Time::now();
  const auto tag = __func__;
  const auto result = EB(context_, ([this, timeout_ms, start, tag]() {
    context_->logger.Shutdown();
    Diagnostics::Shutdown(context_->sdk_key);
    ErrorBoundary::Shutdown(context_->sdk_key);
    const auto thread_shutdown = AsyncHelper::Get(context_->sdk_key)->Shutdown(timeout_ms);

    const auto end = Time::now();
    const auto duration = end - start;
    const auto duration_str = std::to_string(duration);
    Log::Debug("StatsigClient shutdown in " + duration_str + "ms");

    if (!thread_shutdown && duration > constants::kShutdownTimeoutErrorThresholdMs) {
      context_->err_boundary.HandleBadResult(
          tag,
          ShutdownFailureDanglingThreads,
          std::unordered_map<std::string, std::string>{{constants::kShutdownTimeoutExtra, duration_str}}
      );
    }

    return thread_shutdown ? Ok : ShutdownFailureDanglingThreads;
  }));

  context_.reset();
  return result;
}

void StatsigClient::Flush() {
  INIT_GUARD();

  EB(context_, ([this]() {
    context_->logger.Flush();

    return Ok;
  }));
}

void StatsigClient::LogEvent(const StatsigEvent &event) {
  INIT_GUARD();

  EB(context_, ([this, &event]() {
    context_->logger.Enqueue(internal::InternalizeEvent(event, context_->user));

    return Ok;
  }));
}

bool StatsigClient::CheckGate(const String &gate_name) {
  INIT_GUARD(false);

  auto gate = GetFeatureGate(gate_name);
  return gate.GetValue();
}

FeatureGate StatsigClient::GetFeatureGate(const String &gate_name) {
  FeatureGate result(gate_name, internal::evaluation_details::Uninitialized());
  INIT_GUARD(result);

  EB(context_, ([this, &gate_name, &result]() {
    const auto gate_name_actual = FromCompat(gate_name);
    const auto gate = context_->store.GetGate(gate_name_actual);

    context_->logger.Enqueue(
        internal::MakeGateExposure(
            gate_name_actual,
            context_->user,
            gate
        )
    );

    result = FeatureGate(
        gate_name,
        ToCompat(UNWRAP(gate.evaluation, rule_id)),
        std::nullopt,
        gate.details,
        UNWRAP(gate.evaluation, value)
    );

    return Ok;
  }));

  return result;
}

DynamicConfig StatsigClient::GetDynamicConfig(const String &config_name) {
  DynamicConfig result(config_name,
                       internal::evaluation_details::Uninitialized());
  INIT_GUARD(result);

  EB(context_, ([this, &config_name, &result] {
    const auto config_name_actual = FromCompat(config_name);
    const auto config = context_->store.GetConfig(config_name_actual);

    context_->logger.Enqueue(
        internal::MakeConfigExposure(
            config_name_actual,
            context_->user,
            config
        )
    );

    result = DynamicConfig(
        config_name,
        ToCompat(UNWRAP(config.evaluation, rule_id)),
        std::nullopt,
        config.details,
        UNWRAP(config.evaluation, value)
    );

    return Ok;
  }));

  return result;
}

Experiment StatsigClient::GetExperiment(const String &experiment_name) {
  Experiment result(experiment_name,
                    internal::evaluation_details::Uninitialized());
  INIT_GUARD(result);

  EB(context_, ([this, &experiment_name, &result]() {
    const auto exp_name_actual = FromCompat(experiment_name);
    auto experiment = context_->store.GetConfig(exp_name_actual);
    context_->logger.Enqueue(
        internal::MakeConfigExposure(
            exp_name_actual,
            context_->user,
            experiment
        )
    );

    result = Experiment(
        experiment_name,
        ToCompat(UNWRAP(experiment.evaluation, rule_id)),
        GetCompatGroupName(experiment.evaluation),
        experiment.details,
        UNWRAP(experiment.evaluation, value)
    );

    return Ok;
  }));

  return result;
}

Layer StatsigClient::GetLayer(const String &layer_name) {
  Layer result(layer_name, internal::evaluation_details::Uninitialized());
  INIT_GUARD(result);

  EB(context_, ([this, &layer_name, &result] {
    auto logger = &context_->logger;
    auto user = context_->user;

    const auto layer_name_actual = FromCompat(layer_name);
    const auto layer = context_->store.GetLayer(layer_name_actual);

    auto log_exposure = [layer_name_actual, layer, user, logger](
        const std::string &param_name
    ) {
      auto expo = internal::MakeLayerParamExposure(
          layer_name_actual,
          param_name,
          user,
          layer
      );
      logger->Enqueue(expo);
    };

    result = Layer(
        layer_name,
        ToCompat(UNWRAP(layer.evaluation, rule_id)),
        GetCompatGroupName(layer.evaluation),
        layer.details,
        UNWRAP(layer.evaluation, value),
        log_exposure
    );

    return Ok;
  }));

  return result;
}

bool StatsigClient::EnsureInitialized(const char *caller) {
  if (context_ != nullptr && !context_->sdk_key.empty()) {
    return true;
  }

  std::cerr << "[Statsig]: Call made to StatsigClient::" << caller << " before StatsigClient::Initialize" << std::endl;
  return false;
}

}
