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

#ifndef ALICEO2_TF_BUILDER_RPC_H_
#define ALICEO2_TF_BUILDER_RPC_H_

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <grpcpp/grpcpp.h>
#include <discovery.grpc.pb.h>
#pragma GCC diagnostic pop

#include <ConfigConsul.h>
#include <TfSchedulerRpcClient.h>
#include <StfSenderRpcClient.h>

#include <SubTimeFrameDataModel.h>

#include <ConcurrentQueue.h>

#include <vector>
#include <map>
#include <thread>
#include <mutex>

namespace o2::DataDistribution
{

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

class SyncMemoryResources;

class TfBuilderRpcImpl final : public TfBuilderRpc::Service
{
public:
  TfBuilderRpcImpl(std::shared_ptr<ConsulTfBuilder> pDiscoveryConfig, SyncMemoryResources &pMemI)
  : mMemI(pMemI),
    mDiscoveryConfig(pDiscoveryConfig),
    mStfSenderRpcClients(mDiscoveryConfig)
  { }

  virtual ~TfBuilderRpcImpl() { }

  TfSchedulerRpcClient& TfSchedRpcCli() { return mTfSchedulerRpcClient; }

  void initDiscovery(const std::string pRpcSrvBindIp, int &lRealPort /*[out]*/);
  bool start(const std::uint64_t pBufferSize);
  void stop();

  void startAcceptingTfs();
  void stopAcceptingTfs();

  void UpdateSendingThread();
  void StfRequestThread();

  bool recordTfBuilt(const SubTimeFrame &pTf);
  bool recordTfForwarded(const std::uint64_t &pTfId);
  bool sendTfBuilderUpdate();

  bool getNewTfBuildingRequest(TfBuildingInformation &pNewTfRequest)
  { if (!mTfBuildRequests) {
      return false;
    }
    return mTfBuildRequests->pop(pNewTfRequest);
  }

  StfSenderRpcClientCollection<ConsulTfBuilder>& StfSenderRpcClients() { return mStfSenderRpcClients; }

  bool isTerminateRequested() const { return mTerminateRequested; }

  // rpc BuildTfRequest(TfBuildingInformation) returns (BuildTfResponse) { }
  ::grpc::Status BuildTfRequest(::grpc::ServerContext* context, const TfBuildingInformation* request, BuildTfResponse* response) override;

  ::grpc::Status TerminatePartition(::grpc::ServerContext* context, const ::o2::DataDistribution::PartitionInfo* request, ::o2::DataDistribution::PartitionResponse* response) override;

private:
  std::atomic_bool mRunning = false;
  std::atomic_bool mTerminateRequested = false;

  std::atomic_bool mAcceptingTfs = false;
  std::mutex mUpdateLock;

  /// Update sending thread
  std::condition_variable mUpdateCondition;
  std::thread mUpdateThread;

  // Stf request thread
  // std::condition_variable mNewRequestCondition;
  std::thread mStfRequestThread;

  /// TfBuilder Memory Resource
  SyncMemoryResources &mMemI;

  /// Discovery configuration
  std::shared_ptr<ConsulTfBuilder> mDiscoveryConfig;

  /// TfBuilder RPC server
  std::unique_ptr<Server> mServer;

  /// Scheduler RPC client
  StfSenderRpcClientCollection<ConsulTfBuilder> mStfSenderRpcClients;

  /// StfSender RPC clients
  TfSchedulerRpcClient mTfSchedulerRpcClient;

  /// TF buffer size accounting
  std::recursive_mutex mTfIdSizesLock;
  std::unordered_map <uint64_t, uint64_t> mTfIdSizes;
  // Update information for the TfScheduler
  std::uint64_t mBufferSize = 0;
  std::uint64_t mCurrentTfBufferSize = 0;
  std::uint64_t mLastBuiltTfId = 0;
  std::uint32_t mNumBufferedTfs = 0;

  /// Queue of TF building requests
  std::unique_ptr<ConcurrentFifo<TfBuildingInformation>> mTfBuildRequests;
};

} /* namespace o2::DataDistribution */

#endif /* ALICEO2_TF_BUILDER_RPC_H_ */
