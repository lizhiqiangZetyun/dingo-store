// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
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

#include "sdk/raw_kv_impl.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "glog/logging.h"
#include "sdk/client.h"
#include "sdk/common.h"
#include "sdk/meta_cache.h"
#include "sdk/status.h"
#include "sdk/store_rpc.h"
#include "sdk/store_rpc_controller.h"

namespace dingodb {
namespace sdk {

RawKV::RawKVImpl::RawKVImpl(const ClientStub& stub) : stub_(stub) {}

Status RawKV::RawKVImpl::Get(const std::string& key, std::string& value) {
  std::shared_ptr<MetaCache> meta_cache = stub_.GetMetaCache();

  std::shared_ptr<Region> region;
  Status got = meta_cache->LookupRegionByKey(key, region);
  if (!got.IsOK()) {
    return got;
  }

  KvGetRpc rpc;
  FillRpcContext(*rpc.MutableRequest()->mutable_context(), region->RegionId(), region->Epoch());
  rpc.MutableRequest()->set_key(key);

  StoreRpcController controller(stub_, rpc, region);
  Status call = controller.Call();
  if (call.IsOK()) {
    value = rpc.Response()->value();
  }
  return call;
}

void RawKV::RawKVImpl::ProcessSubBatchGet(SubBatchState* sub) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<KvBatchGetRpc*>(sub->rpc));

  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  Status call = controller.Call();
  if (call.IsOK()) {
    for (const auto& kv : rpc->Response()->kvs()) {
      sub->result_kvs.push_back({kv.key(), kv.value()});
    }
  }
  sub->status = call;
}

Status RawKV::RawKVImpl::BatchGet(const std::vector<std::string>& keys, std::vector<KVPair>& kvs) {
  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<std::string>> region_keys;

  for (const auto& key : keys) {
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    region_keys[tmp->RegionId()].push_back(key);
  }

  std::vector<SubBatchState> sub_batch_state;
  std::vector<std::unique_ptr<KvBatchGetRpc>> rpcs;

  for (const auto& entry : region_keys) {
    auto region_id = entry.first;

    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvBatchGetRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());
    for (const auto& key : entry.second) {
      auto* fill = rpc->MutableRequest()->add_keys();
      *fill = key;
    }

    sub_batch_state.emplace_back(rpc.get(), region);
    rpcs.push_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), region_keys.size());
  CHECK_EQ(rpcs.size(), sub_batch_state.size());

  std::vector<std::thread> thread_pool;
  for (auto i = 1; i < sub_batch_state.size(); i++) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchGet, this, &sub_batch_state[i]);
  }

  ProcessSubBatchGet(sub_batch_state.data());

  for (auto& thread : thread_pool) {
    thread.join();
  }

  Status result;

  std::vector<KVPair> tmp_kvs;
  for (auto& state : sub_batch_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    } else {
      tmp_kvs.insert(tmp_kvs.end(), std::make_move_iterator(state.result_kvs.begin()),
                     std::make_move_iterator(state.result_kvs.end()));
    }
  }

  kvs = std::move(tmp_kvs);

  return result;
}

Status RawKV::RawKVImpl::Put(const std::string& key, const std::string& value) {
  std::shared_ptr<MetaCache> meta_cache = stub_.GetMetaCache();

  std::shared_ptr<Region> region;
  Status got = meta_cache->LookupRegionByKey(key, region);
  if (!got.IsOK()) {
    return got;
  }

  KvPutRpc rpc;
  auto* kv = rpc.MutableRequest()->mutable_kv();
  FillRpcContext(*rpc.MutableRequest()->mutable_context(), region->RegionId(), region->Epoch());
  kv->set_key(key);
  kv->set_value(value);

  StoreRpcController controller(stub_, rpc, region);
  return controller.Call();
}

void RawKV::RawKVImpl::ProcessSubBatchPut(SubBatchState* sub) {
  (void)CHECK_NOTNULL(dynamic_cast<KvBatchPutRpc*>(sub->rpc));
  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  sub->status = controller.Call();
}

