//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef GRPC_CORE_EXT_CLIENT_CHANNEL_LB_POLICY_LB_POLICY_TEST_LIB_H
#define GRPC_CORE_EXT_CLIENT_CHANNEL_LB_POLICY_LB_POLICY_TEST_LIB_H

#include <grpc/support/port_platform.h>

#include <stddef.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"
#include "gtest/gtest.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/filters/client_channel/subchannel_pool_interface.h"
#include "src/core/lib/address_utils/parse_address.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/event_engine/default_event_engine.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/match.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/gprpp/work_serializer.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/resolved_address.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/load_balancing/lb_policy.h"
#include "src/core/lib/load_balancing/lb_policy_registry.h"
#include "src/core/lib/load_balancing/subchannel_interface.h"
#include "src/core/lib/resolver/server_address.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/uri/uri_parser.h"

namespace grpc_core {
namespace testing {

class LoadBalancingPolicyTest : public ::testing::Test {
 protected:
  // Channel-level subchannel state for a specific address and channel args.
  // This is analogous to the real subchannel in the ClientChannel code.
  class SubchannelState {
   public:
    // A fake SubchannelInterface object, to be returned to the LB
    // policy when it calls the helper's CreateSubchannel() method.
    // There may be multiple FakeSubchannel objects associated with a
    // given SubchannelState object.
    class FakeSubchannel : public SubchannelInterface {
     public:
      FakeSubchannel(SubchannelState* state,
                     std::shared_ptr<WorkSerializer> work_serializer)
          : state_(state), work_serializer_(std::move(work_serializer)) {}

      SubchannelState* state() const { return state_; }

     private:
      // Converts between
      // SubchannelInterface::ConnectivityStateWatcherInterface and
      // ConnectivityStateWatcherInterface.
      class WatcherWrapper : public AsyncConnectivityStateWatcherInterface {
       public:
        WatcherWrapper(
            std::shared_ptr<WorkSerializer> work_serializer,
            std::unique_ptr<
                SubchannelInterface::ConnectivityStateWatcherInterface>
                watcher)
            : AsyncConnectivityStateWatcherInterface(
                  std::move(work_serializer)),
              watcher_(std::move(watcher)) {}

        void OnConnectivityStateChange(grpc_connectivity_state new_state,
                                       const absl::Status& status) override {
          watcher_->OnConnectivityStateChange(new_state, status);
        }

       private:
        std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
            watcher_;
      };

      void WatchConnectivityState(
          std::unique_ptr<
              SubchannelInterface::ConnectivityStateWatcherInterface>
              watcher) override {
        auto watcher_wrapper = MakeOrphanable<WatcherWrapper>(
            work_serializer_, std::move(watcher));
        watcher_map_[watcher.get()] = watcher_wrapper.get();
        MutexLock lock(&state_->mu_);
        state_->state_tracker_.AddWatcher(GRPC_CHANNEL_SHUTDOWN,
                                          std::move(watcher_wrapper));
      }

      void CancelConnectivityStateWatch(
          ConnectivityStateWatcherInterface* watcher) override {
        auto it = watcher_map_.find(watcher);
        if (it == watcher_map_.end()) return;
        MutexLock lock(&state_->mu_);
        state_->state_tracker_.RemoveWatcher(it->second);
        watcher_map_.erase(it);
      }

      void RequestConnection() override {
        MutexLock lock(&state_->requested_connection_mu_);
        state_->requested_connection_ = true;
      }

      // Don't need these methods here, so they're no-ops.
      void ResetBackoff() override {}
      void AddDataWatcher(std::unique_ptr<DataWatcherInterface>) override {}

      SubchannelState* state_;
      std::shared_ptr<WorkSerializer> work_serializer_;
      std::map<SubchannelInterface::ConnectivityStateWatcherInterface*,
               WatcherWrapper*>
          watcher_map_;
    };

    explicit SubchannelState(absl::string_view address)
        : address_(address), state_tracker_("LoadBalancingPolicyTest") {}

    const std::string& address() const { return address_; }

