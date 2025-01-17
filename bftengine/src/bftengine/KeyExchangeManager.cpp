// Concord
//
// Copyright (c) 2020 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License"). You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the sub-component's license, as noted in the LICENSE
// file.

#include "bftengine/KeyExchangeManager.hpp"
#include "Replica.hpp"
#include "InternalBFTClient.hpp"
#include "kvstream.h"
#include "json_output.hpp"
#include "SigManager.hpp"
#include <thread>

namespace bftEngine::impl {

KeyExchangeManager::KeyExchangeManager(InitData* id)
    : repID_{ReplicaConfig::instance().getreplicaId()},
      clusterSize_{ReplicaConfig::instance().getnumReplicas()},
      publicKeys_{clusterSize_},
      private_keys_(id->secretsMgr),
      clientsPublicKeys_(),
      client_(id->cl),
      multiSigKeyHdlr_(id->kg),
      clientPublicKeyStore_{id->cpks},
      timers_(*(id->timers)) {
  registerForNotification(id->ke);
  notifyRegistry();
  if (!ReplicaConfig::instance().getkeyExchangeOnStart()) initial_exchange_ = true;
  if (!ReplicaConfig::instance().get("concord.bft.keyExchage.clientKeysEnabled", true)) {
    LOG_INFO(KEY_EX_LOG, "Publish client keys is disabled");
    clientsPublicKeys_.published_ = true;
  }
}

void KeyExchangeManager::initMetrics(std::shared_ptr<concordMetrics::Aggregator> a, std::chrono::seconds interval) {
  metrics_.reset(new Metrics(a, interval));
  metrics_->component.Register();
  metricsTimer_ = timers_.add(
      std::chrono::milliseconds(100), concordUtil::Timers::Timer::RECURRING, [this](concordUtil::Timers::Handle h) {
        metrics_->component.UpdateAggregator();
        auto currTime =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch());
        if (currTime - metrics_->lastMetricsDumpTime >= metrics_->metricsDumpIntervalInSec) {
          metrics_->lastMetricsDumpTime = currTime;
          LOG_INFO(KEY_EX_LOG, "-- KeyExchangeManager metrics dump--" + metrics_->component.ToJson());
        }
      });
}

std::string KeyExchangeManager::generateCid(std::string cid) {
  auto now = getMonotonicTime().time_since_epoch();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now);
  auto sn = now_ms.count();
  cid += std::to_string(repID_) + "-" + std::to_string(sn);
  return cid;
}

std::string KeyExchangeManager::onKeyExchange(const KeyExchangeMsg& kemsg, const SeqNum& sn, const std::string& cid) {
  SCOPED_MDC_SEQ_NUM(std::to_string(sn));
  LOG_INFO(KEY_EX_LOG, kemsg.toString() << KVLOG(sn, cid, exchanged()));
  // client query
  if (kemsg.op == KeyExchangeMsg::HAS_KEYS) {
    LOG_INFO(KEY_EX_LOG, "Has keys: " << std::boolalpha << exchanged() << std::noboolalpha);
    if (!exchanged()) return std::string(KeyExchangeMsg::hasKeysFalseReply);
    return std::string(KeyExchangeMsg::hasKeysTrueReply);
  }

  publicKeys_.push(kemsg, sn);
  if (kemsg.repID == repID_) {  // initiated by me
    ConcordAssert(private_keys_.key_data().generated.pub == kemsg.pubkey);
    private_keys_.onKeyExchange(cid, sn);
    for (auto e : registryToExchange_) e->onPrivateKeyExchange(private_keys_.key_data().keys[sn], kemsg.pubkey, sn);
    metrics_->self_key_exchange_counter++;
  } else {  // initiated by others
    for (auto e : registryToExchange_) e->onPublicKeyExchange(kemsg.pubkey, kemsg.repID, sn);
    metrics_->public_key_exchange_for_peer_counter++;
  }
  if (ReplicaConfig::instance().getkeyExchangeOnStart() && (publicKeys_.numOfExchangedReplicas() <= clusterSize_))
    LOG_INFO(KEY_EX_LOG, "Exchanged [" << publicKeys_.numOfExchangedReplicas() << "] out of [" << clusterSize_ << "]");
  if (!initial_exchange_ && exchanged()) initial_exchange_ = true;
  return "ok";
}

