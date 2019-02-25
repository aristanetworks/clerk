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

#ifndef CLERK_GENERATOR_H
#define CLERK_GENERATOR_H

#include <thread>

#include "testimony.h"

namespace clerk {

// FlowGenerator generates flows and adds them to Flow table.
class FlowGenerator : public Processor {
   public:
      FlowGenerator(uint64_t fu, uint64_t ft, int32_t nh, int32_t nf,
            const StateFactory* sf);
      ~FlowGenerator();

      void StartThreads();
      void Gather(std::vector<std::unique_ptr<State>>* states, bool last);

   private:
      // Flow Information.
      struct FlowInfo {
         FlowInfo(uint64_t ts) { lastUpdateSecs = ts; }
         // Timestamp (in seconds) of the last time flow was updated.
         uint64_t lastUpdateSecs;
      };

      void Run();
      void InitFlowKeys();
      flow::Key GetFlowKeyToUpdate();
      flow::Key CreateFlowKey();
      void UpdateFlowTable(flow::Key& key, flow::Stats& stats);

      uint64_t flowUpdateSeconds_;
      uint64_t flowTimeoutSeconds_;
      int32_t numHosts_;
      int32_t numFlowsPerHost_;
      const StateFactory* stateFactory_;

      uint32_t numPersistentFlows_; // Number of persistent flows
      uint64_t flowUpdatePeriodNs_; // Period of flow updates in nanoseconds
      std::unique_ptr<State> state_;
      std::unique_ptr<std::thread> thread_;
      std::mutex lock_;
      std::vector<flow::Key> allFlowKeys_;
      std::unordered_map<flow::Key,std::unique_ptr<FlowInfo>> persistentFlowInfo_;
};

}  // namespace clerk

#endif // CLERK_GENERATOR_H
