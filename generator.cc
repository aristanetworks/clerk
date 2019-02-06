// Copyright (c) 2019 Arista Networks, Inc.  All rights reserved.
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

#include <glog/logging.h>

#include "flow.h"
#include "generator.h"
#include "ipfix.h"

namespace clerk {

FlowGenerator::FlowGenerator(uint64_t fu, uint64_t ft, int32_t nh, int32_t nf,
      const StateFactory* sf) {
   flowUpdateSeconds_ = fu;
   flowTimeoutSeconds_ = ft;
   numHosts_ = nh;
   numFlowsPerHost_ = nf;
   stateFactory_ = sf;
   uint64_t totalFlows = numHosts_ * numFlowsPerHost_;
   // 1% of total flows are persistent flows, in the range [1-100].
   numPersistentFlows_ = min(max(totalFlows / 100, 1UL), 100UL);
   // Set update period so that all flows are updated once (on average)
   // within Flow Update period. There will still be some flows that are removed
   // from Flow table due to Idle Timeout.
   flowUpdatePeriodNs_ = flowUpdateSeconds_ * kNumNanosPerSecond / totalFlows;
   VLOG(2) << "Flow update period: " << flowUpdatePeriodNs_;
   state_ = stateFactory_->New(nullptr);

   // Initialize the Flow keys.
   InitFlowKeys();

   LOG(INFO) << "Running in Flow Generator mode with following parameters";
   LOG(INFO) << "Total flows: " << totalFlows << ", Persistent flows: " <<
      numPersistentFlows_;
}

// Generate an IPv4 address.
static uint32_t GenerateIp4Address(int32_t hostIndex) {
   // IPv4 addresses are generated in the range [10.0.0.1 - 10.255.255.254].
   // (maximum of 2**24 - 2 ~= 16M hosts).
   return ((10 << 24) | (hostIndex + 1));
}

// Generate an IPv6 address.
static void GenerateIp6Address(int32_t hostIndex, char *ip6) {
   // IPv6 addresses are generated in the range [::0A00:0001 - ::0AFF:FFFE].
   // (maximum of 2**24 - 2 ~= 16M hosts).
   for (int i = 0; i < 12; i++) {
      ip6[i] = 0;
   }
   uint32_t ip4 = GenerateIp4Address(hostIndex);
   ip6[12] = ip4 >> 24;
   ip6[13] = ip4 >> 16;
   ip6[14] = ip4 >> 8;
   ip6[15] = ip4;
}

// Generate a TCP/UDP transport port number.
static uint16_t GenerateTransportPort() {
   return rand() % 0xFFFF;
}

// Initialize the Flow keys.
void FlowGenerator::InitFlowKeys() {
   // Create Flow keys for each host.
   for (int hostIndex = 0; hostIndex < numHosts_; hostIndex++) {
      uint32_t srcIp4 = 0;
      char srcIp6[16];
      bool isV4 = rand() % 2 ? true : false;

      // Pick a source IP address.
      if (isV4) {
         srcIp4 = GenerateIp4Address(hostIndex);
      } else {
         GenerateIp6Address(hostIndex, srcIp6);
      }

      for (int j = 0; j < numFlowsPerHost_; j++) {
         uint32_t dstIp4 = 0;
         char dstIp6[16];
         flow::Key key;

         // Pick a destination IP address that is not the same as source IP address.
         int32_t dstHostIndex = 0;
         do {
            dstHostIndex = rand() % numHosts_;
         } while (dstHostIndex == hostIndex);
         if (isV4) {
            dstIp4 = GenerateIp4Address(dstHostIndex);
         } else {
            GenerateIp6Address(dstHostIndex, dstIp6);
         }

         // Build Flow key.
         if (isV4) {
            key.set_src_ip4(srcIp4);
            key.set_dst_ip4(dstIp4);
         } else {
            key.set_src_ip6(srcIp6);
            key.set_dst_ip6(dstIp6);
         }
         key.src_port = GenerateTransportPort();
         key.dst_port = GenerateTransportPort();
         key.protocol = rand() % 2 ? 6 : 17; // TCP or UDP protocol.

         // Save Flow key.
         allFlowKeys_.push_back(key);
         if (persistentFlowInfo_.size() < numPersistentFlows_) {
            // Save flow as Persistent Flow.
            persistentFlowInfo_.emplace(key, new FlowInfo(0));
         }
      }
   }
}

// Get a Flow to update.
flow::Key FlowGenerator::GetFlowKeyToUpdate() {
   uint64_t currentTime = GetCurrentTimeSeconds();

   // Check if any Persistent Flow needs to be updated.
   for (auto iter = persistentFlowInfo_.begin(); iter != persistentFlowInfo_.end();
         ++iter) {
      flow::Key key = iter->first;
      if (currentTime - iter->second->lastUpdateSecs >= flowTimeoutSeconds_ / 2) {
         // Update this flow before it times out.
         return key;
      }
   }

   // Select a random Flow to update.
   return allFlowKeys_[rand() % allFlowKeys_.size()];
}

// Update Flow table.
void FlowGenerator::UpdateFlowTable(flow::Key& key, flow::Stats& stats) {
   // Update IPFIX Flow table.
   std::unique_lock<std::mutex> ml(lock_);
   clerk::IPFIX *ipfixState = reinterpret_cast<clerk::IPFIX*>(state_.get());
   flow::AddToTable(&ipfixState->flows_, key, stats);
   ml.unlock();

   // If this flow is a Persistent Flow, update Persistent Flow information.
   auto iter = persistentFlowInfo_.find(key);
   if (iter != persistentFlowInfo_.end()) {
      // Save Flow update time for Persistent Flow.
      iter->second->lastUpdateSecs = (stats.last_ns / kNumNanosPerSecond);
   }
}

void FlowGenerator::Run() {
   uint64_t lastUpdateTime = 0;

   LOG(INFO) << "Running Flow Generator";

   while (true) {
      lastUpdateTime = GetCurrentTimeNanos();
      flow::Key key = GetFlowKeyToUpdate();
      int numPackets = (rand() % 100) + 1;        // Range of [1 - 100]
      int packetSize = max((rand() % 1501), 64);  // Range of [64 - 1500]
      flow::Stats stats(numPackets * packetSize, numPackets, lastUpdateTime);

      // Update Flow Table.
      UpdateFlowTable(key, stats);

      // Sleep for some time.
      SleepForNanoseconds(lastUpdateTime + flowUpdatePeriodNs_ -
            GetCurrentTimeNanos());
   };
}

void FlowGenerator::StartThreads() {
   thread_.reset(new std::thread([this]() { Run(); }));
}

// Gather and return the current IPFIX state.
void FlowGenerator::Gather(std::vector<std::unique_ptr<State>>* states, bool last) {
   states->clear();
   states->resize(1);
   std::unique_lock<std::mutex> ml(lock_);
   std::unique_ptr<State> stateCopy = stateFactory_->New(state_.get());
   state_.swap(stateCopy);
   (*states)[0] = std::move(stateCopy);
   clerk::IPFIX *ipfixState = reinterpret_cast<clerk::IPFIX*>((*states)[0].get());
   LOG(INFO) << "Current Flow table size: " << ipfixState->flows_.size();
}

FlowGenerator::~FlowGenerator() {
   thread_->join();
}

}  // namespace clerk