    // Sets the connectivity state for this subchannel.  The updated state
    // will be reported to all associated SubchannelInterface objects.
    void SetConnectivityState(grpc_connectivity_state state,
                              const absl::Status& status = absl::OkStatus()) {
      if (state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
        EXPECT_FALSE(status.ok())
            << "bug in test: TRANSIENT_FAILURE must have non-OK status";
      } else {
        EXPECT_TRUE(status.ok())
            << "bug in test: " << ConnectivityStateName(state)
            << " must have OK status: " << status;
      }
      MutexLock lock(&mu_);
      state_tracker_.SetState(state, status, "set from test");
    }

    // Indicates if any of the associated SubchannelInterface objects
    // have requested a connection attempt since the last time this
    // method was called.
    bool ConnectionRequested() {
      MutexLock lock(&requested_connection_mu_);
      return std::exchange(requested_connection_, false);
    }

    // To be invoked by FakeHelper.
    RefCountedPtr<SubchannelInterface> CreateSubchannel(
        std::shared_ptr<WorkSerializer> work_serializer) {
      return MakeRefCounted<FakeSubchannel>(this, std::move(work_serializer));
    }

   private:
    const std::string address_;
    Mutex mu_;
    ConnectivityStateTracker state_tracker_ ABSL_GUARDED_BY(&mu_);
    Mutex requested_connection_mu_;
    bool requested_connection_ ABSL_GUARDED_BY(&requested_connection_mu_) =
        false;
  };

  // A fake helper to be passed to the LB policy.
  class FakeHelper : public LoadBalancingPolicy::ChannelControlHelper {
   public:
    // Represents a state update reported by the LB policy.
    struct StateUpdate {
      grpc_connectivity_state state;
      absl::Status status;
      RefCountedPtr<LoadBalancingPolicy::SubchannelPicker> picker;

      std::string ToString() const {
        return absl::StrFormat("UPDATE{state=%s, status=%s, picker=%p}",
                               ConnectivityStateName(state), status.ToString(),
                               picker.get());
      }
    };

    // Represents a re-resolution request from the LB policy.
    struct ReresolutionRequested {
      std::string ToString() const { return "RERESOLUTION"; }
    };

    FakeHelper(LoadBalancingPolicyTest* test,
               std::shared_ptr<WorkSerializer> work_serializer)
        : test_(test), work_serializer_(std::move(work_serializer)) {}

    // Called at test tear-down time to ensure that we have not left any
    // unexpected events in the queue.
    void ExpectQueueEmpty(SourceLocation location = SourceLocation()) {
      MutexLock lock(&mu_);
      EXPECT_TRUE(queue_.empty()) << location.file() << ":" << location.line();
      for (const Event& event : queue_) {
        gpr_log(GPR_ERROR, "UNEXPECTED EVENT LEFT IN QUEUE: %s",
                EventString(event).c_str());
      }
    }

    // Returns the next event in the queue if it is a state update.
    // If the queue is empty or the next event is not a state update,
    // fails the test and returns nullopt without removing anything from
    // the queue.
    absl::optional<StateUpdate> GetNextStateUpdate(
        SourceLocation location = SourceLocation()) {
      MutexLock lock(&mu_);
      EXPECT_FALSE(queue_.empty()) << location.file() << ":" << location.line();
      if (queue_.empty()) return absl::nullopt;
      Event& event = queue_.front();
      auto* update = absl::get_if<StateUpdate>(&event);
      EXPECT_NE(update, nullptr)
          << "unexpected event " << EventString(event) << " at "
          << location.file() << ":" << location.line();
      if (update == nullptr) return absl::nullopt;
      StateUpdate result = std::move(*update);
      queue_.pop_front();
      return std::move(result);
    }

