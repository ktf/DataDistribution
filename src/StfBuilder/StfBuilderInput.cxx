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

#include "StfBuilderInput.h"
#include "StfBuilderDevice.h"

#include <SubTimeFrameBuilder.h>
#include <Utilities.h>

#include <DataDistLogger.h>

#include <FairMQDevice.h>

#include <vector>
#include <queue>
#include <chrono>
#include <sstream>

namespace o2::DataDistribution
{

void StfInputInterface::start()
{
  mRunning = true;

  mSeqStfQueue.start();
  mBuilderInputQueue = std::make_unique<ConcurrentFifo<std::vector<FairMQMessagePtr>>>();
  mStfBuilder = std::make_unique<SubTimeFrameReadoutBuilder>(mDevice.MemI(), mDevice.dplEnabled());

  mStfSeqThread = create_thread_member("stfb_seq", &StfInputInterface::StfSequencerThread, this);
  mBuilderThread = create_thread_member("stfb_builder", &StfInputInterface::StfBuilderThread, this);
  mInputThread = create_thread_member("stfb_input", &StfInputInterface::DataHandlerThread, this);
}

void StfInputInterface::stop()
{
mRunning = false;

  mStfBuilder->stop();


  if (mInputThread.joinable()) {
    mInputThread.join();
  }

  mBuilderInputQueue->stop();

  if (mBuilderThread.joinable()) {
      mBuilderThread.join();
  }

  mSeqStfQueue.stop();
  if (mStfSeqThread.joinable()) {
    mStfSeqThread.join();
  }

  // mStfBuilders.clear(); // TODO: deal with shm region cleanup
  mBuilderInputQueue.reset();
  mStfBuilder.reset();

  DDDLOG("INPUT INTERFACE: Stopped.");
}

/// Receiving thread
void StfInputInterface::DataHandlerThread()
{
  using namespace std::chrono_literals;
  constexpr std::uint32_t cInvalidStfId = ~0;
  std::vector<FairMQMessagePtr> lReadoutMsgs;
  lReadoutMsgs.reserve(4096);
  // current TF Id
  std::uint32_t lCurrentStfId = cInvalidStfId;

  // Reference to the input channel
  auto& lInputChan = mDevice.GetChannel(mDevice.getInputChannelName());

  try {
    while (mRunning) {

      // Equipment ID for the HBFrames (from the header)
      ReadoutSubTimeframeHeader lReadoutHdr;
      lReadoutMsgs.clear();

      // receive readout messages
      const std::int64_t lRet = lInputChan.Receive(lReadoutMsgs);

      // timeout ok
      if (lRet == static_cast<int64_t>(fair::mq::TransferCode::timeout)) {
        continue;
      }

      // interrupted
      if (lRet == static_cast<int64_t>(fair::mq::TransferCode::interrupted)) {
        if (mAcceptingData) {
          IDDLOG_RL(1000, "READOUT INTERFACE: Receive failed. FMQ state interrupted.");
        }
        std::this_thread::sleep_for(10ms);
        continue;
      }

      // not in running state
      if (lRet > 0 && !mAcceptingData) {
        WDDLOG_RL(1000, "READOUT INTERFACE: Discarding received data because we are not in the FMQ:RUNNING state.");
        continue;
      }

      // error
      if (lRet == static_cast<int64_t>(fair::mq::TransferCode::error)) {
        EDDLOG_RL(1000, "READOUT INTERFACE: Receive failed. fmq_error={} errno={} error={}",
          lRet, errno, std::string(strerror(errno)));
        std::this_thread::sleep_for(10ms);
        continue;
      }

      assert (lRet >= 0 && mAcceptingData);

      if (lReadoutMsgs.empty()) {
        // nothing received?
        continue;
      }

      // Copy to avoid surprises. The receiving header is not O2 compatible and can be discarded
      if (lReadoutMsgs[0]->GetSize() != sizeof(ReadoutSubTimeframeHeader)) {
        EDDLOG_RL(1000, "READOUT INTERFACE: incompatible readout header received. "
          "Make sure to use compatible o2-readout-exe version. received_size={} expected_size={}",
          lReadoutMsgs[0]->GetSize(), sizeof(ReadoutSubTimeframeHeader));
        continue;
      }
      std::memcpy(&lReadoutHdr, lReadoutMsgs[0]->GetData(), sizeof(ReadoutSubTimeframeHeader));

      // check the readout header version
      if (lReadoutHdr.mVersion != sReadoutInterfaceVersion) {
        EDDLOG_RL(1000, "READOUT INTERFACE: Unsupported readout interface version. "
          "Make sure to use compatible o2-readout-exe version. received={} expected={}",
          lReadoutHdr.mVersion, sReadoutInterfaceVersion);
        continue;
      }

      // check for backward/forward tf jumps
      if (lCurrentStfId != cInvalidStfId) {
        static thread_local std::uint64_t sNumNonContIncStfs = 0;
        static thread_local std::uint64_t sNumNonContDecStfs = 0;

        // backward jump
        if (lReadoutHdr.mTimeFrameId < lCurrentStfId) {
          sNumNonContIncStfs++;
          std::stringstream lErrMsg;
          lErrMsg << "READOUT INTERFACE: "
              "TF ID decreased! (" << lCurrentStfId << ") -> (" << lReadoutHdr.mTimeFrameId << ") "
              "o2-readout-exe sent messages with non-monotonic TF id! SubTimeFrames will be incomplete! "
              "Total occurrences: " << sNumNonContIncStfs;

          EDDLOG_RL(200, lErrMsg.str());
          DDDLOG(lErrMsg.str());

          // TODO: accout for lost data
          continue;
        }

        // forward jump
        if (lReadoutHdr.mTimeFrameId > (lCurrentStfId + 1)) {
          sNumNonContDecStfs++;
          WDDLOG_RL(200, "READOUT INTERFACE: TF ID non-contiguous increase! ({}) -> ({}). Total occurrences: {}",
            lCurrentStfId, lReadoutHdr.mTimeFrameId, sNumNonContDecStfs);
        }
        // we keep the data since this might be a legitimate jump
      }

      // get the current TF id
      lCurrentStfId = lReadoutHdr.mTimeFrameId;

      mBuilderInputQueue->push(std::move(lReadoutMsgs));
    }
  } catch (std::runtime_error& e) {
    if (mRunning) {
      EDDLOG_RL(1000, "Receive failed on the Input channel. Stopping the input thread. what={}", e.what());
    }
  }

  DDDLOG("Exiting the input thread.");
}

/// StfBuilding thread
void StfInputInterface::StfBuilderThread()
{
  using namespace std::chrono_literals;

  static constexpr bool cBuildOnTimeout = false;
  // current TF Id
  constexpr std::uint32_t cInvalidStfId = ~0;
  std::uint32_t lCurrentStfId = cInvalidStfId;
  bool lStarted = false;
  std::vector<FairMQMessagePtr> lReadoutMsgs;
  lReadoutMsgs.reserve(1U << 20);

  // support FEEID masking
  std::uint32_t lFeeIdMask = ~std::uint32_t(0); // subspec size
  const auto lFeeMask = std::getenv("DATADIST_FEE_MASK");
  if (lFeeMask) {
    try {
      lFeeIdMask = std::stoul(lFeeMask, nullptr, 16);
    } catch(...) {
      EDDLOG("Cannot convert {} for the FeeID mask.", lFeeMask);
    }
  }
  IDDLOG("StfBuilder: Using {:#06x} as the FeeID mask.", lFeeIdMask);

  // Reference to the input channel
  assert (mBuilderInputQueue);
  assert (mStfBuilder);
  // Input queue
  auto &lInputQueue = *mBuilderInputQueue;
  // Stf builder
  SubTimeFrameReadoutBuilder &lStfBuilder = *mStfBuilder;

  // insert and mask the feeid
  auto lInsertWitFeeIdMasking = [&lStfBuilder, lFeeIdMask] (const header::DataOrigin &pDataOrigin,
    const header::DataHeader::SubSpecificationType &pSubSpec, const ReadoutSubTimeframeHeader &pRdoHeader,
    const FairMQParts::iterator pStartHbf, const std::size_t pInsertCnt) {

    // mask the subspecification if the fee mode is used
    auto lMaskedSubspec = pSubSpec;
    if (ReadoutDataUtils::SubSpecMode::eFeeId == ReadoutDataUtils::sRawDataSubspectype) {
      lMaskedSubspec &= lFeeIdMask;
    }

    lStfBuilder.addHbFrames(pDataOrigin, lMaskedSubspec, pRdoHeader, pStartHbf, pInsertCnt);

    return pInsertCnt;
  };

  const auto cStfDataWaitFor = 2s;

  using hres_clock = std::chrono::high_resolution_clock;

  std::chrono::time_point<hres_clock, std::chrono::duration<double>> lStartSec = hres_clock::now();

  while (mRunning) {

    // Lambda for completing the Stf
    auto finishBuildingCurrentStf = [&](bool pTimeout = false) {
      // Finished: queue the current STF and start a new one
      ReadoutDataUtils::sFirstSeenHBOrbitCnt = 0;

      if (auto lStf = lStfBuilder.getStf(); lStf.has_value()) {
        // start the new STF
        if (pTimeout) {
          WDDLOG("READOUT INTERFACE: finishing STF on a timeout. stf_id={} size={}",
            (*lStf)->header().mId, (*lStf)->getDataSize());
        }

        mSeqStfQueue.push(std::move(*lStf));
        { // MON: data of a new STF received, get the freq and new start time
          auto lNow = hres_clock::now();
          std::chrono::duration<double> lTimeDiff = lNow - lStartSec;
          lStartSec = lNow;
          mStfTimeMean += (lTimeDiff.count()/100.0 - mStfTimeMean/100.0);
        }
      } else {
        mStfTimeMean *= 2.0;
      }
    };

    // Equipment ID for the HBFrames (from the header)
    lReadoutMsgs.clear();

    // receive readout messages
    const auto lRet = lInputQueue.pop_wait_for(lReadoutMsgs, cStfDataWaitFor);
    if (!lRet && mRunning) {
      if (lStarted) {
        // finish on a timeout
        finishBuildingCurrentStf(cBuildOnTimeout);
      }
      continue;
    } else if (!lRet && !mRunning) {
      break;
    } else if (lRet && !mRunning) {
      static thread_local std::uint64_t sAfterStopStfs = 0;
      sAfterStopStfs++;
      WDDLOG_RL(1000, "StfBuilderThread: Building STFs after stop signal. after_stop_stf_count={}", sAfterStopStfs);
    }

    // must not be empty
    if (lReadoutMsgs.empty()) {
      EDDLOG_RL(1000, "READOUT INTERFACE: empty readout multipart.");
      continue;
    }

    // stated to build STFs
    lStarted = true;

    // Copy to avoid surprises. The receiving header is not O2 compatible and can be discarded
    ReadoutSubTimeframeHeader lReadoutHdr;
    // NOTE: the size is checked on receive
    std::memcpy(&lReadoutHdr, lReadoutMsgs[0]->GetData(), sizeof(ReadoutSubTimeframeHeader));

    // log only
    DDDLOG_RL(5000, "READOUT INTERFACE: Received an ReadoutMsg. stf_id={}", lReadoutHdr.mTimeFrameId);

    // check multipart size
    if (lReadoutMsgs.size() == 1 && !lReadoutHdr.mFlags.mLastTFMessage) {
      EDDLOG_RL(1000, "READOUT INTERFACE: Received only a header message without the STF stop bit set.");
      continue;
    }

    // check the link/feeids (first HBF only)
    if (lReadoutMsgs.size() > 1 && lReadoutHdr.mFlags.mIsRdhFormat) {
      try {
        const auto R = RDHReader(lReadoutMsgs[1]);
        const auto lLinkId = R.getLinkID();

        if (lLinkId != lReadoutHdr.mLinkId) {
          EDDLOG_RL(1000, "READOUT INTERFACE: Update link ID does not match RDH in the data block."
            " hdr_link_id={} rdh_link_id={}", lReadoutHdr.mLinkId, lLinkId);
        }
      } catch (RDHReaderException &e) {
        EDDLOG_RL(1000, "READOUT INTERFACE: error while parsing the RDH header. what={}", e.what());
        // TODO: the whole ReadoutMsg is discarded. Account and report the data size.
        continue;
      }
    }

    const auto lIdInBuilding = lStfBuilder.getCurrentStfId();
    lCurrentStfId = lIdInBuilding ? *lIdInBuilding : lReadoutHdr.mTimeFrameId;

    // check for the new TF marker
    if (lReadoutHdr.mTimeFrameId != lCurrentStfId) {
      // we expect to be notified about new TFs
      if (lIdInBuilding) {
        EDDLOG_RL(1000, "READOUT INTERFACE: Update with a new STF ID but the Stop flag was not set for the current STF."
          " current_id={} new_id={}", lCurrentStfId, lReadoutHdr.mTimeFrameId);
        finishBuildingCurrentStf();
      }
      lCurrentStfId = lReadoutHdr.mTimeFrameId;
    }

    const bool lFinishStf = lReadoutHdr.mFlags.mLastTFMessage;
    if (lReadoutMsgs.size() > 1) {
      // check subspecifications of all messages
      header::DataHeader::SubSpecificationType lSubSpecification = ~header::DataHeader::SubSpecificationType(0);
      header::DataOrigin lDataOrigin;
      try {
        const auto R1 = RDHReader(lReadoutMsgs[1]);
        lDataOrigin = ReadoutDataUtils::getDataOrigin(R1);
        lSubSpecification = ReadoutDataUtils::getSubSpecification(R1);
      } catch (RDHReaderException &e) {
        EDDLOG_RL(1000, "READOUT_INTERFACE: Cannot parse RDH of received HBFs. what={}", e.what());
        // TODO: the whole ReadoutMsg is discarded. Account and report the data size.
        continue;
      }

      assert (lReadoutMsgs.size() > 1);
      auto lStartHbf = lReadoutMsgs.begin() + 1; // skip the meta message
      auto lEndHbf = lStartHbf + 1;

      std::size_t lAdded = 0;
      bool lErrorWhileAdding = false;

      while (true) {
        if (lEndHbf == lReadoutMsgs.end()) {
          //insert the remaining span
          std::size_t lInsertCnt = (lEndHbf - lStartHbf);
          lAdded += lInsertWitFeeIdMasking(lDataOrigin, lSubSpecification, lReadoutHdr, lStartHbf, lInsertCnt);
          break;
        }

        header::DataHeader::SubSpecificationType lNewSubSpec = ~header::DataHeader::SubSpecificationType(0);
        try {
          const auto Rend = RDHReader(*lEndHbf);
          lNewSubSpec = ReadoutDataUtils::getSubSpecification(Rend);
        } catch (RDHReaderException &e) {
          EDDLOG_RL(1000, e.what());
          // TODO: portion of the ReadoutMsg is discarded. Account and report the data size.
          lErrorWhileAdding = true;
          break;
        }

        if (lNewSubSpec != lSubSpecification) {
          WDDLOG_RL(10000, "READOUT INTERFACE: Update with mismatched subspecifications."
            " block[0]_subspec={:#06x}, block[{}]_subspec={:#06x}",
            lSubSpecification, (lEndHbf - (lReadoutMsgs.begin() + 1)), lNewSubSpec);
          // insert
          lAdded += lInsertWitFeeIdMasking(lDataOrigin, lSubSpecification, lReadoutHdr, lStartHbf, lEndHbf - lStartHbf);
          lStartHbf = lEndHbf;

          lSubSpecification = lNewSubSpec;
        }
        lEndHbf = lEndHbf + 1;
      }

      if (!lErrorWhileAdding && (lAdded != lReadoutMsgs.size() - 1) ) {
        EDDLOG_RL(500, "BUG: Not all received HBFrames added to the STF.");
      }
    }

    // check if this was the last message of an STF
    if (lFinishStf) {
      finishBuildingCurrentStf();
    }
  }

  DDDLOG("Exiting StfBuilder thread.");
}

void StfInputInterface::StfSequencerThread()
{
  using namespace std::chrono_literals;

  static constexpr std::uint64_t sMaxMissingStfsForSeq = 2ull * 11234 / 256; // 2 seconds of STFs

  while (mRunning) {
    auto lStf = mSeqStfQueue.pop_wait_for(500ms);

    if (lStf == std::nullopt || !mAcceptingData) {
      continue;
    }

    // have data, check the sequence
    if (lStf) {
      const auto lCurrId = (*lStf)->id();
      (*lStf)->setOrigin(SubTimeFrame::Header::Origin::eReadout);

      if (lCurrId <= mLastSeqStfId) {
        EDDLOG_RL(500, "READOUT_INTERFACE: Repeated STF will be rejected. previous_stf_id={} current_stf_id={}",
          mLastSeqStfId, lCurrId);
        // reject this STF.
        continue;
      }

      // expected next stf
      if ((mLastSeqStfId + 1) == lCurrId) {
        mLastSeqStfId = lCurrId;
        mDevice.I().queue(eStfBuilderOut, std::move(*lStf));
        continue;
      }

      // there are missing STFs
      const auto lMissingIdStart = mLastSeqStfId + 1;
      const auto lMissingCnt = lCurrId - lMissingIdStart;

      if (lMissingCnt < sMaxMissingStfsForSeq) {
        WDDLOG_RL(1000, "READOUT_INTERFACE: Creating empty (missing) STFs. previous_stf_id={} num_missing={}",
          mLastSeqStfId, lMissingCnt);
        // create the missing ones and continue
        for (std::uint64_t lStfIdIdx = lMissingIdStart; lStfIdIdx < lCurrId; lStfIdIdx++) {
          auto lEmptyStf = std::make_unique<SubTimeFrame>(lStfIdIdx);
          (*lStf)->setOrigin(SubTimeFrame::Header::Origin::eNull);
          mDevice.I().queue(eStfBuilderOut, std::move(lEmptyStf));
        }
      } else {
        WDDLOG_RL(1000, "READOUT_INTERFACE: Large STF gap. previous_stf_id={} current_stf_id={} num_missing={}",
          mLastSeqStfId, lCurrId, lMissingCnt);
      }

      // insert the actual stf
      mLastSeqStfId = lCurrId;
      mDevice.I().queue(eStfBuilderOut, std::move(*lStf));
      continue;
    }

    // TODO: handle timeouts
  }

  DDDLOG("Exiting StfSequencerThread thread.");
}

} /* namespace o2::DataDistribution */
