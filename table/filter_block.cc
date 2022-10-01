// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
// 每 2KB 一个索引点
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

// 根据block_offset创建filter, filter的数量只和 block_offset 有关
// 默认每 2KB offset 就创建一个 filter
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  //2kb一个filter
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

// 缓存每个 key 数据到 keys_ 中
// 每个 key 长度到 start_
void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

// 完成 filter block 的构建
Slice FilterBlockBuilder::Finish() {
  // 先为剩余的key创建一个新的filter
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  // 记录每一个 offset / 2KB = i 的索引对应的过滤器数据位置
  // 将filter offset列表按照顺序编码到 result_中
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  // 记录过滤器数据长度
  // offset列表的offset编码到result_的最后面
  PutFixed32(&result_, array_offset);
  // 记录每多少个数据分块生成一个过滤器数据
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

// 生成过滤器数据
void FilterBlockBuilder::GenerateFilter() {
  //key个数
  const size_t num_keys = start_.size();
  // 如果 key 的数量为0, 把下一个 filter 的 offset 放入 filter_offsets_ 中, 即让 filter offset 指向下一个 filter
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  // start_ 数组存放所有 key append 而成的字符串的起始位置
  // 方便计算, 记录左右一个 key 的位置
  // key0key1key2key3
  // start[i + 1] - start[i] 即为 key 的长度
  // tem_keys_ 将所有 key 提取为 vector
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  // 记录 filter 数据的偏移
  // 每个 filter 数据对应一个数据分块(如果数据分块 < 2KB, 可能该数据过滤器数据对应多个数据分块)
  // filter0 | filter1 | xxxxx
  filter_offsets_.push_back(result_.size());
  // 根据 key 数据生成过滤器数据并 append 到 result_ 之后
  // 默认过滤器使用 BloomFilterPolicy, bloom.cc
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n - 1];
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