Status RawKV::RawKVImpl::BatchPut(const std::vector<KVPair>& kvs) {
  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<KVPair>> region_kvs;

  for (const auto& kv : kvs) {
    auto key = kv.key;
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    region_kvs[tmp->RegionId()].push_back(kv);
  }

  std::vector<SubBatchState> sub_batch_put_state;
  std::vector<std::unique_ptr<KvBatchPutRpc>> rpcs;
  for (const auto& entry : region_kvs) {
    auto region_id = entry.first;

    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvBatchPutRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());
    for (const auto& kv : entry.second) {
      auto* fill = rpc->MutableRequest()->add_kvs();
      fill->set_key(kv.key);
      fill->set_value(kv.value);
    }

    sub_batch_put_state.emplace_back(rpc.get(), region);
    rpcs.emplace_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), region_kvs.size());
  CHECK_EQ(rpcs.size(), sub_batch_put_state.size());

  std::vector<std::thread> thread_pool;
  for (auto i = 1; i < sub_batch_put_state.size(); i++) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchPut, this, &sub_batch_put_state[i]);
  }

  ProcessSubBatchPut(sub_batch_put_state.data());

  for (auto& thread : thread_pool) {
    thread.join();
  }

  Status result;
  for (auto& state : sub_batch_put_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    }
  }

  return result;
}

Status RawKV::RawKVImpl::PutIfAbsent(const std::string& key, const std::string& value, bool& state) {
  std::shared_ptr<MetaCache> meta_cache = stub_.GetMetaCache();

  std::shared_ptr<Region> region;
  Status result = meta_cache->LookupRegionByKey(key, region);
  if (result.IsOK()) {
    KvPutIfAbsentRpc rpc;
    FillRpcContext(*rpc.MutableRequest()->mutable_context(), region->RegionId(), region->Epoch());

    auto* kv = rpc.MutableRequest()->mutable_kv();
    kv->set_key(key);
    kv->set_value(value);

    StoreRpcController controller(stub_, rpc, region);
    result = controller.Call();
    if (result.IsOK()) {
      state = rpc.Response()->key_state();
    }
  }

  return result;
}

void RawKV::RawKVImpl::ProcessSubBatchPutIfAbsent(SubBatchState* sub) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<KvBatchPutIfAbsentRpc*>(sub->rpc));
  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  Status call = controller.Call();

  if (call.IsOK()) {
    CHECK_EQ(rpc->Request()->kvs_size(), rpc->Response()->key_states_size());
    for (auto i = 0; i < rpc->Request()->kvs_size(); i++) {
      sub->key_op_states.push_back({rpc->Request()->kvs(i).key(), rpc->Response()->key_states(i)});
    }
  }

  sub->status = call;
}

Status RawKV::RawKVImpl::BatchPutIfAbsent(const std::vector<KVPair>& kvs, std::vector<KeyOpState>& states) {
  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<KVPair>> region_kvs;

  for (const auto& kv : kvs) {
    auto key = kv.key;
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    region_kvs[tmp->RegionId()].push_back(kv);
  }

  std::vector<SubBatchState> sub_batch_state;
  std::vector<std::unique_ptr<KvBatchPutIfAbsentRpc>> rpcs;
  for (const auto& entry : region_kvs) {
    auto region_id = entry.first;

    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvBatchPutIfAbsentRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());
    for (const auto& kv : entry.second) {
      auto* fill = rpc->MutableRequest()->add_kvs();
      fill->set_key(kv.key);
      fill->set_value(kv.value);
    }
    rpc->MutableRequest()->set_is_atomic(true);

    sub_batch_state.emplace_back(rpc.get(), region);
    rpcs.emplace_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), region_kvs.size());
  CHECK_EQ(rpcs.size(), sub_batch_state.size());

  std::vector<std::thread> thread_pool;
  for (auto i = 1; i < sub_batch_state.size(); i++) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchPutIfAbsent, this, &sub_batch_state[i]);
  }

  ProcessSubBatchPutIfAbsent(sub_batch_state.data());

  for (auto& thread : thread_pool) {
    thread.join();
  }

  Status result;
  std::vector<KeyOpState> tmp_states;
  for (auto& state : sub_batch_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    } else {
      tmp_states.insert(tmp_states.end(), std::make_move_iterator(state.key_op_states.begin()),
                        std::make_move_iterator(state.key_op_states.end()));
    }
  }

  states = std::move(tmp_states);

  return result;
}