    // Returns the next event in the queue if it is a re-resolution.
    // If the queue is empty or the next event is not a re-resolution,
    // fails the test and returns nullopt without removing anything
    // from the queue.
    absl::optional<ReresolutionRequested> GetNextReresolution(
        SourceLocation location = SourceLocation()) {
      MutexLock lock(&mu_);
      EXPECT_FALSE(queue_.empty()) << location.file() << ":" << location.line();
      if (queue_.empty()) return absl::nullopt;
      Event& event = queue_.front();
      auto* reresolution = absl::get_if<ReresolutionRequested>(&event);
      EXPECT_NE(reresolution, nullptr)
          << "unexpected event " << EventString(event) << " at "
          << location.file() << ":" << location.line();
      if (reresolution == nullptr) return absl::nullopt;
      ReresolutionRequested result = *reresolution;
      queue_.pop_front();
      return result;
    }

   private:
    // Represents an event reported by the LB policy.
    using Event = absl::variant<StateUpdate, ReresolutionRequested>;

    // Returns a human-readable representation of an event.
    static std::string EventString(const Event& event) {
      return Match(
          event, [](const StateUpdate& update) { return update.ToString(); },
          [](const ReresolutionRequested& reresolution) {
            return reresolution.ToString();
          });
    }

    RefCountedPtr<SubchannelInterface> CreateSubchannel(
        ServerAddress address, const ChannelArgs& args) override {
      SubchannelKey key(address.address(), args);
      auto it = test_->subchannel_pool_.find(key);
      if (it == test_->subchannel_pool_.end()) {
        auto address_uri = grpc_sockaddr_to_uri(&address.address());
        GPR_ASSERT(address_uri.ok());
        it = test_->subchannel_pool_
                 .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                          std::forward_as_tuple(std::move(*address_uri)))
                 .first;
      }
      return it->second.CreateSubchannel(work_serializer_);
    }

    void UpdateState(
        grpc_connectivity_state state, const absl::Status& status,
        RefCountedPtr<LoadBalancingPolicy::SubchannelPicker> picker) override {
      MutexLock lock(&mu_);
      queue_.push_back(StateUpdate{state, status, std::move(picker)});
    }

    void RequestReresolution() override {
      MutexLock lock(&mu_);
      queue_.push_back(ReresolutionRequested());
    }

    absl::string_view GetAuthority() override { return "server.example.com"; }

    grpc_event_engine::experimental::EventEngine* GetEventEngine() override {
      return grpc_event_engine::experimental::GetDefaultEventEngine().get();
    }

    void AddTraceEvent(TraceSeverity, absl::string_view) override {}

    LoadBalancingPolicyTest* test_;
    std::shared_ptr<WorkSerializer> work_serializer_;
    Mutex mu_;
    std::deque<Event> queue_ ABSL_GUARDED_BY(&mu_);
  };

  // A fake MetadataInterface implementation, for use in PickArgs.
  class FakeMetadata : public LoadBalancingPolicy::MetadataInterface {
   public:
    explicit FakeMetadata(std::map<std::string, std::string> metadata)
        : metadata_(std::move(metadata)) {}

    const std::map<std::string, std::string>& metadata() const {
      return metadata_;
    }

   private:
    void Add(absl::string_view key, absl::string_view value) override {
      metadata_[std::string(key)] = std::string(value);
    }

    std::vector<std::pair<std::string, std::string>> TestOnlyCopyToVector()
        override {
      return {};  // Not used.
    }

    absl::optional<absl::string_view> Lookup(
        absl::string_view key, std::string* /*buffer*/) const override {
      auto it = metadata_.find(std::string(key));
      if (it == metadata_.end()) return absl::nullopt;
      return it->second;
    }

    std::map<std::string, std::string> metadata_;
  };

  // A fake CallState implementation, for use in PickArgs.
  class FakeCallState : public LoadBalancingPolicy::CallState {
   public:
    ~FakeCallState() override {
      for (void* allocation : allocations_) {
        gpr_free(allocation);
      }
    }

   private:
    void* Alloc(size_t size) override {
      void* allocation = gpr_malloc(size);
      allocations_.push_back(allocation);
      return allocation;
    }

    std::vector<void*> allocations_;
  };

  LoadBalancingPolicyTest()
      : work_serializer_(std::make_shared<WorkSerializer>()) {}

