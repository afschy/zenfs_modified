// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
// #if !defined(ROCKSDB_LITE) && defined(OS_LINUX)

#include <errno.h>
#include <libzbd/zbd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "metrics.h"
#include "param_loader.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"
#include "db/dbformat.h"

namespace ROCKSDB_NAMESPACE {

// the zone allocator must open a new zone, finishing existing zones if needed
#define LIFETIME_DIFF_NOT_GOOD (100)
// the zone allocator should open a new zone if it can be done without finishing an existing zone
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

class ZonedBlockDevice;
class ZonedBlockDeviceBackend;
class ZoneSnapshot;
class ZenFSSnapshotOptions;
class ZoneExtent;
class ZoneFile;

class ZoneList {
 private:
  void *data_;
  unsigned int zone_count_;

 public:
  ZoneList(void *data, unsigned int zone_count)
      : data_(data), zone_count_(zone_count){};
  void *GetData() { return data_; };
  unsigned int ZoneCount() { return zone_count_; };
  ~ZoneList() { free(data_); };
};

class Zone {
  ZonedBlockDevice *zbd_;
  ZonedBlockDeviceBackend *zbd_be_;
  std::atomic_bool busy_;

 public:
  explicit Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
                std::unique_ptr<ZoneList> &zones, unsigned int idx);

  uint64_t start_;
  uint64_t capacity_; /* remaining capacity */
  uint64_t max_capacity_;
  uint64_t wp_; // logical address of where the next data will be put
  Env::WriteLifeTimeHint lifetime_;
  std::atomic<uint64_t> used_capacity_;
  uint32_t reset_count_;  // incremented on each call to Reset()
  uint32_t finish_count_; // incremented on each call to Finish()
  MappingPolicyType policy_;  // the policy for the ZoneFile object that wrote to this zone first
  int level_; // the level of the ZoneFile object that wrote to this zone first
  bool flag_gc_reset_ = false;

  IOStatus Reset();
  IOStatus Finish();
  IOStatus Close();

  IOStatus Append(char *data, uint32_t size);
  bool IsUsed();
  bool IsFull();
  bool IsEmpty();
  uint64_t GetZoneNr();
  uint64_t GetCapacityLeft();
  bool IsBusy() { return this->busy_.load(std::memory_order_relaxed); }
  bool Acquire() {
    bool expected = false;
    return this->busy_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel);
  }
  bool Release() {
    bool expected = true;
    return this->busy_.compare_exchange_strong(expected, false,
                                               std::memory_order_acq_rel);
  }

  void EncodeJson(std::ostream &json_stream);

  inline IOStatus CheckRelease() {
    if (!Release()) {
      assert(false);
      return IOStatus::Corruption("Failed to unset busy flag of zone " +
                                  std::to_string(GetZoneNr()));
    }

    return IOStatus::OK();
  }
};

// Calls the underlying ZNS library to perform zone operations
class ZonedBlockDeviceBackend {
 public:
  uint32_t block_sz_ = 0;
  uint64_t zone_sz_ = 0;
  uint32_t nr_zones_ = 0;

 public:
  virtual IOStatus Open(bool readonly, bool exclusive,
                        unsigned int *max_active_zones,
                        unsigned int *max_open_zones) = 0;

