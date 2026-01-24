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
#include "db/dbformat.h"
#include "db/version_edit.h"

namespace ROCKSDB_NAMESPACE {

class ZoneExtent {  // part of a file in a specific zone
 public:
  uint64_t start_;
  uint64_t length_;
  Zone* zone_;
  int level_;

  explicit ZoneExtent(uint64_t start, uint64_t length, Zone* zone, int level);
  Status DecodeFrom(Slice* input);
  void EncodeTo(std::string* output);
  void EncodeJson(std::ostream& json_stream);
};

class ZoneFile;

/* Interface for persisting metadata for files */
class MetadataWriter {
 public:
  virtual ~MetadataWriter();
  virtual IOStatus Persist(ZoneFile* zoneFile) = 0;
};

class ZoneFile {
 private:
  const uint64_t NO_EXTENT = 0xffffffffffffffff;

  ZonedBlockDevice* zbd_;

  std::vector<ZoneExtent*> extents_;  // parts of a file can be spread across multiple zones
  std::vector<std::string> linkfiles_;

  // the zone the file will be put, as long as there is enough space
  // nullptr by default, allocated using AllocateIOZone when needed
  Zone* active_zone_;
  uint64_t extent_start_ = NO_EXTENT;
  uint64_t extent_filepos_ = 0;

  Env::WriteLifeTimeHint lifetime_;
  IOType io_type_; /* Only used when writing, for SST files value should be kData */
  uint64_t file_size_;
  uint64_t file_id_;

  uint32_t nr_synced_extents_ = 0;
  bool open_for_wr_ = false;
  std::mutex open_for_wr_mtx_;

  time_t m_time_;
  bool is_sparse_ = false;
  bool is_deleted_ = false;

  MetadataWriter* metadata_writer_ = NULL;

  std::mutex writer_mtx_;
  std::atomic<int> readers_{0};

 public:

  // the level, and the smallest and largest keys of the SST file
  int level_ = -1;
  InternalKey smallest_;
  InternalKey largest_;
  InternalKeyComparator icmp_;

  uint64_t num_entries_ = 0;
  uint64_t num_deletions_ = 0;
  uint64_t num_range_deletions_ = 0;
  uint64_t file_size_meta_ = 0;
  uint64_t compensated_file_size_ = 0;
  uint64_t compensated_range_deletion_size = 0;
  SequenceNumber smallest_seqno_ = kMaxSequenceNumber;
  SequenceNumber largest_seqno_ = 0;
  
  bool has_keys_ = false; // set to true on the first call to UpdateInternalKeys or UpdateInternalKeysRange
  bool is_recorded_ = false;

  uint64_t rank_ = 100000;

  static const int SPARSE_HEADER_SIZE = 8;

  explicit ZoneFile(ZonedBlockDevice* zbd, uint64_t file_id_,
                    MetadataWriter* metadata_writer);

  virtual ~ZoneFile();

  void AcquireWRLock();
  bool TryAcquireWRLock();
  void ReleaseWRLock();

  IOStatus CloseWR();
  bool IsOpenForWR();

  IOStatus PersistMetadata();

  IOStatus Append(void* buffer, int data_size);
  IOStatus BufferedAppend(char* data, uint32_t size);
  IOStatus SparseAppend(char* data, uint32_t size);
  IOStatus SetWriteLifeTimeHint(Env::WriteLifeTimeHint lifetime);
  
  void SetLevel(int level=-1);
  void UpdateInternalKeys(const Slice& key, SequenceNumber seqno);
  void UpdateInternalKeysRange(const InternalKey& start, const InternalKey& end, SequenceNumber seqno, const InternalKeyComparator& icmp);
  virtual void UpdateMetadata(const TableProperties& table_properties);
  void UpdateMetadata(const FileMetaData* meta, uint64_t average_value_size=0);
  void SetInternalComparator(const InternalKeyComparator& icmp);
  void ComputeCompensatedSize();
  
  void SetIOType(IOType io_type);
  std::string GetFilename();
  time_t GetFileModificationTime();
  void SetFileModificationTime(time_t mt);
  uint64_t GetFileSize();
  uint64_t GetFileSizeMeta();
  void SetFileSize(uint64_t sz);
  void ClearExtents();