  void TearDown() override {
    // Note: Can't safely trigger this from inside the FakeHelper dtor,
    // because if there is a picker in the queue that is holding a ref
    // to the LB policy, that will prevent the LB policy from being
    // destroyed, and therefore the FakeHelper will not be destroyed.
    // (This will cause an ASAN failure, but it will not display the
    // queued events, so the failure will be harder to diagnose.)
    helper_->ExpectQueueEmpty();
  }

  // Creates an LB policy of the specified name.
  // Creates a new FakeHelper for the new LB policy, and sets helper_ to
  // point to the FakeHelper.
  OrphanablePtr<LoadBalancingPolicy> MakeLbPolicy(absl::string_view name) {
    auto helper = std::make_unique<FakeHelper>(this, work_serializer_);
    helper_ = helper.get();
    LoadBalancingPolicy::Args args = {work_serializer_, std::move(helper),
                                      ChannelArgs()};
    return CoreConfiguration::Get()
        .lb_policy_registry()
        .CreateLoadBalancingPolicy(name, std::move(args));
  }

  // Creates an LB policy config from json.
  static RefCountedPtr<LoadBalancingPolicy::Config> MakeConfig(
      const Json& json, SourceLocation location = SourceLocation()) {
    auto status_or_config =
        CoreConfiguration::Get().lb_policy_registry().ParseLoadBalancingConfig(
            json);
    EXPECT_TRUE(status_or_config.ok())
        << status_or_config.status() << "\n"
        << location.file() << ":" << location.line();
    return status_or_config.value();
  }

  // Converts an address URI into a grpc_resolved_address.
  static grpc_resolved_address MakeAddress(absl::string_view address_uri) {
    auto uri = URI::Parse(address_uri);
    GPR_ASSERT(uri.ok());
    grpc_resolved_address address;
    GPR_ASSERT(grpc_parse_uri(*uri, &address));
    return address;
  }

  // Constructs an update containing a list of addresses.
  LoadBalancingPolicy::UpdateArgs BuildUpdate(
      absl::Span<const absl::string_view> addresses,
      RefCountedPtr<LoadBalancingPolicy::Config> config = nullptr) {
    LoadBalancingPolicy::UpdateArgs update;
    update.addresses.emplace();
    for (const absl::string_view& address : addresses) {
      update.addresses->emplace_back(MakeAddress(address), ChannelArgs());
    }
    update.config = std::move(config);
    return update;
  }

  // Applies the update on the LB policy.
  absl::Status ApplyUpdate(LoadBalancingPolicy::UpdateArgs update_args,
                           LoadBalancingPolicy* lb_policy) {
    absl::Status status;
    absl::Notification notification;
    work_serializer_->Run(
        [&]() {
          status = lb_policy->UpdateLocked(std::move(update_args));
          notification.Notify();
        },
        DEBUG_LOCATION);
    notification.WaitForNotification();
    return status;
  }

  // Keeps reading state updates until continue_predicate() returns false.
  // Returns false if the helper reports no events or if the event is
  // not a state update; otherwise (if continue_predicate() tells us to
  // stop) returns true.
  bool WaitForStateUpdate(
      std::function<bool(FakeHelper::StateUpdate update)> continue_predicate,
      SourceLocation location = SourceLocation()) {
    while (true) {
      auto update = helper_->GetNextStateUpdate(location);
      if (!update.has_value()) return false;
      if (!continue_predicate(std::move(*update))) return true;
    }
  }

  // Expects that the LB policy has reported the specified connectivity
  // state to helper_.  Returns the picker from the state update.
  RefCountedPtr<LoadBalancingPolicy::SubchannelPicker> ExpectState(
      grpc_connectivity_state expected_state,
      absl::Status expected_status = absl::OkStatus(),
      SourceLocation location = SourceLocation()) {
    auto update = helper_->GetNextStateUpdate(location);
    if (!update.has_value()) return nullptr;
    EXPECT_EQ(update->state, expected_state)
        << "got " << ConnectivityStateName(update->state) << ", expected "
        << ConnectivityStateName(expected_state) << "\n"
        << "at " << location.file() << ":" << location.line();
    EXPECT_EQ(update->status, expected_status)
        << update->status << "\n"
        << location.file() << ":" << location.line();
    EXPECT_NE(update->picker, nullptr)
        << location.file() << ":" << location.line();
    return std::move(update->picker);
  }

