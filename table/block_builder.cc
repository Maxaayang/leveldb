// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// BlockBuilder generates blocks where keys are prefix-compressed:
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
//
// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/options.h"
#include "util/coding.h"

namespace leveldb {

// 第一个重启点为0
BlockBuilder::BlockBuilder(const Options* options)
    : options_(options), restarts_(), counter_(0), finished_(false) {
  assert(options->block_restart_interval >= 1);
  restarts_.push_back(0);  // First restart point is at offset 0
}

// 写入数据后, 调用该接口清空, 开启新的数据分块
// 避免重新 new 一个新的 BlockBuilder 以此节省内存
void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // First restart point is at offset 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

// 估算写入一个数据分块占用多大的磁盘空间
// 写入 key-value 数据大小 + 重启点数组大小 + 重启点长度记录值大小
size_t BlockBuilder::CurrentSizeEstimate() const {
  return (buffer_.size() +                       // Raw data buffer
          restarts_.size() * sizeof(uint32_t) +  // Restart array
          sizeof(uint32_t));                     // Restart array length
}

// 将当前BlockBuilder中的数据形成下盘格式
// buffer_中已经是 key-value 数据了, 只需要将重启点数组和长度写入即可
Slice BlockBuilder::Finish() {
  // Append restart array
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  PutFixed32(&buffer_, restarts_.size());
  finished_ = true;
  return Slice(buffer_);
}

// 数据写入
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  //表示上一个key
  Slice last_key_piece(last_key_);
  assert(!finished_);
  //block_restart_interval控制着重启点之间的距离
  assert(counter_ <= options_->block_restart_interval);
  //这里验证一下，首先有数据，其次当前的key肯定要比上一次的key要大，因为他是有序的 
  assert(buffer_.empty()  // No values yet?
         || options_->comparator->Compare(key, last_key_piece) > 0);
  // 共同前缀长度, 默认为0, 和上一个key的共同前缀长度
  size_t shared = 0;
  //block_restart_interval控制着重启点之间的距离
  // 这里不需要重启, 即前缀可压缩, 查找共同的前缀长度
  if (counter_ < options_->block_restart_interval) {
    // See how much sharing to do with previous string
    //计算和前一个key共享的部分
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression
    // 需要重启, 记录下一个重启点
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  // 非共同长度
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_
  // 写入数据共同前缀长度 / 非共同前缀长度 / value 的大小
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Add string delta to buffer_ followed by value
  // 写入key非共同前缀部分 / 写入value数据
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  // 更新下最后一个key
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}

}  // namespace leveldb
