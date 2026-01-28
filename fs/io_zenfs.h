// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

// #if !defined(ROCKSDB_LITE) && defined(OS_LINUX)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <semaphore>

#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"
#include "zbd_zenfs.h"

namespace ROCKSDB_NAMESPACE {

// FSWritableFile is the file wrapper used by RocksDB for writing to files
// Every FileSystem implementation must extend this class with its own write implementation
class ZonedWritableFile : public FSWritableFile {
 public:
  explicit ZonedWritableFile(ZonedBlockDevice* zbd, bool buffered,
                             std::shared_ptr<ZoneFile> zoneFile);
  virtual ~ZonedWritableFile();

  virtual IOStatus Append(const Slice& data, const IOOptions& options,
                          IODebugContext* dbg) override;
  virtual IOStatus Append(const Slice& data, const IOOptions& opts,
                          const DataVerificationInfo& /* verification_info */,
                          IODebugContext* dbg) override {
    return Append(data, opts, dbg);
  }
  virtual IOStatus PositionedAppend(const Slice& data, uint64_t offset,
                                    const IOOptions& options,
                                    IODebugContext* dbg) override;
  virtual IOStatus PositionedAppend(
      const Slice& data, uint64_t offset, const IOOptions& opts,
      const DataVerificationInfo& /* verification_info */,
      IODebugContext* dbg) override {
    return PositionedAppend(data, offset, opts, dbg);
  }
  virtual IOStatus Truncate(uint64_t size, const IOOptions& options,
                            IODebugContext* dbg) override;
  virtual IOStatus Close(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Flush(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override;
  virtual IOStatus RangeSync(uint64_t offset, uint64_t nbytes,
                             const IOOptions& options,
                             IODebugContext* dbg) override;
  virtual IOStatus Fsync(const IOOptions& options,
                         IODebugContext* dbg) override;

  virtual uint64_t GetFileSize(const IOOptions& /*options*/,
                               IODebugContext* /*dbg*/) override {
    return zoneFile_->GetFileSize();
  }

  bool use_direct_io() const override { return !buffered; }
  bool IsSyncThreadSafe() const override { return true; };
  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }
  void SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) override;
  void SetLevel(int level=-1) override;
  virtual void UpdateInternalKeys(const Slice& key, SequenceNumber seqno) override;
  virtual void UpdateInternalKeysRange(const Slice& start, const Slice& end, SequenceNumber seqno, const CompareInterface* icmp) override;
  virtual void UpdateMetadata(const TableProperties& table_properties) override;
  virtual void UpdateMetadata(uint64_t num_entries, uint64_t num_deletions, uint64_t num_range_deletions, uint64_t file_size, uint64_t compensated_range_deletion_size, uint64_t average_value_size=0) override;
  virtual void SetInternalComparator(CompareInterface* icmp) override;
  virtual Env::WriteLifeTimeHint GetWriteLifeTimeHint() override {
    return zoneFile_->GetWriteLifeTimeHint();
  }

 private:
  IOStatus BufferedWrite(const Slice& data);
  IOStatus FlushBuffer();
  IOStatus DataSync();
  IOStatus CloseInternal();

  bool buffered;
  char* sparse_buffer;
  char* buffer;
  // the size of each buffer, should be enough to hold and entire file
  // default is 64+1 MB for 64 MB SST files, can be tuned using params.txt for smaller/larget SST files
  uint32_t buffer_size_megabytes;
  size_t buffer_sz;
  uint32_t block_sz;
  uint32_t buffer_pos;
  uint64_t wp;
  int write_temp;
  bool open;

  std::shared_ptr<ZoneFile> zoneFile_;
  MetadataWriter* metadata_writer_;

  std::mutex buffer_mtx_; // protects the buffer during memory allocation

  // maximum number of buffers that can be allocated for writing files
  // used for limiting memory usage
  // tunable parameter from params.txt
  static inline uint32_t max_buffer_count = INT32_MAX;
  // incremented when buffer is allocated in the constructor
  // decremented when buffer is de-allocated in the destructor
  static inline uint32_t curr_buffer_count = 0;

  // if it isn't possible to allocate a buffer because curr_buffer_count == max_buffer_count,
  // wait for one of the buffers from other files to be de-allocated, which will decrease curr_buffer_count
  // the condition_variable is used for waiting, along with buffer_count_mtx_
  static inline std::mutex buffer_count_mtx_;
  static inline std::condition_variable buffer_count_condvar_;
};

class ZonedSequentialFile : public FSSequentialFile {
 private:
  std::shared_ptr<ZoneFile> zoneFile_;
  uint64_t rp;
  bool direct_;

 public:
  explicit ZonedSequentialFile(std::shared_ptr<ZoneFile> zoneFile,
                               const FileOptions& file_opts)
      : zoneFile_(zoneFile),
        rp(0),
        direct_(file_opts.use_direct_reads && !zoneFile->IsSparse()) {}

  IOStatus Read(size_t n, const IOOptions& options, Slice* result,
                char* scratch, IODebugContext* dbg) override;
  IOStatus PositionedRead(uint64_t offset, size_t n, const IOOptions& options,
                          Slice* result, char* scratch,
                          IODebugContext* dbg) override;
  IOStatus Skip(uint64_t n) override;

  bool use_direct_io() const override { return direct_; };

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t offset, size_t length) override {
    return zoneFile_->InvalidateCache(offset, length);
  }
};

class ZonedRandomAccessFile : public FSRandomAccessFile {
 private:
  std::shared_ptr<ZoneFile> zoneFile_;
  bool direct_;

 public:
  explicit ZonedRandomAccessFile(std::shared_ptr<ZoneFile> zoneFile,
                                 const FileOptions& file_opts)
      : zoneFile_(zoneFile),
        direct_(file_opts.use_direct_reads && !zoneFile->IsSparse()) {}

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override;

  IOStatus Prefetch(uint64_t /*offset*/, size_t /*n*/,
                    const IOOptions& /*options*/,
                    IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  bool use_direct_io() const override { return direct_; }

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t offset, size_t length) override {
    return zoneFile_->InvalidateCache(offset, length);
  }
};

}  // namespace ROCKSDB_NAMESPACE

// #endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)