Status RawKV::RawKVImpl::Delete(const std::string& key) {
  std::shared_ptr<MetaCache> meta_cache = stub_.GetMetaCache();

  std::shared_ptr<Region> region;
  Status ret = meta_cache->LookupRegionByKey(key, region);
  if (ret.IsOK()) {
    KvBatchDeleteRpc rpc;
    FillRpcContext(*rpc.MutableRequest()->mutable_context(), region->RegionId(), region->Epoch());
    auto* fill = rpc.MutableRequest()->add_keys();
    *fill = key;

    StoreRpcController controller(stub_, rpc, region);
    ret = controller.Call();
    if (!ret.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << rpc.Method() << " send to region: " << region->RegionId()
                         << " fail: " << ret.ToString();
    }
  }

  return ret;
}

void RawKV::RawKVImpl::ProcessSubBatchDelete(SubBatchState* sub) {
  (void)CHECK_NOTNULL(dynamic_cast<KvBatchDeleteRpc*>(sub->rpc));
  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  sub->status = controller.Call();
}

Status RawKV::RawKVImpl::BatchDelete(const std::vector<std::string>& keys) {
  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<std::string>> region_keys;

  for (const auto& key : keys) {
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }

    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    region_keys[tmp->RegionId()].emplace_back(key);
  }

  std::vector<SubBatchState> sub_batch_state;
  std::vector<std::unique_ptr<KvBatchDeleteRpc>> rpcs;
  for (const auto& entry : region_keys) {
    auto region_id = entry.first;

    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvBatchDeleteRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());

    for (const auto& key : entry.second) {
      *(rpc->MutableRequest()->add_keys()) = key;
    }

    sub_batch_state.emplace_back(rpc.get(), region);
    rpcs.emplace_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), region_keys.size());
  CHECK_EQ(rpcs.size(), sub_batch_state.size());

  std::vector<std::thread> thread_pool;
  for (auto i = 1; i < sub_batch_state.size(); i++) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchDelete, this, &sub_batch_state[i]);
  }

  ProcessSubBatchDelete(sub_batch_state.data());

  for (auto& thread : thread_pool) {
    thread.join();
  }

  Status result;
  for (auto& state : sub_batch_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    }
  }

  return result;
}

void RawKV::RawKVImpl::ProcessSubBatchDeleteRange(SubBatchState* sub) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<KvDeleteRangeRpc*>(sub->rpc));
  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  sub->status = controller.Call();
  sub->delete_count = rpc->Response()->delete_count();
}

