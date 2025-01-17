// Concord
//
// Copyright (c) 2020-2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License"). You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the sub-component's license, as noted in the LICENSE
// file.

#pragma once

#include "messages/ClientPreProcessRequestMsg.hpp"
#include "messages/PreProcessRequestMsg.hpp"
#include "messages/PreProcessReplyMsg.hpp"
#include "messages/PreProcessResultMsg.hpp"
#include "PreProcessorRecorder.hpp"
#include <vector>
#include <map>
#include <list>

namespace preprocessor {

// This class collects and stores data relevant to the processing of one specific client request by all replicas.

typedef enum { NONE, CONTINUE, COMPLETE, CANCEL, EXPIRED, CANCELLED_BY_PRIMARY, RETRY_PRIMARY } PreProcessingResult;
using ReplicaIdsList = std::vector<ReplicaId>;

class RequestProcessingState {
 public:
  RequestProcessingState(ReplicaId myReplicaId,
                         uint16_t numOfReplicas,
                         const std::string& batchCid,
                         uint16_t clientId,
                         uint16_t reqOffsetInBatch,
                         const std::string& cid,
                         ReqId reqSeqNum,
                         ClientPreProcessReqMsgUniquePtr clientReqMsg,
                         PreProcessRequestMsgSharedPtr preProcessRequestMsg,
                         const char* signature = nullptr,
                         const uint32_t signatureLen = 0);
  ~RequestProcessingState() = default;

  // None of RequestProcessingState functions is thread-safe. They are guarded by RequestState::mutex from PreProcessor.
  void handlePrimaryPreProcessed(const char* preProcessResult, uint32_t preProcessResultLen);
  void handlePreProcessReplyMsg(const PreProcessReplyMsgSharedPtr& preProcessReplyMsg);
  std::unique_ptr<MessageBase> buildClientRequestMsg(bool emptyReq = false);
  void setPreProcessRequest(PreProcessRequestMsgSharedPtr preProcessReqMsg);
  const PreProcessRequestMsgSharedPtr& getPreProcessRequest() const { return preProcessRequestMsg_; }
  const auto getClientId() const { return clientId_; }
  const auto getReqOffsetInBatch() const { return reqOffsetInBatch_; }
  const SeqNum getReqSeqNum() const { return reqSeqNum_; }
  PreProcessingResult definePreProcessingConsensusResult();
  const char* getPrimaryPreProcessedResult() const { return primaryPreProcessResult_; }
  uint32_t getPrimaryPreProcessedResultLen() const { return primaryPreProcessResultLen_; }
  bool isReqTimedOut() const;
  const uint64_t reqRetryId() const { return reqRetryId_; }
  uint64_t getReqTimeoutMilli() const {
    return clientPreProcessReqMsg_ ? clientPreProcessReqMsg_->requestTimeoutMilli() : 0;
  }

  const char* getReqSignature() const {
    if (!clientRequestSignature_.empty()) {
      return clientRequestSignature_.data();
    }
    return nullptr;
  }
  uint32_t getReqSignatureLength() const { return clientRequestSignature_.size(); }

  const std::string getReqCid() const { return clientPreProcessReqMsg_ ? clientPreProcessReqMsg_->getCid() : ""; }
  const std::string& getBatchCid() const { return batchCid_; }
  void detectNonDeterministicPreProcessing(const uint8_t* newHash, NodeIdType newSenderId, uint64_t reqRetryId) const;
  void releaseResources();
  ReplicaIdsList getRejectedReplicasList() { return rejectedReplicaIds_; }
  void resetRejectedReplicasList() { rejectedReplicaIds_.clear(); }
  void setPreprocessingRightNow(bool set) { preprocessingRightNow_ = set; }
  const std::list<PreProcessResultSignature>& getPreProcessResultSignatures();

  static void init(uint16_t numOfRequiredReplies, preprocessor::PreProcessorRecorder* histograms);

 private:
  static concord::util::SHA3_256::Digest convertToArray(
      const uint8_t resultsHash[concord::util::SHA3_256::SIZE_IN_BYTES]);
  static uint64_t getMonotonicTimeMilli();
  static logging::Logger& logger() {
    static logging::Logger logger_ = logging::getLogger("concord.preprocessor");
    return logger_;
  }
  auto calculateMaxNbrOfEqualHashes(uint16_t& maxNumOfEqualHashes) const;
  void detectNonDeterministicPreProcessing(const concord::util::SHA3_256::Digest& newHash,
                                           NodeIdType newSenderId,
                                           uint64_t reqRetryId) const;

 private:
  static uint16_t numOfRequiredEqualReplies_;
  static preprocessor::PreProcessorRecorder* preProcessorHistograms_;

  // The use of the class data members is thread-safe. The PreProcessor class uses a per-instance mutex lock for
  // the RequestProcessingState objects.
  const ReplicaId myReplicaId_;
  const uint16_t numOfReplicas_;
  const std::string batchCid_;
  const uint16_t clientId_;
  const uint16_t reqOffsetInBatch_;
  const std::string cid_;
  const ReqId reqSeqNum_;
  const uint64_t entryTime_;
  const std::string clientRequestSignature_;
  ClientPreProcessReqMsgUniquePtr clientPreProcessReqMsg_;
  PreProcessRequestMsgSharedPtr preProcessRequestMsg_;
  uint16_t numOfReceivedReplies_ = 0;
  ReplicaIdsList rejectedReplicaIds_;
  const char* primaryPreProcessResult_ = nullptr;  // This memory is statically pre-allocated per client in PreProcessor
  uint32_t primaryPreProcessResultLen_ = 0;
  concord::util::SHA3_256::Digest primaryPreProcessResultHash_ = {};
  // Maps result hash to a list of replica signatures sent for this hash. Implcitly this also gives the number of
  // replicas returning a specific hash.
  std::map<concord::util::SHA3_256::Digest, std::list<PreProcessResultSignature>> preProcessingResultHashes_;
  bool retrying_ = false;
  bool preprocessingRightNow_ = false;
  uint64_t reqRetryId_ = 0;
};

using RequestProcessingStateUniquePtr = std::unique_ptr<RequestProcessingState>;

}  // namespace preprocessor
