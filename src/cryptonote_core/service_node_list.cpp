// Copyright (c)      2018, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <functional>
#include <random>

#include "ringct/rctSigs.h"
#include "wallet/wallet2.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_basic/tx_extra.h"
#include "common/int-util.h"

#include "service_node_list.h"

#undef LOKI_DEFAULT_LOG_CATEGORY
#define LOKI_DEFAULT_LOG_CATEGORY "service_nodes"

namespace service_nodes
{

  service_node_list::service_node_list(cryptonote::Blockchain& blockchain)
    : m_blockchain(blockchain)
  {
    blockchain.hook_block_added(*this);
    blockchain.hook_blockchain_detached(*this);
    blockchain.hook_init(*this);
    blockchain.hook_validate_miner_tx(*this);
  }

  void service_node_list::init()
  {
    // TODO: Save this calculation, only do it if it's not here.
    LOG_PRINT_L0("Recalculating service nodes list, scanning last 30 days");

    m_service_nodes_infos.clear();

    while (!m_rollback_events.empty())
    {
      m_rollback_events.pop_front();
    }

    uint64_t current_height = m_blockchain.get_current_blockchain_height();
    uint64_t start_height = 0;
    if (current_height >= STAKING_REQUIREMENT_LOCK_BLOCKS + STAKING_RELOCK_WINDOW_BLOCKS)
    {
      start_height = current_height - STAKING_REQUIREMENT_LOCK_BLOCKS - STAKING_RELOCK_WINDOW_BLOCKS;
    }

    for (uint64_t height = start_height; height <= current_height; height += 1000)
    {
      std::list<std::pair<cryptonote::blobdata, cryptonote::block>> blocks;
      if (!m_blockchain.get_blocks(height, 1000, blocks))
      {
        LOG_ERROR("Unable to initialize service nodes list");
        return;
      }

      for (const auto& block_pair : blocks)
      {
        const cryptonote::block& block = block_pair.second;
        std::list<cryptonote::transaction> txs;
        std::list<crypto::hash> missed_txs;
        if (!m_blockchain.get_transactions(block.tx_hashes, txs, missed_txs))
        {
          LOG_ERROR("Unable to get transactions for block " << block.hash);
          return;
        }

        block_added_generic(block, txs);
      }
    }

    m_rollback_events.push_back(std::unique_ptr<rollback_event>(new prevent_rollback(current_height)));
  }

  std::vector<crypto::public_key> service_node_list::get_service_node_pubkeys() const
  {
    std::vector<crypto::public_key> result;
    for (const auto& iter : m_service_nodes_infos)
      result.push_back(iter.first);

    std::sort(result.begin(), result.end(),
      [](const crypto::public_key &a, const crypto::public_key &b) {
        return memcmp(reinterpret_cast<const void*>(&a), reinterpret_cast<const void*>(&b), sizeof(a)) < 0;
      });
    return result;
  }

  const std::shared_ptr<quorum_state> service_node_list::get_quorum_state(uint64_t height) const
  {
    std::shared_ptr<service_nodes::quorum_state> result;
    const auto &it = m_quorum_states.find(height);
    if (it == m_quorum_states.end())
    {
      // TODO(loki): Not being able to find the quorum is going to be a fatal error.
    }
    else
    {
      result = it->second;
    }

    return result;
  }

  bool service_node_list::is_service_node(const crypto::public_key& pubkey) const
  {
    return m_service_nodes_infos.find(pubkey) != m_service_nodes_infos.end();
  }

  bool service_node_list::reg_tx_has_correct_unlock_time(const cryptonote::transaction& tx, uint64_t block_height) const
  {
    return tx.unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER && tx.unlock_time >= block_height + STAKING_REQUIREMENT_LOCK_BLOCKS;
  }

  bool service_node_list::reg_tx_extract_fields(const cryptonote::transaction& tx, std::vector<cryptonote::account_public_address>& addresses, std::vector<uint32_t>& shares, crypto::public_key& service_node_key, crypto::public_key& tx_pub_key) const
  {
    cryptonote::tx_extra_service_node_register registration;
    if (get_service_node_register_from_tx_extra(tx.extra, registration))
    {
      addresses.clear();
      addresses.reserve(registration.m_public_spend_keys.size());
      for (size_t i = 0; i < registration.m_public_spend_keys.size(); i++)
        addresses.push_back(cryptonote::account_public_address{ registration.m_public_spend_keys[i], registration.m_public_view_keys[i] });
      shares = registration.m_shares;
      service_node_key = registration.m_service_node_key;
    }
    else
    {
      addresses.clear();
      shares.clear();
      service_node_key = crypto::null_pkey;
    }
    tx_pub_key = cryptonote::get_tx_pub_key_from_extra(tx.extra);
    return !addresses.empty() &&
      tx_pub_key != crypto::null_pkey &&
      service_node_key != crypto::null_pkey &&
      !shares.empty();
  }