void KeyExchangeManager::notifyRegistry() {
  // sort public keys by sequence number in order to "replay" ordered key exchange operations for proper initialization
  // sequence number may be not unique due to batching - therefore a multimap
  // seqnum -> [repid, pubkey]
  std::multimap<SeqNum, std::pair<uint16_t, std::string>> ordered_public_keys;
  for (uint32_t i = 0; i < clusterSize_; i++) {
    if (!publicKeys_.keyExists(i)) continue;
    for (auto [sn, pk] : publicKeys_.keys(i).keys) ordered_public_keys.insert({sn, std::make_pair(i, pk)});
  }

  for (auto [sn, pk_info] : ordered_public_keys)
    for (auto ke : registryToExchange_) ke->onPublicKeyExchange(pk_info.second, pk_info.first, sn);

  for (auto ke : registryToExchange_)
    for (auto [sn, pk] : private_keys_.key_data().keys)
      ke->onPrivateKeyExchange(pk, publicKeys_.getKey(repID_, sn), sn);
}

void KeyExchangeManager::loadPublicKeys() {
  // after State Transfer public keys for all replicas are expected to exist
  auto num_loaded = publicKeys_.loadAllReplicasKeyStoresFromReservedPages();
  if (ReplicaConfig::instance().getkeyExchangeOnStart()) ConcordAssert(num_loaded == clusterSize_);
  LOG_INFO(KEY_EX_LOG, "building crypto system after state transfer");
  notifyRegistry();
}

void KeyExchangeManager::sendKeyExchange(const SeqNum& sn) {
  // first check whether we've already generated keys lately
  if (private_keys_.lastGeneratedSeqnum() &&  // if not init  ial
      (sn - private_keys_.lastGeneratedSeqnum()) / checkpointWindowSize < 2) {
    LOG_INFO(KEY_EX_LOG, "ignore request - already generated keys for seqnum: " << private_keys_.lastGeneratedSeqnum());
    return;
  }
  KeyExchangeMsg msg;
  auto cid = generateCid(kInitialKeyExchangeCid);
  auto [prv, pub] = multiSigKeyHdlr_->generateMultisigKeyPair();
  private_keys_.key_data().generated.priv = prv;
  private_keys_.key_data().generated.pub = pub;
  private_keys_.key_data().generated.cid = cid;
  private_keys_.key_data().generated.sn = sn;
  private_keys_.save();

  LOG_INFO(KEY_EX_LOG, "Sending key exchange :" << KVLOG(cid, pub));
  msg.pubkey = pub;
  msg.repID = repID_;
  std::stringstream ss;
  concord::serialize::Serializable::serialize(ss, msg);
  auto strMsg = ss.str();
  client_->sendRequest(bftEngine::KEY_EXCHANGE_FLAG, strMsg.size(), strMsg.c_str(), cid);
  metrics_->sent_key_exchange_counter++;
}

// sends the clients public keys via the internal client, if keys weren't published or outdated.
void KeyExchangeManager::sendInitialClientsKeys(const std::string& keys) {
  if (clientsPublicKeys_.published_) {
    LOG_INFO(KEY_EX_LOG, "Clients public keys were already published");
    return;
  }
  auto ret = std::async(std::launch::async, [this, keys]() {
    while (!initial_exchange_) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    auto cid = generateCid(kInitialClientsKeysCid);
    LOG_INFO(KEY_EX_LOG, "Sends clients keys, cid: " << cid << " payload size " << keys.size());
    client_->sendRequest(
        bftEngine::CLIENTS_PUB_KEYS_FLAG | bftEngine::PUBLISH_ON_CHAIN_OBJECT_FLAG, keys.size(), keys.c_str(), cid);
  });
}

void KeyExchangeManager::onPublishClientsKeys(const std::string& keys, std::optional<std::string> bootstrap_keys) {
  auto save = true;
  if (bootstrap_keys) {
    if (keys != *bootstrap_keys) {
      LOG_FATAL(KEY_EX_LOG, "Initial Published Clients keys and replica client keys do not match");
      ConcordAssertEQ(keys, *bootstrap_keys);
    }
    if (clientKeysPublished()) save = false;
  }
  if (save) saveClientsPublicKeys(keys);
}