Status RawKV::RawKVImpl::DeleteRange(const std::string& start, const std::string& end, bool with_start, bool with_end,
                                     int64_t& delete_count) {
  if (start >= end) {
    return Status::IllegalState("start key must < end key");
  }

  struct DeleteRangeContext {
    std::string start;
    bool with_start;
    std::string end;
    bool with_end;
  };

  auto meta_cache = stub_.GetMetaCache();

  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<DeleteRangeContext>> to_delete;

  std::shared_ptr<Region> tmp;
  Status got = meta_cache->LookupRegionByKey(start, tmp);
  if (!got.IsOK()) {
    return got;
  }
  CHECK_NOTNULL(tmp.get());

  std::string next;
  bool delete_end_key = false;

  {
    // process start key
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    if (end < tmp->Range().end_key()) {
      to_delete[tmp->RegionId()].push_back({start, with_start, end, with_end});
    } else if (end > tmp->Range().end_key()) {
      to_delete[tmp->RegionId()].push_back({start, with_start, tmp->Range().end_key(), false});
      next = tmp->Range().end_key();
    } else {
      CHECK_EQ(end, tmp->Range().end_key());
      to_delete[tmp->RegionId()].push_back({start, with_start, end, false});
      if (with_end) {
        delete_end_key = true;
      }
    }
  }

  CHECK_NE(next, end);

  {
    // process others
    while (!next.empty()) {
      CHECK_NE(next, end);
      CHECK(!delete_end_key);

      got = meta_cache->LookupRegionByKey(next, tmp);
      if (!got.IsOK()) {
        return got;
      }
      CHECK_NOTNULL(tmp.get());

      auto iter = region_id_to_region.find(tmp->RegionId());
      DCHECK(iter == region_id_to_region.end());
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));

      if (end < tmp->Range().end_key()) {
        to_delete[tmp->RegionId()].push_back({next, true, end, with_end});
        break;
      } else if (end > tmp->Range().end_key()) {
        to_delete[tmp->RegionId()].push_back({next, true, tmp->Range().end_key(), false});
        next = tmp->Range().end_key();
      } else {
        CHECK_EQ(end, tmp->Range().end_key());
        to_delete[tmp->RegionId()].push_back({next, true, end, false});
        if (with_end) {
          delete_end_key = true;
        }
        break;
      }
    }
  }

  DCHECK_EQ(region_id_to_region.size(), to_delete.size());

  std::vector<SubBatchState> sub_batch_state;
  std::vector<std::unique_ptr<KvDeleteRangeRpc>> rpcs;
  for (const auto& entry : to_delete) {
    auto region_id = entry.first;
    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvDeleteRangeRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());
    for (const DeleteRangeContext& delete_range : entry.second) {
      auto* range_with_option = rpc->MutableRequest()->mutable_range();

      auto* range = range_with_option->mutable_range();
      range->set_start_key(delete_range.start);
      range->set_end_key(delete_range.end);

      range_with_option->set_with_start(delete_range.with_start);
      range_with_option->set_with_end(delete_range.with_end);
    }

    sub_batch_state.emplace_back(rpc.get(), region);
    rpcs.emplace_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), to_delete.size());
  CHECK_EQ(rpcs.size(), sub_batch_state.size());

  std::vector<std::thread> thread_pool;
  thread_pool.reserve(sub_batch_state.size());
  for (auto& batch_state : sub_batch_state) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchDeleteRange, this, &batch_state);
  }

  int64_t tmp_delete_count = 0;
  Status result;

  // process end key
  if (delete_end_key) {
    Status delete_status = Delete(end);
    if (delete_status.IsOK()) {
      tmp_delete_count += 1;
    } else {
      result = delete_status;
    }
  }

  for (auto& thread : thread_pool) {
    thread.join();
  }

  for (auto& state : sub_batch_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    } else {
      tmp_delete_count += state.delete_count;
    }
  }

  delete_count = tmp_delete_count;

  return result;
}

Status RawKV::RawKVImpl::CompareAndSet(const std::string& key, const std::string& value,
                                       const std::string& expected_value, bool& state) {
  std::shared_ptr<MetaCache> meta_cache = stub_.GetMetaCache();

  std::shared_ptr<Region> region;
  Status ret = meta_cache->LookupRegionByKey(key, region);
  if (ret.IsOK()) {
    KvCompareAndSetRpc rpc;
    FillRpcContext(*rpc.MutableRequest()->mutable_context(), region->RegionId(), region->Epoch());
    auto* kv = rpc.MutableRequest()->mutable_kv();
    kv->set_key(key);
    kv->set_value(value);
    rpc.MutableRequest()->set_expect_value(expected_value);

    StoreRpcController controller(stub_, rpc, region);
    ret = controller.Call();
    if (ret.IsOK()) {
      state = rpc.Response()->key_state();
    }
  }

  return ret;
}