  uint64_t service_node_list::get_reg_tx_staking_output_contribution(const cryptonote::transaction& tx, int i, crypto::key_derivation derivation, hw::device& hwdev) const
  {
    if (tx.vout[i].target.type() != typeid(cryptonote::txout_to_key))
    {
      return 0;
    }

    rct::key mask;
    uint64_t money_transferred = 0;

    crypto::secret_key scalar1;
    hwdev.derivation_to_scalar(derivation, i, scalar1);
    try
    {
      switch (tx.rct_signatures.type)
      {
      case rct::RCTTypeSimple:
      case rct::RCTTypeSimpleBulletproof:
        money_transferred = rct::decodeRctSimple(tx.rct_signatures, rct::sk2rct(scalar1), i, mask, hwdev);
        break;
      case rct::RCTTypeFull:
      case rct::RCTTypeFullBulletproof:
        money_transferred = rct::decodeRct(tx.rct_signatures, rct::sk2rct(scalar1), i, mask, hwdev);
        break;
      default:
        LOG_ERROR("Unsupported rct type: " << tx.rct_signatures.type);
        return 0;
      }
    }
    catch (const std::exception &e)
    {
      LOG_ERROR("Failed to decode input " << i);
      return 0;
    }

    return money_transferred;
  }

  bool service_node_list::is_deregistration_tx(const cryptonote::transaction& tx, crypto::public_key& key) const
  {
    if (tx.version != cryptonote::transaction::version_3_deregister_tx)
      return false;

    cryptonote::tx_extra_service_node_deregister deregister;
    if (!cryptonote::get_service_node_deregister_from_tx_extra(tx.extra, deregister))
    {
      LOG_ERROR("Transaction deregister did not have deregister data in tx extra, possibly corrupt tx in blockchain");
      return false;
    }

    const std::shared_ptr<quorum_state> state = get_quorum_state(deregister.block_height);

    if (!state)
    {
      // TODO(loki): Not being able to find a quorum is fatal! We want better caching abilities.
      LOG_ERROR("Quorum state for height: " << deregister.block_height << ", was not stored by the daemon");
      return false;
    }

    if (deregister.service_node_index >= state->nodes_to_test.size())
    {
      LOG_ERROR("Service node index to vote off has become invalid, quorum rules have changed without a hardfork.");
      return false;
    }

    key = state->nodes_to_test[deregister.service_node_index];

    return true;
  }

  bool service_node_list::is_registration_tx(const cryptonote::transaction& tx, uint64_t block_height, int index, crypto::public_key& key, service_node_info& info) const
  {
    if (!reg_tx_has_correct_unlock_time(tx, block_height))
      return false;

    crypto::public_key tx_pub_key, service_node_key;
    std::vector<cryptonote::account_public_address> service_node_addresses;
    std::vector<uint32_t> service_node_shares;

    if (!reg_tx_extract_fields(tx, service_node_addresses, service_node_shares, service_node_key, tx_pub_key))
      return false;

    uint64_t total = 0;
    for (size_t i = 0; i < service_node_shares.size(); i++)
      total += service_node_shares[i];
    if (total > STAKING_SHARES)
      return false;

    if (is_service_node(service_node_key))
      return false;

    // TODO: check service_node_key has signed service node addresses and
    // service node shares and timestamp, and that timestamp is not old.

    cryptonote::keypair gov_key = cryptonote::get_deterministic_keypair_from_height(1);

    if (tx.vout.size() < service_node_addresses.size())
      return false;

    uint64_t transferred = 0;
    for (size_t i = 0; i < service_node_addresses.size(); i++)
    {
      crypto::key_derivation derivation;
      crypto::generate_key_derivation(service_node_addresses[i].m_view_public_key, gov_key.sec, derivation);

      hw::device& hwdev = hw::get_device("default");

      // TODO: check if output has correct unlock time here, when unlock time is done per output.
      transferred += get_reg_tx_staking_output_contribution(tx, i, derivation, hwdev);
    }

    if (transferred < m_blockchain.get_staking_requirement(block_height))
      return false;

    key = service_node_key;

    info.block_height = block_height;
    info.transaction_index = index;
    info.addresses = service_node_addresses;
    info.shares = service_node_shares;

    return true;
  }