void KeyExchangeManager::onClientPublicKeyExchange(const std::string& key,
                                                   concord::util::crypto::KeyFormat fmt,
                                                   NodeIdType clientId) {
  LOG_INFO(KEY_EX_LOG, "key: " << key << " fmt: " << (uint16_t)fmt << " client: " << clientId);
  // persist a new key
  clientPublicKeyStore_->setClientPublicKey(clientId, key, fmt);
  // load a new key
  loadClientPublicKey(key, fmt, clientId, true);
}

void KeyExchangeManager::loadClientPublicKey(const std::string& key,
                                             concord::util::crypto::KeyFormat fmt,
                                             NodeIdType clientId,
                                             bool saveToReservedPages) {
  LOG_INFO(KEY_EX_LOG, "key: " << key << " fmt: " << (uint16_t)fmt << " client: " << clientId);
  SigManager::instance()->setClientPublicKey(key, clientId, fmt);
  if (saveToReservedPages) saveClientsPublicKeys(SigManager::instance()->getClientsPublicKeys());
}

void KeyExchangeManager::sendInitialKey() {
  LOG_INFO(KEY_EX_LOG, "");
  if (private_keys_.hasGeneratedKeys()) {
    LOG_INFO(KEY_EX_LOG, "Replica has already generated keys");
    return;
  }
  // First Key exchange is on start, in order not to trigger view change, we'll wait for all replicas to be connected.
  // In order not to block it's done as async operation.
  auto ret = std::async(std::launch::async, [this]() {
    SCOPED_MDC(MDC_REPLICA_ID_KEY, std::to_string(ReplicaConfig::instance().replicaId));
    waitForFullCommunication();
    sendKeyExchange(0);
    metrics_->sent_key_exchange_on_start_status.Get().Set("True");
  });
}

void KeyExchangeManager::waitForFullCommunication() {
  auto avlble = client_->numOfConnectedReplicas(clusterSize_);
  LOG_INFO(KEY_EX_LOG, "Consensus engine: " << avlble << " replicas are connected");
  // Num of connections should be: (clusterSize - 1)
  while (avlble < clusterSize_ - 1) {
    LOG_INFO(KEY_EX_LOG, "Consensus engine not available, " << avlble << " replicas are connected");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    avlble = client_->numOfConnectedReplicas(clusterSize_);
  }
  // If transport is UDP, we can't know the connection status, and we are in Apollo context therefore giving 2sec grace.
  if (client_->isUdp()) {
    LOG_INFO(KEY_EX_LOG, "UDP communication");
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  LOG_INFO(KEY_EX_LOG, "Consensus engine available, " << avlble << " replicas are connected");
}

std::string KeyExchangeManager::getStatus() const {
  using concordUtils::toPair;
  std::ostringstream oss;
  std::unordered_map<std::string, std::string> result;
  result.insert(toPair("exchanged", exchanged()));
  result.insert(toPair("sent_key_exchange_on_start", metrics_->sent_key_exchange_on_start_status.Get().Get()));
  result.insert(toPair("sent_key_exchange", metrics_->sent_key_exchange_counter.Get().Get()));
  result.insert(toPair("self_key_exchange", metrics_->self_key_exchange_counter.Get().Get()));
  result.insert(toPair("public_key_exchange_for_peer", metrics_->public_key_exchange_for_peer_counter.Get().Get()));

  oss << concordUtils::kContainerToJson(result);
  return oss.str();
}

bool KeyExchangeManager::PrivateKeys::load() {
  LOG_INFO(KEY_EX_LOG, "");
  auto secrets = secretsMgr_->decryptFile(secrets_file_);
  if (!secrets.has_value()) {
    LOG_WARN(KEY_EX_LOG, "Got empty string from loader. This is expected on first startup with an empty database");
    return false;
  }
  std::stringstream ss;
  std::string str = secrets.value();
  ss.write(str.c_str(), std::streamsize(str.size()));
  concord::serialize::Serializable::deserialize(ss, data_);
  if (data_.generated.sn)
    LOG_INFO(
        KEY_EX_LOG,
        "loaded generated private key for: " << KVLOG(data_.generated.sn, data_.generated.cid, data_.generated.pub));
  for (const auto& it : data_.keys) LOG_INFO(KEY_EX_LOG, "loaded private key for sn: " << it.first);
  return true;
}

}  // namespace bftEngine::impl