  // Waits for the LB policy to get connected, then returns the final
  // picker.  There can be any number of CONNECTING updates, each of
  // which must return a picker that queues picks, followed by one
  // update for state READY, whose picker is returned.
  RefCountedPtr<LoadBalancingPolicy::SubchannelPicker> WaitForConnected(
      SourceLocation location = SourceLocation()) {
    RefCountedPtr<LoadBalancingPolicy::SubchannelPicker> final_picker;
    WaitForStateUpdate(
        [&](FakeHelper::StateUpdate update) {
          if (update.state == GRPC_CHANNEL_CONNECTING) {
            EXPECT_TRUE(update.status.ok())
                << update.status << " at " << location.file() << ":"
                << location.line();
            ExpectPickQueued(update.picker.get(), location);
            return true;  // Keep going.
          }
          EXPECT_EQ(update.state, GRPC_CHANNEL_READY)
              << ConnectivityStateName(update.state) << " at "
              << location.file() << ":" << location.line();
          final_picker = std::move(update.picker);
          return false;  // Stop.
        },
        location);
    return final_picker;
  }

  // Waits for the LB policy to fail a connection attempt.  There can be
  // any number of CONNECTING updates, each of which must return a picker
  // that queues picks, followed by one update for state TRANSIENT_FAILURE,
  // whose status is passed to check_status() and whose picker must fail
  // picks with a status that is passed to check_status().
  // Returns true if the reported states match expectations.
  bool WaitForConnectionFailed(
      std::function<void(const absl::Status&)> check_status,
      SourceLocation location = SourceLocation()) {
    bool retval = false;
    WaitForStateUpdate([&](FakeHelper::StateUpdate update) {
      if (update.state == GRPC_CHANNEL_CONNECTING) {
        EXPECT_TRUE(update.status.ok())
            << update.status << " at " << location.file() << ":"
            << location.line();
        ExpectPickQueued(update.picker.get(), location);
        return true;  // Keep going.
      }
      EXPECT_EQ(update.state, GRPC_CHANNEL_TRANSIENT_FAILURE)
          << ConnectivityStateName(update.state) << " at " << location.file()
          << ":" << location.line();
      check_status(update.status);
      ExpectPickFail(update.picker.get(), check_status, location);
      retval = update.state == GRPC_CHANNEL_TRANSIENT_FAILURE;
      return false;  // Stop.
    });
    return retval;
  }

  // Expects a state update for the specified state and status, and then
  // expects the resulting picker to queue picks.
  void ExpectStateAndQueuingPicker(
      grpc_connectivity_state expected_state,
      absl::Status expected_status = absl::OkStatus(),
      SourceLocation location = SourceLocation()) {
    auto picker = ExpectState(expected_state, expected_status, location);
    ExpectPickQueued(picker.get(), location);
  }

  // Convenient frontend to ExpectStateAndQueuingPicker() for CONNECTING.
  void ExpectConnectingUpdate(SourceLocation location = SourceLocation()) {
    ExpectStateAndQueuingPicker(GRPC_CHANNEL_CONNECTING, absl::OkStatus(),
                                location);
  }

  // Does a pick and returns the result.
  LoadBalancingPolicy::PickResult DoPick(
      LoadBalancingPolicy::SubchannelPicker* picker) {
    ExecCtx exec_ctx;
    FakeMetadata metadata({});
    FakeCallState call_state;
    return picker->Pick({"/service/method", &metadata, &call_state});
  }

  // Requests a pick on picker and expects a Queue result.
  void ExpectPickQueued(LoadBalancingPolicy::SubchannelPicker* picker,
                        SourceLocation location = SourceLocation()) {
    auto pick_result = DoPick(picker);
    ASSERT_TRUE(absl::holds_alternative<LoadBalancingPolicy::PickResult::Queue>(
        pick_result.result))
        << PickResultString(pick_result) << " at " << location.file() << ":"
        << location.line();
  }