  uint32_t GetBlockSize() { return zbd_->GetBlockSize(); }
  ZonedBlockDevice* GetZbd() { return zbd_; }
  std::vector<ZoneExtent*> GetExtents() { return extents_; }
  Env::WriteLifeTimeHint GetWriteLifeTimeHint() { return lifetime_; }

  IOStatus PositionedRead(uint64_t offset, size_t n, Slice* result,
                          char* scratch, bool direct);
  ZoneExtent* GetExtent(uint64_t file_offset, uint64_t* dev_offset);
  void PushExtent();
  IOStatus AllocateNewZone();

  void EncodeTo(std::string* output, uint32_t extent_start);
  void EncodeUpdateTo(std::string* output) {
    EncodeTo(output, nr_synced_extents_);
  };
  void EncodeSnapshotTo(std::string* output) { EncodeTo(output, 0); };
  void EncodeJson(std::ostream& json_stream);
  void MetadataSynced() { nr_synced_extents_ = extents_.size(); };
  void MetadataUnsynced() { nr_synced_extents_ = 0; };

  IOStatus MigrateData(uint64_t offset, uint32_t length, Zone* target_zone);

  Status DecodeFrom(Slice* input);
  Status MergeUpdate(std::shared_ptr<ZoneFile> update, bool replace);

  uint64_t GetID() { return file_id_; }

  bool IsSparse() { return is_sparse_; };

  void SetSparse(bool is_sparse) { is_sparse_ = is_sparse; };
  uint64_t HasActiveExtent() { return extent_start_ != NO_EXTENT; };
  uint64_t GetExtentStart() { return extent_start_; };

  IOStatus Recover();

  void ReplaceExtentList(std::vector<ZoneExtent*> new_list);
  void AddLinkName(const std::string& linkfile);
  IOStatus RemoveLinkName(const std::string& linkfile);
  IOStatus RenameLink(const std::string& src, const std::string& dest);
  uint32_t GetNrLinks() { return linkfiles_.size(); }
  const std::vector<std::string>& GetLinkFiles() const { return linkfiles_; }

  IOStatus InvalidateCache(uint64_t pos, uint64_t size);

 private:
  void ReleaseActiveZone();
  void SetActiveZone(Zone* zone);
  IOStatus CloseActiveZone();
  // Adds the record of this file to the container in ZonedBlockDevice
  // called in CloseWR()
  void CreateOrUpdateRecord();

 public:
  std::shared_ptr<ZenFSMetrics> GetZBDMetrics() { return zbd_->GetMetrics(); };
  IOType GetIOType() const { return io_type_; };
  bool IsDeleted() const { return is_deleted_; };
  void SetDeleted() {
    is_deleted_ = true;
    zbd_->RemoveZoneFileRecord(std::shared_ptr<ZoneFile>(this));
  };
  IOStatus RecoverSparseExtents(uint64_t start, uint64_t end, Zone* zone);

 public:
  class ReadLock {
   public:
    ReadLock(ZoneFile* zfile) : zfile_(zfile) {
      zfile_->writer_mtx_.lock();
      zfile_->readers_++;
      zfile_->writer_mtx_.unlock();
    }
    ~ReadLock() { zfile_->readers_--; }

   private:
    ZoneFile* zfile_;
  };
  class WriteLock {
   public:
    WriteLock(ZoneFile* zfile) : zfile_(zfile) {
      zfile_->writer_mtx_.lock();
      while (zfile_->readers_ > 0) {
      }
    }
    ~WriteLock() { zfile_->writer_mtx_.unlock(); }

   private:
    ZoneFile* zfile_;
  };
};

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
  virtual void UpdateInternalKeysRange(const InternalKey& start, const InternalKey& end, SequenceNumber seqno, const InternalKeyComparator& icmp) override;
  virtual void UpdateMetadata(const TableProperties& table_properties) override;
  virtual void UpdateMetadata(const FileMetaData* meta, uint64_t average_value_size=0) override;
  virtual void SetInternalComparator(const InternalKeyComparator& icmp) override;
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
