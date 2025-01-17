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

#include "StfBuilderDevice.h"

#include <DataDistLogger.h>
#include <SubTimeFrameUtils.h>
#include <SubTimeFrameVisitors.h>
#include <ReadoutDataModel.h>
#include <SubTimeFrameDataModel.h>
#include <SubTimeFrameDPL.h>
#include <Utilities.h>

#include <options/FairMQProgOptions.h>
#include <Framework/SourceInfoHeader.h>

#include <chrono>
#include <thread>
#include <exception>
#include <boost/algorithm/string.hpp>

namespace o2::DataDistribution
{

using namespace std::chrono_literals;

constexpr int StfBuilderDevice::gStfOutputChanId;

StfBuilderDevice::StfBuilderDevice()
  : DataDistDevice()
{
}

static bool sResetDeviceCalled = false;

StfBuilderDevice::~StfBuilderDevice()
{
  DDDLOG("StfBuilderDevice::~StfBuilderDevice()");

  if (!sResetDeviceCalled) {
    EDDLOG("StfBuilderDevice::Reset() was not called. Performing cleanup");
    // clear all Stfs from the pipeline before the transport is deleted
    if (mI) {
      I().stopPipeline();
      I().clearPipeline();
      mI.reset();
    }


    mMemI.reset();
  }
}

void StfBuilderDevice::Init()
{
  DDDLOG("StfBuilderDevice::Init()");
  mI = std::make_unique<StfBuilderInstance>();
  mMemI = std::make_unique<MemoryResources>(this->AddTransport(fair::mq::Transport::SHM));

  I().mFileSource = std::make_unique<SubTimeFrameFileSource>(*mI, eStfFileSourceOut);
  I().mReadoutInterface = std::make_unique<StfInputInterface>(*this);
  I().mFileSink = std::make_unique<SubTimeFrameFileSink>(*this, *mI, eStfFileSinkIn, eStfFileSinkOut);
}

void StfBuilderDevice::Reset()
{
  DDDLOG("StfBuilderDevice::Reset()");
  // clear all Stfs from the pipeline before the transport is deleted
  I().stopPipeline();
  I().clearPipeline();

  mI.reset();
  mMemI.reset();

  sResetDeviceCalled = true;
}

void StfBuilderDevice::InitTask()
{
  DataDistLogger::SetThreadName("stfb-main");

  I().mInputChannelName = GetConfig()->GetValue<std::string>(OptionKeyInputChannelName);
  I().mOutputChannelName = GetConfig()->GetValue<std::string>(OptionKeyOutputChannelName);
  I().mDplChannelName = GetConfig()->GetValue<std::string>(OptionKeyDplChannelName);
  I().mStandalone = GetConfig()->GetValue<bool>(OptionKeyStandalone);
  I().mMaxStfsInPipeline = GetConfig()->GetValue<std::int64_t>(OptionKeyMaxBufferedStfs);
  I().mMaxBuiltStfs = GetConfig()->GetValue<std::uint64_t>(OptionKeyMaxBuiltStfs);

  // input data handling
  ReadoutDataUtils::sSpecifiedDataOrigin = getDataOriginFromOption(
    GetConfig()->GetValue<std::string>(OptionKeyStfDetector));

  ReadoutDataUtils::sRdhVersion =
    GetConfig()->GetValue<ReadoutDataUtils::RdhVersion>(OptionKeyRhdVer);

  ReadoutDataUtils::sRawDataSubspectype =
    GetConfig()->GetValue<ReadoutDataUtils::SubSpecMode>(OptionKeySubSpec);

  ReadoutDataUtils::sRdhSanityCheckMode =
    GetConfig()->GetValue<ReadoutDataUtils::SanityCheckMode>(OptionKeyRdhSanityCheck);

  ReadoutDataUtils::sEmptyTriggerHBFrameFilterring =
    GetConfig()->GetValue<bool>(OptionKeyFilterEmptyTriggerData);

  // Buffering limitation
  if (I().mMaxStfsInPipeline > 0) {
    if (I().mMaxStfsInPipeline < 4) {
      I().mMaxStfsInPipeline = 4;
      WDDLOG("Configuration: max buffered SubTimeFrames limit increased to {}", I().mMaxStfsInPipeline);
    }
    I().mPipelineLimit = true;
    WDDLOG("Configuration: Max buffered SubTimeFrames limit is set to {}."
      " Consider increasing it if data loss occurs.", I().mMaxStfsInPipeline);
  } else {
    I().mPipelineLimit = false;
    IDDLOG("Not imposing limits on number of buffered SubTimeFrames. Possibility of creating back-pressure.");
  }

  // Limited number of STF?
  IDDLOG("Configuration: Number of built SubTimeFrames is {}",
    I().mMaxBuiltStfs == 0 ? "not limited" : ("limited to " + std::to_string(I().mMaxBuiltStfs)));

  // File sink
  if (!I().mFileSink->loadVerifyConfig(*(this->GetConfig()))) {
    std::this_thread::sleep_for(1s); exit(-1);
  }

  // File source
  if (!I().mFileSource->loadVerifyConfig(*(this->GetConfig()))) {
    std::this_thread::sleep_for(1s); exit(-1);
  }

  I().mState.mRunning = true;

  // make sure we have detector if not using files
  if (!I().mFileSource->enabled()) {
    if ((ReadoutDataUtils::sRdhVersion < ReadoutDataUtils::RdhVersion::eRdhVer6) &&
      (ReadoutDataUtils::sSpecifiedDataOrigin == gDataOriginInvalid)) {
      EDDLOG("Detector string parameter must be specified when receiving the data from the "
        "readout and not using RDHv6 or greater.");
      std::this_thread::sleep_for(1s); exit(-1);
    } else {
      IDDLOG("READOUT INTERFACE: Configured detector: {}", ReadoutDataUtils::sSpecifiedDataOrigin.str);
    }

    if (ReadoutDataUtils::sRdhVersion == ReadoutDataUtils::RdhVersion::eRdhInvalid) {
      EDDLOG("The RDH version must be specified when receiving data from readout.");
      std::this_thread::sleep_for(1s); exit(-1);
    } else {
      IDDLOG("READOUT INTERFACE: Configured RDHv{}", ReadoutDataUtils::sRdhVersion);
      RDHReader::Initialize(unsigned(ReadoutDataUtils::sRdhVersion));
    }

    IDDLOG("READOUT INTERFACE: Configured O2 SubSpec mode: {}", to_string(ReadoutDataUtils::sRawDataSubspectype));

    if (ReadoutDataUtils::sRdhSanityCheckMode != ReadoutDataUtils::SanityCheckMode::eNoSanityCheck) {
      IDDLOG("Extensive RDH checks enabled. Data that does not meet the criteria will be {}.",
        (ReadoutDataUtils::sRdhSanityCheckMode == ReadoutDataUtils::eSanityCheckDrop ? "dropped" : "kept"));
    }

    if (ReadoutDataUtils::sEmptyTriggerHBFrameFilterring) {
      IDDLOG("Filtering of empty HBFrames in triggered mode enabled.");
    }
  }

  // Using DPL?
  if (I().mDplChannelName != "" && !I().mStandalone) {
    I().mDplEnabled = true;
    IDDLOG("DPL Channel name: {}",  I().mDplChannelName);
  } else {
    I().mDplEnabled = false;
    I().mDplChannelName = "";
    IDDLOG("Not sending data to DPL.");
  }

  // check if output enabled
  if (isStandalone() && !I().mFileSink->enabled()) {
    WDDLOG("Running in standalone mode and with STF file sink disabled. Data will be lost.");
  }

  // try to see if channels have been configured
  {
    if (!I().mFileSource->enabled()) {
      try {
        GetChannel(I().mInputChannelName);
      } catch(std::exception &) {
        EDDLOG("Input channel not configured (from o2-readout-exe) and not running with file source enabled.");
        std::this_thread::sleep_for(1s); exit(-1);
      }
    }

    try {
      if (!isStandalone()) {
        GetChannel(I().mDplEnabled ? I().mDplChannelName : I().mOutputChannelName);
      }
    } catch(std::exception &) {
      EDDLOG("Output channel (to DPL or StfSender) must be configured if not running in stand-alone mode.");
      std::this_thread::sleep_for(1s); exit(-1);
    }
  }

  // start output thread
  I().mOutputThread = create_thread_member("stfb_out", &StfBuilderDevice::StfOutputThread, this);
  // start file sink
  I().mFileSink->start();

  // start file source
  I().mFileSource->start(MemI(), I().mDplEnabled);

  // start a thread for readout process
  if (!I().mFileSource->enabled()) {
    I().mReadoutInterface->start();
  }

  // info thread
  I().mInfoThread = create_thread_member("stfb_info", &StfBuilderDevice::InfoThread, this);

  IDDLOG("InitTask() done... ");
}

void StfBuilderDevice::ResetTask()
{
  DDDLOG("StfBuilderDevice::ResetTask()");

  // Signal ConditionalRun() and other threads to stop
  I().mState.mRunning = false;

  // Stop the pipeline
  I().stopPipeline();
  I().clearPipeline();

  // NOTE: everything with threads goes below
  // signal and wait for the output thread
  if (I().mFileSource->enabled()) {
    I().mFileSource->stop();
  } else {
    I().mReadoutInterface->stop();
  }

  // stop the file sink
  if (I().mFileSink) {
    I().mFileSink->stop();
  }

  // signal and wait for the output thread
  if (I().mOutputThread.joinable()) {
    I().mOutputThread.join();
  }

  // wait for the info thread
  if (I().mInfoThread.joinable()) {
    I().mInfoThread.join();
  }

  // stop the memory resources very last
  MemI().stop();

  DDDLOG("StfBuilderDevice::ResetTask() done... ");
}

void StfBuilderDevice::StfOutputThread()
{
  std::unique_ptr<InterleavedHdrDataSerializer> lStfSerializer;
  std::unique_ptr<StfToDplAdapter> lStfDplAdapter;

  if (!isStandalone()) {
    // cannot get the channels in standalone mode
    auto& lOutputChan = getOutputChannel();
    IDDLOG("StfOutputThread: sending data to channel: {}", lOutputChan.GetName());

    if (!dplEnabled()) {
      lStfSerializer = std::make_unique<InterleavedHdrDataSerializer>(lOutputChan);
    } else {
      lStfDplAdapter = std::make_unique<StfToDplAdapter>(lOutputChan);
    }
  }

  while (I().mState.mRunning) {
    using hres_clock = std::chrono::high_resolution_clock;

    // Get a STF ready for sending
    std::unique_ptr<SubTimeFrame> lStf = I().dequeue(eStfSendIn);
    if (!lStf) {
      break;
    }

    // decrement the stf counter
    I().mCounters.mNumStfs--;

    DDDLOG_RL(2000, "Sending an STF out. stf_id={} stf_size={} unique_equipments={}",
      lStf->header().mId, lStf->getDataSize(), lStf->getEquipmentIdentifiers().size());

    // get data size sample
    I().mStfSizeMean += (lStf->getDataSize()/64 - I().mStfSizeMean/64);

    if (!isStandalone()) {
      const auto lSendStartTime = hres_clock::now();

      try {

        if (!dplEnabled()) {
          assert (lStfSerializer);
          lStfSerializer->serialize(std::move(lStf));
        } else {
          // Send to DPL bridge
          assert (lStfDplAdapter);
          lStfDplAdapter->sendToDpl(std::move(lStf));
        }
      } catch (std::exception& e) {
        if (IsReadyOrRunningState()) {
          EDDLOG("StfOutputThread: exception on send: what={}", e.what());
        } else {
          IDDLOG("StfOutputThread(NOT_RUNNING): shutting down: what={}", e.what());
        }
        break;
      }

      // record time spent in sending
      static auto sStartOfStfSending = hres_clock::now();
      if (I().mRestartRateCounter) {
        sStartOfStfSending = hres_clock::now();
        I().mSentOutStfs = 0;
        I().mRestartRateCounter = false;
      }

      I().mSentOutStfs++;
      I().mSentOutStfsTotal++;

      const auto lNow = hres_clock::now();
      const double lTimeMs = std::max(1e-6, std::chrono::duration<double, std::milli>(lNow - lSendStartTime).count());
      I().mSentOutRate = double(I().mSentOutStfs) / std::chrono::duration<double>(lNow - sStartOfStfSending).count();
      I().mStfDataTimeSamples += (lTimeMs / 100.0 - I().mStfDataTimeSamples / 100.0);
    }

    // check if we should exit:
    // 1. max number of stf set, or
    // 2. file reply used without loop parameter
    if ( (I().mMaxBuiltStfs > 0) && (I().mSentOutStfsTotal == I().mMaxBuiltStfs) ) {
      IDDLOG("Maximum number of sent SubTimeFrames reached. Exiting.");
      break;
    }
  }

  // leaving the output thread, send end of the stream info
  if (dplEnabled()) {
    o2::framework::SourceInfoHeader lDplExitHdr;
    lDplExitHdr.state = o2::framework::InputChannelState::Completed;
    auto lDoneStack = Stack(
      DataHeader(gDataDescriptionInfo, gDataOriginAny, 0, 0),
      o2::framework::DataProcessingHeader(),
      lDplExitHdr
    );

    // Send a multipart
    auto& lOutputChan = getOutputChannel();
    FairMQParts lCompletedMsg;
    auto lNoFree = [](void*, void*) { /* stack */ };
    lCompletedMsg.AddPart(lOutputChan.NewMessage(lDoneStack.data(), lDoneStack.size(), lNoFree));
    lCompletedMsg.AddPart(lOutputChan.NewMessage());
    lOutputChan.Send(lCompletedMsg);

    IDDLOG("Source Completed message sent to DPL.");
    // NOTE: no guarantees this will be sent out
    std::this_thread::sleep_for(2s);
  }

  I().mState.mRunning = false; // trigger stop via CondRun()

  IDDLOG("Output: Stopped SubTimeFrame sending. sent_total={} rate={:.4}", I().mSentOutStfsTotal, I().mSentOutRate);
  DDDLOG("Exiting StfOutputThread...");
}

void StfBuilderDevice::InfoThread()
{
  while (I().mState.mRunning) {

    std::this_thread::sleep_for(2s);

    if (I().mState.mPaused) {
      continue;
    }

    IDDLOG("SubTimeFrame size_mean={} frequency_mean={:.4} sending_time_ms_mean={:.4} queued_stf={}",
      I().mStfSizeMean, (1.0 / I().mReadoutInterface->StfTimeMean()),
      I().mStfDataTimeSamples, I().mCounters.mNumStfs);
    IDDLOG("SubTimeFrame sent_total={} rate={:.4}", I().mSentOutStfsTotal, I().mSentOutRate);
  }

  DDDLOG("Exiting Info thread...");
}

bool StfBuilderDevice::ConditionalRun()
{
  // nothing to do here sleep for awhile
  std::this_thread::sleep_for(500ms);

  if (!I().mState.mRunning) {
    DDDLOG("ConditionalRun() returning false.");
  }

  return I().mState.mRunning;
}

bpo::options_description StfBuilderDevice::getDetectorProgramOptions() {
  bpo::options_description lDetectorOptions("SubTimeFrameBuilder data source", 120);

  lDetectorOptions.add_options() (
    OptionKeyStfDetector,
    bpo::value<std::string>()->default_value(""),
    "Specifies the detector string for SubTimeFrame building. Allowed are: "
    "ACO, CPV, CTP, EMC, FT0, FV0, FDD, HMP, ITS, MCH, MFT, MID, PHS, TOF, TPC, TRD, ZDC."
  )(
    OptionKeyRhdVer,
    bpo::value<ReadoutDataUtils::RdhVersion>()->default_value(ReadoutDataUtils::RdhVersion::eRdhInvalid, ""),
    "Specifies the version of RDH. Supported versions of the RDH are: 3, 4, 5, 6."
  )(
    OptionKeySubSpec,
    bpo::value<ReadoutDataUtils::SubSpecMode>()->default_value(ReadoutDataUtils::SubSpecMode::eCruLinkId, "feeid"),
    "Specifies the which RDH fields are used for O2 Subspecification field: Allowed are:"
    "'cru_linkid' or 'feeid'."
  );

  return lDetectorOptions;
}

bpo::options_description StfBuilderDevice::getStfBuildingProgramOptions() {
  bpo::options_description lStfBuildingOptions("Options controlling SubTimeFrame building", 120);

  lStfBuildingOptions.add_options() (
    OptionKeyRdhSanityCheck,
    bpo::value<ReadoutDataUtils::SanityCheckMode>()->default_value(ReadoutDataUtils::SanityCheckMode::eNoSanityCheck, "off"),
    "Enable extensive RDH verification. Permitted values: off, print, drop (caution, any data not meeting criteria will be dropped)")(
    OptionKeyFilterEmptyTriggerData,
    bpo::bool_switch()->default_value(false),
    "Filter out empty HBFrames with RDHv4 sent in triggered mode.");

  return lStfBuildingOptions;
}

o2::header::DataOrigin StfBuilderDevice::getDataOriginFromOption(const std::string pArg)
{
  const auto lDetStr = boost::to_upper_copy<std::string>(pArg);

  if (lDetStr == "ACO") {
    return o2::header::gDataOriginACO;
  }
  else if (lDetStr == "CPV") {
    return o2::header::gDataOriginCPV;
  }
  else if (lDetStr == "CTP") {
    return o2::header::gDataOriginCTP;
  }
  else if (lDetStr == "EMC") {
    return o2::header::gDataOriginEMC;
  }
  else if (lDetStr == "FT0") {
    return o2::header::gDataOriginFT0;
  }
  else if (lDetStr == "FV0") {
    return o2::header::gDataOriginFV0;
  }
  else if (lDetStr == "FDD") {
    return o2::header::gDataOriginFDD;
  }
  else if (lDetStr == "HMP") {
    return o2::header::gDataOriginHMP;
  }
  else if (lDetStr == "ITS") {
    return o2::header::gDataOriginITS;
  }
  else if (lDetStr == "MCH") {
    return o2::header::gDataOriginMCH;
  }
  else if (lDetStr == "MFT") {
    return o2::header::gDataOriginMFT;
  }
  else if (lDetStr == "MID") {
    return o2::header::gDataOriginMID;
  }
  else if (lDetStr == "PHS") {
    return o2::header::gDataOriginPHS;
  }
  else if (lDetStr == "TOF") {
    return o2::header::gDataOriginTOF;
  }
  else if (lDetStr == "TPC") {
    return o2::header::gDataOriginTPC;
  }
  else if (lDetStr == "TRD") {
    return o2::header::gDataOriginTRD;
  }
  else if (lDetStr == "ZDC") {
    return o2::header::gDataOriginZDC;
  }
  else if (lDetStr == "TST") {
    return o2::header::gDataOriginTST;
  }

  return o2::header::gDataOriginInvalid;
}

} /* namespace o2::DataDistribution */