void RawKV::RawKVImpl::ProcessSubBatchCompareAndSet(SubBatchState* sub) {
  auto* rpc = CHECK_NOTNULL(dynamic_cast<KvBatchCompareAndSetRpc*>(sub->rpc));
  StoreRpcController controller(stub_, *sub->rpc, sub->region);
  Status call = controller.Call();
  if (call.IsOK()) {
    CHECK_EQ(rpc->Request()->kvs_size(), rpc->Response()->key_states_size());
    for (auto i = 0; i < rpc->Request()->kvs_size(); i++) {
      sub->key_op_states.push_back({rpc->Request()->kvs(i).key(), rpc->Response()->key_states(i)});
    }
  }
  sub->status = call;
}

Status RawKV::RawKVImpl::BatchCompareAndSet(const std::vector<KVPair>& kvs,
                                            const std::vector<std::string>& expected_values,
                                            std::vector<KeyOpState>& states) {
  if (kvs.size() != expected_values.size()) {
    return Status::InvalidArgument(
        fmt::format("kvs size:{} must equal expected_values size:{}", kvs.size(), expected_values.size()));
  }

  struct CompareAndSetContext {
    KVPair kv_pair;
    std::string expected_value;
  };

  auto meta_cache = stub_.GetMetaCache();
  std::unordered_map<int64_t, std::shared_ptr<Region>> region_id_to_region;
  std::unordered_map<int64_t, std::vector<CompareAndSetContext>> region_kvs;

  for (auto i = 0; i < kvs.size(); i++) {
    auto kv = kvs[i];
    auto key = kv.key;
    std::shared_ptr<Region> tmp;
    Status got = meta_cache->LookupRegionByKey(key, tmp);
    if (!got.IsOK()) {
      return got;
    }
    auto iter = region_id_to_region.find(tmp->RegionId());
    if (iter == region_id_to_region.end()) {
      region_id_to_region.emplace(std::make_pair(tmp->RegionId(), tmp));
    }

    const auto& expected_value = expected_values[i];
    region_kvs[tmp->RegionId()].push_back({kv, expected_value});
  }

  CHECK_EQ(region_id_to_region.size(), region_kvs.size());

  std::vector<SubBatchState> sub_batch_state;
  std::vector<std::unique_ptr<KvBatchCompareAndSetRpc>> rpcs;
  for (const auto& entry : region_kvs) {
    auto region_id = entry.first;
    auto iter = region_id_to_region.find(region_id);
    CHECK(iter != region_id_to_region.end());
    auto region = iter->second;

    auto rpc = std::make_unique<KvBatchCompareAndSetRpc>();
    FillRpcContext(*rpc->MutableRequest()->mutable_context(), region_id, region->Epoch());
    for (const CompareAndSetContext& context : entry.second) {
      auto* kv = rpc->MutableRequest()->add_kvs();
      kv->set_key(context.kv_pair.key);
      kv->set_value(context.kv_pair.value);
      *(rpc->MutableRequest()->add_expect_values()) = context.expected_value;
    }

    sub_batch_state.emplace_back(rpc.get(), region);
    rpcs.emplace_back(std::move(rpc));
  }

  CHECK_EQ(rpcs.size(), region_kvs.size());
  CHECK_EQ(rpcs.size(), sub_batch_state.size());

  std::vector<std::thread> thread_pool;
  for (auto i = 1; i < sub_batch_state.size(); i++) {
    thread_pool.emplace_back(&RawKV::RawKVImpl::ProcessSubBatchCompareAndSet, this, &sub_batch_state[i]);
  }

  ProcessSubBatchCompareAndSet(sub_batch_state.data());

  for (auto& thread : thread_pool) {
    thread.join();
  }

  Status result;
  std::vector<KeyOpState> tmp_states;
  for (auto& state : sub_batch_state) {
    if (!state.status.IsOK()) {
      DINGO_LOG(WARNING) << "rpc: " << state.rpc->Method() << " send to region: " << state.region->RegionId()
                         << " fail: " << state.status.ToString();
      if (result.IsOK()) {
        // only return first fail status
        result = state.status;
      }
    } else {
      tmp_states.insert(tmp_states.end(), std::make_move_iterator(state.key_op_states.begin()),
                        std::make_move_iterator(state.key_op_states.end()));
    }
  }

  states = std::move(tmp_states);

  return result;
}

}  // namespace sdk
}  // namespace dingodb