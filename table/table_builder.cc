// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }

  // 这部分数据被封装在 TableBuilder::Rep 中
  // LevelDB大量采用了这种手法进行数据提取, 避免暴露数据结构类型到外部
  Options options;                // 配置
  Options index_block_options;    // 配置
  WritableFile* file;             // 文件句柄
  uint64_t offset;                // 偏移
  Status status;                  // 状态
  BlockBuilder data_block;        // 写入 key-value 数据的 builder
  BlockBuilder index_block;       // 写入代表索引的 key-value 数据的 builder
  std::string last_key;           // 上一个 key
  int64_t num_entries;            // 多少个 key-value 数据
  bool closed;  // Either Finish() or Abandon() has been called. 完成或取消时调用
  FilterBlockBuilder* filter_block; // 过滤器 builder

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.
  bool pending_index_entry;    // 是否开启一个新的数据分块
  BlockHandle pending_handle;  // Handle to add to index block, 记录数据分块的偏移信息

  std::string compressed_output;  // 用于压缩数据的缓存
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  // 校验一下状态, 保证写入 key 有序
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }
  // 表示上一个数据block刚刷新到磁盘
  if (r->pending_index_entry) {
    assert(r->data_block.empty());
    // 找到最短距离最近的key存储下来，所以不一定是原始的key
    // 生成上一个块的索引数据 key 值
    // 即上一个块最后一个 key 和当前块的第一个 key 的公共前缀 +1
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    // 作为index部分
    r->pending_handle.EncodeTo(&handle_encoding);
    // 索引数据 value 值记录 offset 和 size
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  // 过滤器 builder 缓存完整的 key 值
  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(key);
  }

  // 记录一下写最后的 key 值
  r->last_key.assign(key.data(), key.size());
  // k-value 数据个数
  r->num_entries++;
  // 将 key-value 添加到数据分块中, 由 blockBuilder 做前缀压缩
  r->data_block.Add(key, value);

  // 估算一个数据分块长度, 如果大于设定值进行刷盘
  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  // 强判读状态, 是否有可写文件和数据
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  // 写一个 block 的数据
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    r->pending_index_entry = true;
    // 保证文件落盘
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    // 过滤器开启一个新数据分块
    r->filter_block->StartBlock(r->offset);
  }
}

// 写数据分块
void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  // 调用 blockbuilder 的 finish 准备好数据
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?
  // 选取压缩算法, 进行压缩
  switch (type) {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: {
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) &&
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else {
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  // 写入压缩后(如果可以压缩)数据
  WriteRawBlock(block_contents, type, handle);
  // 清空下缓存, 重新开始 data_block
  r->compressed_output.clear();
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  // handle 表示当前写入文件的 offset 和 size
  // 提供给索引区块设置数据时使用
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  // 文件追加写入数据
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    // 写入压缩类型和检验数据
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      // 记录 offset 为偏移加大小
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const { return rep_->status; }

// key-value 写入完毕, 写入其他数据
Status TableBuilder::Finish() {
  Rep* r = rep_;
  // 刷入最后一块 data_block
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  // 写入非压缩的过滤器数据
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // 写入元数据
  // 元数据也是 key-value 数据
  // 记录 key 为过滤器名字
  // 记录 value 为过滤器数据块的偏移和大小
  // Write metaindex block
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != nullptr) {
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    // 写入数据
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // Write index block
  // 写入索引分块数据
  if (ok()) {
    // 生成最后一块数据区的索引信息
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  // 写入 footer 数据
  // 即元数据分块的偏移和大小
  // 索引数据分块的偏移和大小
  // 用元数据索引过滤器数据分块
  // 用索引数据索引数据分块
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace leveldb
