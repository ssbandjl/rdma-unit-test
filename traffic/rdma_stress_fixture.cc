// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "traffic/rdma_stress_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "infiniband/verbs.h"
#include "public/introspection.h"
#include "public/status_matchers.h"
#include "traffic/client.h"
#include "traffic/latency_measurement.h"
#include "traffic/op_types.h"
#include "traffic/operation_generator.h"
#include "traffic/qp_state.h"
#include "traffic/transport_validation.h"

namespace rdma_unit_test {

RdmaStressFixture::RdmaStressFixture() {
  validation_ = std::make_unique<TransportValidation>();
  latency_measure_ = std::make_unique<LatencyMeasurement>();
  // Open the verbs device available.
  auto context = ibv_.OpenDevice();
  CHECK_OK(context.status());  // Crash OK
  context_ = *context;
  port_gid_ = ibv_.GetLocalPortGid(context_);

  // Change the blocking mode of the async event queue.
  VLOG(2) << "Allow getting asynchronous events in nonblocking mode.";
  int flags = TEMP_FAILURE_RETRY(fcntl(context_->async_fd, F_GETFL));
  if (flags < 0) {
    LOG(ERROR) << "Failed reading async_fd file status flags on device "
               << context_->device->name
               << ". Calls to PollAndAckAsyncEvents will remain blocking.";
    return;
  }
  int ret = TEMP_FAILURE_RETRY(
      fcntl(context_->async_fd, F_SETFL, flags | O_NONBLOCK));
  LOG_IF(ERROR, ret < 0)
      << "Failed setting async events queue to nonblocking"
      << " mode on device " << context_->device->name
      << ". Calls to PollAndAckAsyncEvents will remain blocking.";
}

absl::Status RdmaStressFixture::SetUpRcClientsQPs(Client* local,
                                                  uint32_t local_qp_id,
                                                  Client* remote,
                                                  uint32_t remote_qp_id) {
  if (local_qp_id >= local->num_qps() || remote_qp_id >= remote->num_qps()) {
    return absl::InvalidArgumentError(
        "Please create qps before setting up the connection!");
  }
  QpState* local_qp = local->GetQpState(local_qp_id);
  QpState* remote_qp = remote->GetQpState(remote_qp_id);
  local_qp->set_remote_qp_state(remote_qp);
  remote_qp->set_remote_qp_state(local_qp);
  auto status = ibv_.SetUpRcQp(local->GetQpState(local_qp_id)->qp(),
                               remote->GetQpState(remote_qp_id)->qp());
  if (status.ok()) {
    LOG(INFO) << absl::StrCat("Connect local Client", local->client_id(),
                              ", QP (id): ", local_qp_id, ", to remote Client",
                              remote->client_id(), " QP (id): ", remote_qp_id);
  }
  return status;
}

void RdmaStressFixture::CreateSetUpRcQps(Client& initiator, Client& target,
                                         uint16_t qps_per_client) {
  DCHECK_EQ(initiator.num_qps(), target.num_qps());
  const auto qps_size = initiator.num_qps();
  for (uint32_t qp_id = qps_size; qp_id < qps_size + qps_per_client; ++qp_id) {
    CHECK_OK(initiator.CreateQps(1, /*is_rc=*/true));  // Crash OK
    CHECK_OK(target.CreateQps(1, /*is_rc=*/true));     // Crash OK
    // Set up Qpairs.
    EXPECT_OK(SetUpRcClientsQPs(&initiator, qp_id, &target, qp_id));
    EXPECT_OK(SetUpRcClientsQPs(&target, qp_id, &initiator, qp_id));
  }
  LOG(INFO) << "Successfully created " << initiator.num_qps() - qps_size
            << " new qps per client. Total qps: "
            << initiator.num_qps() + target.num_qps();
}

void RdmaStressFixture::HaltExecution(Client& initiator) {
  // Log the operations in flight, for debugging purposes.
  initiator.DumpPendingOps();

  // Keep polling async event for possible errors until no more events are
  // there.
  while (true) {
    auto async_event_status = PollAndAckAsyncEvents();
    if (async_event_status.ok()) break;
    LOG(ERROR) << async_event_status.message();
  }
}

absl::Status RdmaStressFixture::PollAndAckAsyncEvents() {
  // Poll on the async fd of the RDMA context, check if an event is available.
  pollfd poll_fd{};
  poll_fd.fd = context_->async_fd;
  poll_fd.events = POLLIN;
  int millisec_timeout = 0;
  int ret = TEMP_FAILURE_RETRY(poll(&poll_fd, 1, millisec_timeout));
  if (0 == ret) {
    return absl::OkStatus();
  }

  if (ret < 0) {
    return absl::InternalError(
        absl::StrCat("poll failed with errno ", errno, " on async event fd."));
  }

  // Read the ready event.
  ibv_async_event event{};
  ret = ibv_get_async_event(context_, &event);
  if (ret) {
    return absl::UnavailableError("Async event doesn't exist.");
  }

  auto status = absl::InternalError(absl::StrCat(
      "Verbs async event received event type: ", event.event_type));
  // Acknowledge the event, or else we can't destroy the verbs resources
  // involving the received async event.
  ibv_ack_async_event(&event);
  return status;
}

void RdmaStressFixture::ConfigureLatencyMeasurements(OpTypes op_type) {
  latency_measure_->ConfigureLatencyMeasurements(op_type);
}

void RdmaStressFixture::CollectClientLatencyStats(const Client& client) {
  latency_measure_->CollectClientLatencyStats(client);
}

void RdmaStressFixture::CheckLatencies() { latency_measure_->CheckLatencies(); }

void RdmaStressFixture::DumpState(Client& initiator) {
  for (uint32_t qp_id = 0; qp_id < initiator.num_qps(); ++qp_id) {
    VLOG(2) << initiator.GetQpState(qp_id);
  }
}

ibv_pd* RdmaStressFixture::NewPd() { return ibv_.AllocPd(context_); }

}  // namespace rdma_unit_test