  // Requests a pick on picker and expects a Complete result.
  // The address of the resulting subchannel is returned, or nullopt if
  // the result was something other than Complete.
  absl::optional<std::string> ExpectPickComplete(
      LoadBalancingPolicy::SubchannelPicker* picker,
      SourceLocation location = SourceLocation()) {
    auto pick_result = DoPick(picker);
    auto* complete = absl::get_if<LoadBalancingPolicy::PickResult::Complete>(
        &pick_result.result);
    EXPECT_NE(complete, nullptr) << PickResultString(pick_result) << " at "
                                 << location.file() << ":" << location.line();
    if (complete == nullptr) return absl::nullopt;
    auto* subchannel = static_cast<SubchannelState::FakeSubchannel*>(
        complete->subchannel.get());
    return subchannel->state()->address();
  }

  // Requests a picker on picker and expects a Fail result.
  // The failing status is passed to check_status.
  void ExpectPickFail(LoadBalancingPolicy::SubchannelPicker* picker,
                      std::function<void(const absl::Status&)> check_status,
                      SourceLocation location = SourceLocation()) {
    auto pick_result = DoPick(picker);
    auto* fail = absl::get_if<LoadBalancingPolicy::PickResult::Fail>(
        &pick_result.result);
    ASSERT_NE(fail, nullptr) << PickResultString(pick_result) << " at "
                             << location.file() << ":" << location.line();
    check_status(fail->status);
  }

  // Returns a human-readable string for a pick result.
  static std::string PickResultString(
      const LoadBalancingPolicy::PickResult& result) {
    return Match(
        result.result,
        [](const LoadBalancingPolicy::PickResult::Complete& complete) {
          auto* subchannel = static_cast<SubchannelState::FakeSubchannel*>(
              complete.subchannel.get());
          return absl::StrFormat(
              "COMPLETE{subchannel=%s, subchannel_call_tracker=%p}",
              subchannel->state()->address(),
              complete.subchannel_call_tracker.get());
        },
        [](const LoadBalancingPolicy::PickResult::Queue&) -> std::string {
          return "QUEUE{}";
        },
        [](const LoadBalancingPolicy::PickResult::Fail& fail) -> std::string {
          return absl::StrFormat("FAIL{%s}", fail.status.ToString());
        },
        [](const LoadBalancingPolicy::PickResult::Drop& drop) -> std::string {
          return absl::StrFormat("FAIL{%s}", drop.status.ToString());
        });
  }

  // Returns the entry in the subchannel pool, or null if not present.
  SubchannelState* FindSubchannel(absl::string_view address,
                                  const ChannelArgs& args = ChannelArgs()) {
    SubchannelKey key(MakeAddress(address), args);
    auto it = subchannel_pool_.find(key);
    if (it == subchannel_pool_.end()) return nullptr;
    return &it->second;
  }

  // Creates and returns an entry in the subchannel pool.
  // This can be used in cases where we want to test that a subchannel
  // already exists when the LB policy creates it (e.g., due to it being
  // created by another channel and shared via the global subchannel
  // pool, or by being created by another LB policy in this channel).
  SubchannelState* CreateSubchannel(absl::string_view address,
                                    const ChannelArgs& args = ChannelArgs()) {
    SubchannelKey key(MakeAddress(address), args);
    auto it = subchannel_pool_
                  .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                           std::forward_as_tuple(address))
                  .first;
    return &it->second;
  }

  void ExpectQueueEmpty(SourceLocation location = SourceLocation()) {
    helper_->ExpectQueueEmpty(location);
  }

  std::shared_ptr<WorkSerializer> work_serializer_;
  FakeHelper* helper_ = nullptr;
  std::map<SubchannelKey, SubchannelState> subchannel_pool_;
};

}  // namespace testing
}  // namespace grpc_core

#endif  // GRPC_CORE_EXT_CLIENT_CHANNEL_LB_POLICY_LB_POLICY_TEST_LIB_H