  void service_node_list::block_added(const cryptonote::block& block, const std::vector<cryptonote::transaction>& txs)
  {
    block_added_generic(block, txs);
  }


  template<typename T>
  void service_node_list::block_added_generic(const cryptonote::block& block, const T& txs)
  {
    uint64_t block_height = cryptonote::get_block_height(block);
    int hard_fork_version = m_blockchain.get_hard_fork_version(block_height);

    if (hard_fork_version < 9)
      return;

    while (!m_rollback_events.empty() && m_rollback_events.front()->m_block_height < block_height - ROLLBACK_EVENT_EXPIRATION_BLOCKS)
    {
      m_rollback_events.pop_front();
    }

    crypto::public_key winner_pubkey = cryptonote::get_service_node_winner_from_tx_extra(block.miner_tx.extra);
    if (m_service_nodes_infos.count(winner_pubkey) == 1)
    {
      m_rollback_events.push_back(
        std::unique_ptr<rollback_event>(
          new rollback_change(block_height, winner_pubkey, m_service_nodes_infos[winner_pubkey])
        )
      );
      // set the winner as though it was re-registering at transaction index=-1 for this block
      m_service_nodes_infos[winner_pubkey].block_height = 0;
      m_service_nodes_infos[winner_pubkey].transaction_index = -1;
    }

    for (const crypto::public_key& pubkey : get_expired_nodes(block_height))
    {
      auto i = m_service_nodes_infos.find(pubkey);
      if (i != m_service_nodes_infos.end())
      {
        m_rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(block_height, pubkey, i->second)));
        m_service_nodes_infos.erase(i);
      }
      // Service nodes may expire early if they double staked by accident, so
      // expiration doesn't mean the node is in the list.
    }

    int index = 0;
    for (const cryptonote::transaction& tx : txs)
    {
      crypto::public_key key;
      service_node_info info;
      if (is_registration_tx(tx, block_height, index, key, info))
      {
        auto iter = m_service_nodes_infos.find(key);
        if (iter == m_service_nodes_infos.end())
        {
          m_rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_new(block_height, key)));
          m_service_nodes_infos[key] = info;
        }
        else
        {
          MDEBUG("Detected stake using an existing service node key, funds were locked for no reward");
        }
      }
      else if (is_deregistration_tx(tx, key))
      {
        auto iter = m_service_nodes_infos.find(key);
        if (iter != m_service_nodes_infos.end())
        {
          m_rollback_events.push_back(std::unique_ptr<rollback_event>(new rollback_change(block_height, key, iter->second)));
          m_service_nodes_infos.erase(iter);
        }
        else
        {
          MWARNING("Tried to kick off a service node that is no longer registered");
        }
      }
      index++;
    }

    const uint64_t curr_height           = m_blockchain.get_current_blockchain_height();

    const size_t QUORUM_LIFETIME         = loki::service_node_deregister::DEREGISTER_LIFETIME_BY_HEIGHT;
    const size_t cache_state_from_height = (curr_height < QUORUM_LIFETIME) ? 0 : curr_height - QUORUM_LIFETIME;

    if (block_height >= cache_state_from_height)
    {
      store_quorum_state_from_rewards_list(block_height);

      while (!m_quorum_states.empty() && m_quorum_states.begin()->first < cache_state_from_height)
      {
        m_quorum_states.erase(m_quorum_states.begin());
      }
    }
  }

  void service_node_list::blockchain_detached(uint64_t height)
  {
    while (!m_rollback_events.empty() && m_rollback_events.back()->m_block_height >= height)
    {
      if (!m_rollback_events.back()->apply(m_service_nodes_infos))
      {
        init();
        break;
      }
      m_rollback_events.pop_back();
    }

    while (!m_quorum_states.empty() && (--m_quorum_states.end())->first >= height)
    {
      m_quorum_states.erase(--m_quorum_states.end());
    }
  }

  std::vector<crypto::public_key> service_node_list::get_expired_nodes(uint64_t block_height) const
  {
    std::vector<crypto::public_key> expired_nodes;

    if (block_height < STAKING_REQUIREMENT_LOCK_BLOCKS + STAKING_RELOCK_WINDOW_BLOCKS)
      return expired_nodes;

    const uint64_t expired_nodes_block_height = block_height - STAKING_REQUIREMENT_LOCK_BLOCKS - STAKING_RELOCK_WINDOW_BLOCKS;

    std::list<std::pair<cryptonote::blobdata, cryptonote::block>> blocks;
    if (!m_blockchain.get_blocks(expired_nodes_block_height, 1, blocks))
    {
      LOG_ERROR("Unable to get historical blocks");
      return expired_nodes;
    }

    const cryptonote::block& block = blocks.begin()->second;
    std::list<cryptonote::transaction> txs;
    std::list<crypto::hash> missed_txs;
    if (!m_blockchain.get_transactions(block.tx_hashes, txs, missed_txs))
    {
      LOG_ERROR("Unable to get transactions for block " << block.hash);
      return expired_nodes;
    }

    int index = 0;
    for (const cryptonote::transaction& tx : txs)
    {
      crypto::public_key key;
      service_node_info info;
      if (is_registration_tx(tx, expired_nodes_block_height, index, key, info))
      {
        expired_nodes.push_back(key);
      }
      index++;
    }

    return expired_nodes;
  }

  std::vector<std::pair<cryptonote::account_public_address, uint32_t>> service_node_list::get_winner_addresses_and_shares(const crypto::hash& prev_id) const
  {
    crypto::public_key key = select_winner(prev_id);
    if (key == crypto::null_pkey)
      return { std::make_pair(null_address, STAKING_SHARES) };
    std::vector<std::pair<cryptonote::account_public_address, uint32_t>> winners;
    for (size_t i = 0; i < m_service_nodes_infos.at(key).addresses.size(); i++)
      winners.push_back(std::make_pair(m_service_nodes_infos.at(key).addresses[i], m_service_nodes_infos.at(key).shares[i]));
    return winners;
  }

  crypto::public_key service_node_list::select_winner(const crypto::hash& prev_id) const
  {
    auto oldest_waiting = std::pair<uint64_t, int>(std::numeric_limits<uint64_t>::max(), std::numeric_limits<int>::max());
    crypto::public_key key = crypto::null_pkey;
    for (const auto& info : m_service_nodes_infos)
    {
      auto waiting_since = std::make_pair(info.second.block_height, info.second.transaction_index);
      if (waiting_since < oldest_waiting)
      {
        waiting_since = oldest_waiting;
        key = info.first;
      }
    }
    return key;
  }

  /// validates the miner TX for the next block
  //
  bool service_node_list::validate_miner_tx(const crypto::hash& prev_id, const cryptonote::transaction& miner_tx, uint64_t height, int hard_fork_version, uint64_t base_reward)
  {
    if (hard_fork_version < 9)
      return true;

    uint64_t total_service_node_reward = cryptonote::get_service_node_reward(height, base_reward, hard_fork_version);

    crypto::public_key winner = select_winner(prev_id);

    crypto::public_key check_winner_pubkey = cryptonote::get_service_node_winner_from_tx_extra(miner_tx.extra);
    if (check_winner_pubkey != winner)
      return false;

    const std::vector<cryptonote::account_public_address> addresses =
      winner == crypto::null_pkey
        ? std::vector<cryptonote::account_public_address>{ null_address }
        : m_service_nodes_infos.at(winner).addresses;

    const std::vector<uint32_t> shares =
      winner == crypto::null_pkey
        ? std::vector<uint32_t>{ STAKING_SHARES }
        : m_service_nodes_infos.at(winner).shares;

    if (miner_tx.vout.size() - 1 < addresses.size())
      return false;

    for (size_t i = 0; i < addresses.size(); i++)
    {
      size_t vout_index = miner_tx.vout.size() - 1 /* governance */ - addresses.size() + i;

      uint64_t reward = cryptonote::get_share_of_reward(shares[i], total_service_node_reward);

      if (miner_tx.vout[vout_index].amount != reward)
      {
        MERROR("Service node reward amount incorrect. Should be " << cryptonote::print_money(reward) << ", is: " << cryptonote::print_money(miner_tx.vout[vout_index].amount));
        return false;
      }

      if (miner_tx.vout[vout_index].target.type() != typeid(cryptonote::txout_to_key))
      {
        MERROR("Service node output target type should be txout_to_key");
        return false;
      }

      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);;
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      cryptonote::keypair gov_key = cryptonote::get_deterministic_keypair_from_height(height);

      bool r = crypto::generate_key_derivation(addresses[i].m_view_public_key, gov_key.sec, derivation);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << addresses[i].m_view_public_key << ", " << gov_key.sec << ")");
      r = crypto::derive_public_key(derivation, vout_index, addresses[i].m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << vout_index << ", "<< addresses[i].m_spend_public_key << ")");

      if (boost::get<cryptonote::txout_to_key>(miner_tx.vout[vout_index].target).key != out_eph_public_key)
      {
        MERROR("Invalid service node reward output");
        return false;
      }
    }

    return true;
  }

  void service_node_list::store_quorum_state_from_rewards_list(uint64_t height)
  {
    const crypto::hash block_hash = m_blockchain.get_block_id_by_height(height);
    if (block_hash == crypto::null_hash)
    {
      MERROR("Block height: " << height << " returned null hash");
      return;
    }

    std::vector<crypto::public_key> full_node_list = get_service_node_pubkeys();
    std::vector<size_t>                              pub_keys_indexes(full_node_list.size());
    {
      size_t index = 0;
      for (size_t i = 0; i < full_node_list.size(); i++) { pub_keys_indexes[i] = i; }

      // Shuffle indexes
      uint64_t seed = 0;
      std::memcpy(&seed, block_hash.data, std::min(sizeof(seed), sizeof(block_hash.data)));

      std::mt19937_64 mersenne_twister(seed);
      std::shuffle(pub_keys_indexes.begin(), pub_keys_indexes.end(), mersenne_twister);
    }

    // Assign indexes from shuffled list into quorum and list of nodes to test
    if (!m_quorum_states[height])
      m_quorum_states[height] = std::shared_ptr<quorum_state>(new quorum_state());

    std::shared_ptr<quorum_state> state = m_quorum_states[height];
    state->clear();
    {
      std::vector<crypto::public_key>& quorum = state->quorum_nodes;
      {
        quorum.clear();
        quorum.resize(std::min(full_node_list.size(), QUORUM_SIZE));
        for (size_t i = 0; i < quorum.size(); i++)
        {
          size_t node_index                   = pub_keys_indexes[i];
          const crypto::public_key &key = full_node_list[node_index];
          quorum[i] = key;
        }
      }

      std::vector<crypto::public_key>& nodes_to_test = state->nodes_to_test;
      {
        size_t num_remaining_nodes = pub_keys_indexes.size() - quorum.size();
        size_t num_nodes_to_test   = std::max(num_remaining_nodes/NTH_OF_THE_NETWORK_TO_TEST,
                                              std::min(MIN_NODES_TO_TEST, num_remaining_nodes));

        nodes_to_test.clear();
        nodes_to_test.resize(num_nodes_to_test);

        const int pub_keys_offset = quorum.size();
        for (size_t i = 0; i < nodes_to_test.size(); i++)
        {
          size_t node_index             = pub_keys_indexes[pub_keys_offset + i];
          const crypto::public_key &key = full_node_list[node_index];
          nodes_to_test[i]              = key;
        }
      }
    }
  }
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  service_node_list::rollback_event::rollback_event(uint64_t block_height) : m_block_height(block_height)
  {
  }

  service_node_list::rollback_change::rollback_change(uint64_t block_height, const crypto::public_key& key, const service_node_info& info)
    : service_node_list::rollback_event(block_height), m_key(key), m_info(info)
  {
  }

  bool service_node_list::rollback_change::apply(std::unordered_map<crypto::public_key, service_node_info>& service_nodes_infos) const
  {
    service_nodes_infos[m_key] = m_info;
    return true;
  }

  service_node_list::rollback_new::rollback_new(uint64_t block_height, const crypto::public_key& key)
    : service_node_list::rollback_event(block_height), m_key(key)
  {
  }

  bool service_node_list::rollback_new::apply(std::unordered_map<crypto::public_key, service_node_info>& service_nodes_infos) const
  {
    auto iter = service_nodes_infos.find(m_key);
    if (iter == service_nodes_infos.end())
    {
      MERROR("Could not find service node pubkey in rollback new");
      return false;
    }
    service_nodes_infos.erase(iter);
    return true;
  }

  service_node_list::prevent_rollback::prevent_rollback(uint64_t block_height) : service_node_list::rollback_event(block_height)
  {
  }

  bool service_node_list::prevent_rollback::apply(std::unordered_map<crypto::public_key, service_node_info>& service_nodes_infos) const
  {
    MERROR("Unable to rollback any further!");
    return false;
  }
}
