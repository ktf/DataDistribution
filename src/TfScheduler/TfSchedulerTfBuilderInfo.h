// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef ALICEO2_TF_SCHEDULER_TFBUILDER_INFO_H_
#define ALICEO2_TF_SCHEDULER_TFBUILDER_INFO_H_

#include <ConfigParameters.h>
#include <ConfigConsul.h>

#include <StfSenderRpcClient.h>

#include <discovery.pb.h>
#include <discovery.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <Utilities.h>

#include <vector>
#include <map>
#include <deque>
#include <thread>
#include <chrono>

namespace o2
{
namespace DataDistribution
{

using namespace std::chrono_literals;

struct TfBuilderInfo {
  std::chrono::system_clock::time_point mUpdateLocalTime;
  TfBuilderUpdateMessage mTfBuilderUpdate;
  std::uint64_t mLastScheduledTf = 0;
  std::uint64_t mEstimatedFreeMemory;

  TfBuilderInfo() = delete;

  TfBuilderInfo(std::chrono::system_clock::time_point pUpdateLocalTime, const TfBuilderUpdateMessage &pTfBuilderUpdate)
  : mUpdateLocalTime(pUpdateLocalTime),
    mTfBuilderUpdate(pTfBuilderUpdate)
  {
    mEstimatedFreeMemory = mTfBuilderUpdate.free_memory();
  }

  const std::string& id() const { return mTfBuilderUpdate.info().process_id(); }
  std::uint64_t last_scheduled_tf_id() const { return mLastScheduledTf; }
  std::uint64_t last_built_tf_id() const { return mTfBuilderUpdate.last_built_tf_id(); }
};

class TfSchedulerTfBuilderInfo
{
 public:
  TfSchedulerTfBuilderInfo() = delete;
  TfSchedulerTfBuilderInfo(std::shared_ptr<ConsulTfSchedulerInstance> pDiscoveryConfig)
  : mDiscoveryConfig(pDiscoveryConfig)
  {
    mGlobalInfo.reserve(1000); // number of EPNs
  }

  ~TfSchedulerTfBuilderInfo() { }

  void start() {
    mGlobalInfo.clear();

    mRunning = true;
    // start gRPC client monitoring thread
    mHousekeepingThread = create_thread_member("sched_tfb_mon", &TfSchedulerTfBuilderInfo::HousekeepingThread, this);
  }

  void stop() {
    mRunning = false;

    if (mHousekeepingThread.joinable()) {
      mHousekeepingThread.join();
    }

    {
      std::scoped_lock lLock(mGlobalInfoLock);
      // delete all info
      mGlobalInfo.clear();
    }
    {
      std::scoped_lock lLock(mReadyInfoLock);
      mReadyTfBuilders.clear();
    }
  }

  void HousekeepingThread();

  void updateTfBuilderInfo(const TfBuilderUpdateMessage &pTfBuilderUpdate);

  void addReadyTfBuilder(std::shared_ptr<TfBuilderInfo> pInfo)
  {
    std::scoped_lock lLock(mReadyInfoLock);
    mReadyTfBuilders.push_back(std::move(pInfo));
  }

  void removeReadyTfBuilder(const std::string &pId)
  {
    std::scoped_lock lLock(mReadyInfoLock);
    for (auto it = mReadyTfBuilders.begin(); it != mReadyTfBuilders.end(); it++) {
      if ((*it)->id() == pId) {
        DDDLOG("Removed TfBuilder from the ready list. tfb_id={}", pId);
        mReadyTfBuilders.erase(it);
        break;
      }
    }
  }

  bool findTfBuilderForTf(const std::uint64_t pSize, std::string& pTfBuilderId /*out*/);

  bool markTfBuilderWithTfId(const std::string& pTfBuilderId, const std::uint64_t pTfIf)
  {
    std::scoped_lock lLock(mGlobalInfoLock, mReadyInfoLock);
    if (mGlobalInfo.count(pTfBuilderId) > 0) {
      mGlobalInfo[pTfBuilderId]->mLastScheduledTf = pTfIf;
      return true;
    }
    return false;
  }

private:
  /// Overestimation of actual size for TF building
  static constexpr std::uint64_t sTfSizeOverestimatePercent = std::uint64_t(10);

  /// Discard timeout for non-complete TFs
  static constexpr auto sTfBuilderDiscardTimeout = 5s;

  /// Discovery configuration
  std::shared_ptr<ConsulTfSchedulerInstance> mDiscoveryConfig;

  /// Housekeeping thread
  std::atomic_bool mRunning = false;
  std::thread mHousekeepingThread;

  /// TfSender global info
  mutable std::recursive_mutex mGlobalInfoLock;
    std::unordered_map<std::string, std::shared_ptr<TfBuilderInfo>> mGlobalInfo;

  /// List of TfBuilders with available resources
  mutable std::recursive_mutex mReadyInfoLock;
    std::deque<std::shared_ptr<TfBuilderInfo>> mReadyTfBuilders;
};

}
} /* namespace o2::DataDistribution */

#endif /* ALICEO2_TF_SCHEDULER_TFBUILDER_INFO_H_ */