  virtual std::unique_ptr<ZoneList> ListZones() = 0;
  virtual IOStatus Reset(uint64_t start, bool *offline,
                         uint64_t *max_capacity) = 0;
  virtual IOStatus Finish(uint64_t start) = 0;
  virtual IOStatus Close(uint64_t start) = 0;
  virtual int Read(char *buf, int size, uint64_t pos, bool direct) = 0;
  virtual int Write(char *data, uint32_t size, uint64_t pos) = 0;
  virtual int InvalidateCache(uint64_t pos, uint64_t size) = 0;
  virtual bool ZoneIsSwr(std::unique_ptr<ZoneList> &zones,
                         unsigned int idx) = 0;
  virtual bool ZoneIsOffline(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual bool ZoneIsWritable(std::unique_ptr<ZoneList> &zones,
                              unsigned int idx) = 0;
  virtual bool ZoneIsActive(std::unique_ptr<ZoneList> &zones,
                            unsigned int idx) = 0;
  virtual bool ZoneIsOpen(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual uint64_t ZoneStart(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual uint64_t ZoneMaxCapacity(std::unique_ptr<ZoneList> &zones,
                                   unsigned int idx) = 0;
  virtual uint64_t ZoneWp(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual std::string GetFilename() = 0;
  uint32_t GetBlockSize() { return block_sz_; };
  uint64_t GetZoneSize() { return zone_sz_; };
  uint32_t GetNrZones() { return nr_zones_; };
  virtual ~ZonedBlockDeviceBackend(){};
};

enum class ZbdBackendType {
  kBlockDev,
  kZoneFS,
};

class ZonedBlockDevice {
 private:
  std::unique_ptr<ZonedBlockDeviceBackend> zbd_be_;
  std::vector<Zone *> io_zones;   // zones that are allowed to store SST/WAL files
  std::vector<Zone *> meta_zones; // zones set aside for storing metadata
  time_t start_time_;
  std::shared_ptr<Logger> logger_;
  uint32_t finish_threshold_ = 0;

 public:
  std::unordered_map<uint64_t, Zone*> zone_start_to_pointer_map_;
  std::unordered_map<uint64_t, Zone*> zone_index_to_pointer_map_;
  
 private:
  // bookkeeping
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> gc_bytes_written_{0};
  std::atomic<uint64_t> finish_bytes_written_{0};
  uint64_t alloc_count_ = 0;    // how many times GetBestOpenZoneMatch() is called
  uint64_t failure_count_ = 0;  // how many times the default policy is invoked by GetBestOpenZoneMatch()
  uint64_t total_reset_count_ = 0;
  uint64_t gc_reset_count_ = 0;

  std::atomic<long> active_io_zones_;
  std::atomic<long> open_io_zones_;
  /* Protects zone_resuorces_  condition variable, used
     for notifying changes in open_io_zones_ */
  std::mutex zone_resources_mtx_;
  std::condition_variable zone_resources_;
  std::mutex zone_deferred_status_mutex_;
  IOStatus zone_deferred_status_;

  std::condition_variable migrate_resource_;
  std::mutex migrate_zone_mtx_;
  std::atomic<bool> migrating_{false};

  unsigned int max_nr_active_io_zones_;
  unsigned int max_nr_open_io_zones_;

  std::shared_ptr<ZenFSMetrics> metrics_;

  // one list for each level
  // the list contains pointers to each known file in the level
  // pointers are sorted on the InternalKey string at ZoneFile::smallest_
  std::vector< std::list< ZoneFile* > > levelwise_file_list_;

  void EncodeJsonZone(std::ostream &json_stream,
                      const std::vector<Zone *> zones);

 public:
  // Stores all new parameters for the platform, loaded from param_loader.h
  ZenfsParamContainer zenfs_parameters_;
  FILE* logfile_;  // for minimal logs, mostly for the garbage collector
  FILE* zonestate_logfile_; // for printing detailed zone states for all gc iterations

  // mutex to be acquired when accessing/modifying levelwise_file_list_
  std::mutex levelwise_files_mtx_;
  std::mutex alloc_mutex_;

  std::map<std::string, std::shared_ptr<ZoneFile>> *files_ = nullptr;
  std::mutex *files_mtx_ = nullptr;

  explicit ZonedBlockDevice(std::string path, ZbdBackendType backend,
                            std::shared_ptr<Logger> logger,
                            std::shared_ptr<ZenFSMetrics> metrics =
                                std::make_shared<NoZenFSMetrics>());
  virtual ~ZonedBlockDevice();

  IOStatus Open(bool readonly, bool exclusive);

  Zone *GetIOZone(uint64_t offset);

  // Hands over a zone to the ZoneFile for writing data
  // Tries to hand over an already open zone (File Placement Policy)
  // If that's not possible/desirable, opens a new zone (New Zone Allocation Policy)
  IOStatus AllocateIOZone(Env::WriteLifeTimeHint file_lifetime, IOType io_type,
                          Zone **out_zone, ZoneFile* zonefile);
  IOStatus AllocateMetaZone(Zone **out_meta_zone);

  uint64_t GetFreeSpace();
  uint64_t GetUsedSpace();
  uint64_t GetReclaimableSpace();

  std::string GetFilename();
  uint32_t GetBlockSize();

  IOStatus ResetUnusedIOZones(bool gc=false);
  void LogZoneStats();
  void LogZoneUsage();
  void LogGarbageInfo();
  void LogDetailedZoneState();

  uint64_t GetZoneSize();
  uint32_t GetNrZones();
  std::vector<Zone *> GetMetaZones() { return meta_zones; }

  void SetFinishTreshold(uint32_t threshold) { finish_threshold_ = threshold; }

  void PutOpenIOZoneToken();
  void PutActiveIOZoneToken();

  void EncodeJson(std::ostream &json_stream);

  void SetZoneDeferredStatus(IOStatus status);

  std::shared_ptr<ZenFSMetrics> GetMetrics() { return metrics_; }

  void GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot);

  int Read(char *buf, uint64_t offset, int n, bool direct);
  IOStatus InvalidateCache(uint64_t pos, uint64_t size);

  IOStatus ReleaseMigrateZone(Zone *zone, bool alloc_new=false);

  IOStatus TakeMigrateZone(Zone **out_zone, ZoneFile* zonefile,
                           Env::WriteLifeTimeHint lifetime, uint32_t min_capacity, bool* alloc_new);

  void AddBytesWritten(uint64_t written) { bytes_written_ += written; };
  void AddGCBytesWritten(uint64_t written) { gc_bytes_written_ += written; };
  void AddFinishBytesWritten(uint64_t written) { finish_bytes_written_ += written; };
  uint64_t GetUserBytesWritten() {
    return bytes_written_.load() - gc_bytes_written_.load();
  };
  uint64_t GetTotalBytesWritten() { return bytes_written_.load(); };
  uint64_t GetGCBytesWritten() { return gc_bytes_written_.load(); };
  uint64_t GetFinishBytesWritten() { return finish_bytes_written_.load(); };

  // Adds a new file to the appropriate level and position of levelwise_file_list_
  // Called after the whole file is written
  void AddZoneFileRecord(ZoneFile* zonefile_ptr);
  void AddZoneFileRecordNoLock(ZoneFile* zonefile_ptr);
  // Searches for and removes a particular file from levelwise_file_list_
  // Called when a file is marked as deleted
  void RemoveZoneFileRecord(ZoneFile* zonefile_ptr);
  void RemoveZoneFileRecordNoLock(ZoneFile* zonefile_ptr);
  // Re-computes the compensated file sizes for all recorded files
  void RecomputeCompensatedFileSizes();

  uint64_t GetEmptyZoneCount();
  uint64_t GetAllocCount() { return alloc_count_; }
  uint64_t GetFailureCount() { return failure_count_; }
  uint64_t GetTotalResetCount() { return total_reset_count_; }
  uint64_t GetGCResetCount() { return gc_reset_count_; }
  uint64_t GetFreePercent();

 private:
  IOStatus GetZoneDeferredStatus();
  bool GetActiveIOZoneTokenIfAvailable();
  void WaitForOpenIOZoneToken(bool prioritized);
  IOStatus ApplyFinishThreshold();
  IOStatus FinishCheapestIOZone();
  
  // Related to choosing the best existing zone for a ZoneFile object
  // Tries to find the zone that best fits the given zonefile's policy
  // Goes to the fallback policy if primary policy fails
  IOStatus GetBestOpenZoneMatch(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);

  // Implementations of all File Placement policies
  // ZenFS default, tries to put the file in a zone with equal or higher lifetime
  // Levels 0 and 1 have WLTH_MEDIUM, level-2 has WLTH_LONG, levels 3 and onward have WLTH_EXTREME
  // RocksDB assigns those before writing
  IOStatus MatchLifetimeBased(ZoneFile* zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  // Looks for files with overlapping keyranges in the upper and lower level
  // Chooses the zone that holds the most amount of data from the overlapping files
  IOStatus MatchCAZA(ZoneFile* zonefile,
                      Env::WriteLifeTimeHint file_lifetime,
                      unsigned int *best_diff_out, Zone **zone_out,
                      uint32_t min_capacity = 0);
  // Chooses the zone that holds the most data from adjacent files
  // Closer files have higher weight compared to farther files
  IOStatus MatchSameLevelNearbyKeys(ZoneFile* zonefile,
                                    Env::WriteLifeTimeHint file_lifetime,
                                    unsigned int *best_diff_out, Zone **zone_out,
                                    uint32_t min_capacity = 0);
  IOStatus MatchSameLevelNearbyKeysSimple(ZoneFile* zonefile,
                                          Env::WriteLifeTimeHint file_lifetime,
                                          unsigned int *best_diff_out, Zone **zone_out,
                                          uint32_t min_capacity = 0);
  // Fills a zone in order of arrival (per-level)
  IOStatus MatchArrivalTimeBased(ZoneFile* zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  // Puts files that exceed the given tombstone ratio (tunable parameter) into a special zone
  IOStatus MatchTombstoneDensity(ZoneFile* zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  // Placeholder, forwards to MatchLifetimeBased
  IOStatus MatchTombstoneTTL(ZoneFile* zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  // Puts all files with the kClusterTogether policy in the same zone, doesn't consider any other factor
  IOStatus MatchClusterTogether(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  // Creates a ranking of all files in a level based on how much a file overlaps with level+1
  // Tries to put the new file in the same zone with similar-ranked files in its level
  // Closer files (rankwise) have higher weight than farther files
  IOStatus MatchOverlapChildren(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  // Same as MatchOverlapChildren, but for level+2
  IOStatus MatchOverlapGrandchildren(ZoneFile* zonefile,
                                      Env::WriteLifeTimeHint file_lifetime,
                                      unsigned int *best_diff_out, Zone **zone_out,
                                      uint32_t min_capacity = 0);
  // Creates a ranking of all files in a level based on compensated file size
  // Tries to put the new file in the same zone with similar-ranked files in its level
  // Closer files (rankwise) have higher weight than farther files
  IOStatus MatchCompensatedSize(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  
  IOStatus MatchOAZA(ZoneFile* zonefile,
                      Env::WriteLifeTimeHint file_lifetime,
                      unsigned int *best_diff_out, Zone **zone_out,
                      uint32_t min_capacity = 0);

  IOStatus ChooseZoneWithClosestFile(ZoneFile* zonefile,
                                     uint64_t zonefile_index,
                                     std::vector<ZoneFile*> &own_level_files,
                                     Zone* &allocated_zone,
                                     uint32_t min_capacity = 0);

  IOStatus ChooseZoneWithClosestRank(ZoneFile* zonefile,
                                     uint64_t zonefile_index,
                                     std::vector<ZoneFile*> &own_level_ranking,
                                     Zone* &allocated_zone,
                                     uint32_t min_capacity = 0);

  IOStatus ChooseZoneWithHighestContrib(Zone* &allocated_zone,
                                        uint64_t &highest_contribution,
                                        std::map<uint64_t, uint64_t> &contribution_map,
                                        uint32_t min_capacity = 0);
  
  // Related to the allocation of a new empty zone when needed
  // Only relevant when static zone-to-block mapping is used in the underlying ZNS SSD
  IOStatus AllocateEmptyZone(Zone **zone_out, int level=-1);
  // Hands over the first empty zone
  IOStatus AllocateEmptyZoneDefault(Zone **zone_out);
  // Round-robin allocation
  IOStatus AllocateEmptyZoneSequential(Zone **zone_out);
  IOStatus AllocateEmptyZoneRandom(Zone **zone_out);
  // Greedily chooses the zone with the least reset count
  IOStatus AllocateEmptyZoneLeastWear(Zone **zone_out);
  // Puts lower-level files on high-wear zones, since those files get deleted more infrequently
  // Puts upper-level files on low-wear zones for the opposite reason
  IOStatus AllocateEmptyZoneHotnessBased(Zone **zone_out, int level);

  // levelwise_files_mtx_ must be held before calling
  // Returns a vector of pointers to files that overlap with zonefile_ptr in a target_level
  void GetOverlappingFiles(std::vector<ZoneFile*>& ret_container, ZoneFile* zonefile_ptr, int target_level);
  // Returns a vector of pointers to all files in target_level
  void GetSameLevelFiles(std::vector<ZoneFile*>& ret_container, int target_level);
  // Returns the total number of files that overlap with zonefile_ptr in target_level
  uint64_t GetOverlapFileCount(ZoneFile* zonefile_ptr, int target_level);
  // Returns the total number of bytes across files that overlap with zonefile_ptr in target_level
  uint64_t GetOverlapByteCount(ZoneFile* zonefile_ptr, int target_level);
  // Returns the total number of keys across files that overlap with zonefile_ptr in target_level
  uint64_t GetOverlapKeyCount(ZoneFile* zonefile_ptr, int target_level);
  // container (output) is a list of ZoneFile pointers, sorted by its compaction rank
  void OverlapRankHelperSizeBased(std::vector<ZoneFile*>& container, int own_level, int target_level);
  // container (output) is a list of ZoneFile pointers, sorted by its compaction rank
  void OverlapRankHelperKeyBased(std::vector<ZoneFile*>& container, int own_level, int target_level);
  // Returns true if the given files overlap
  bool IsOverlapping(ZoneFile* a, ZoneFile* b, InternalKeyComparator& icmp);
  // Returns true if the ratio of tombstones in the given file is higher than the threshold
  bool IsHighTombstone(ZoneFile* zonefile_ptr);

  // result (output) contains the amount of data each zone holds from the given file(s)
  // the map's key is a zone's first address, and the value is how many bytes of data from the file(s) that zone holds
  // factor is multiplied with the byte count for the file(s), used for giving different weights to different files
  void GetPerZoneContribution(std::vector<ZoneFile*>& files, std::map<uint64_t, uint64_t>& result, double factor=1.0);
  void GetPerZoneContribution(ZoneFile* zonefile, std::map<uint64_t, uint64_t>& result, double factor=1.0);

  MappingPolicyType GetPolicy(int level) {
    if(level < 0) return kLifetimeBased;
    if(level <= zenfs_parameters_.min_boundary) return zenfs_parameters_.upper_level_policy;
    if(level >= zenfs_parameters_.max_boundary) return zenfs_parameters_.lower_level_policy;
    return zenfs_parameters_.middle_level_policy;
  }

  MappingPolicyType GetSecondaryPolicy(int level) {
    if(level < 0) return kLifetimeBased;
    if(level <= zenfs_parameters_.min_boundary) return zenfs_parameters_.upper_level_policy_fallback;
    if(level >= zenfs_parameters_.max_boundary) return zenfs_parameters_.lower_level_policy_fallback;
    return zenfs_parameters_.middle_level_policy_fallback;
  }

  // Used by MatchLifetimeBased
  static unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                                      Env::WriteLifeTimeHint file_lifetime) {
    assert(file_lifetime <= Env::WLTH_EXTREME);

    if ((file_lifetime == Env::WLTH_NOT_SET) ||
        (file_lifetime == Env::WLTH_NONE)) {
      if (file_lifetime == zone_lifetime) {
        return 0;
      } else {
        return LIFETIME_DIFF_NOT_GOOD;
      }
    }

    if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
    if (zone_lifetime == file_lifetime) return LIFETIME_DIFF_COULD_BE_WORSE;

    return LIFETIME_DIFF_NOT_GOOD;
  }
};

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
  uint64_t compensated_range_deletion_size_ = 0;
  SequenceNumber smallest_seqno_ = kMaxSequenceNumber;
  SequenceNumber largest_seqno_ = 0;
  
  bool has_keys_ = false; // set to true on the first call to UpdateInternalKeys or UpdateInternalKeysRange
  bool is_recorded_ = false;
  uint64_t key_update_count_ = 0;
  uint64_t key_update_post_alloc_count_ = 0;

  uint64_t rank_ = 100000;
  uint64_t alloc_size_ = 0; // used for non-fragmented zone allocation

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
  void UpdateInternalKeysRange(const Slice& start, const Slice& end, SequenceNumber seqno, const CompareInterface* icmp);
  virtual void UpdateMetadata(const TableProperties& table_properties);
  void UpdateMetadata(uint64_t num_entries, uint64_t num_deletions, uint64_t num_range_deletions, uint64_t file_size, uint64_t compensated_range_deletion_size, uint64_t average_value_size=0);
  void SetInternalComparator(CompareInterface* icmp);
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
    zbd_->RemoveZoneFileRecord(this);
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

}  // namespace ROCKSDB_NAMESPACE

// #endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)
