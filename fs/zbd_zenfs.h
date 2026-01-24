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

namespace ROCKSDB_NAMESPACE {

class ZonedBlockDevice;
class ZonedBlockDeviceBackend;
class ZoneSnapshot;
class ZenFSSnapshotOptions;

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

  inline IOStatus CheckRelease();
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

  // bookkeeping
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> gc_bytes_written_{0};

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
  std::vector< std::list< std::shared_ptr<ZoneFile> > > levelwise_file_list_;

  void EncodeJsonZone(std::ostream &json_stream,
                      const std::vector<Zone *> zones);

 public:
  // Stores all new parameters for the platform, loaded from param_loader.h
  ZenfsParamContainer zenfs_parameters_;

  // mutex to be acquired when accessing/modifying levelwise_file_list_
  std::mutex levelwise_files_mtx_;

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
                          Zone **out_zone, std::shared_ptr<ZoneFile> zonefile);
  IOStatus AllocateMetaZone(Zone **out_meta_zone);

  uint64_t GetFreeSpace();
  uint64_t GetUsedSpace();
  uint64_t GetReclaimableSpace();

  std::string GetFilename();
  uint32_t GetBlockSize();

  IOStatus ResetUnusedIOZones();
  void LogZoneStats();
  void LogZoneUsage();
  void LogGarbageInfo();

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

  IOStatus ReleaseMigrateZone(Zone *zone);

  IOStatus TakeMigrateZone(Zone **out_zone, std::shared_ptr<ZoneFile> zonefile,
                           Env::WriteLifeTimeHint lifetime, uint32_t min_capacity);

  void AddBytesWritten(uint64_t written) { bytes_written_ += written; };
  void AddGCBytesWritten(uint64_t written) { gc_bytes_written_ += written; };
  uint64_t GetUserBytesWritten() {
    return bytes_written_.load() - gc_bytes_written_.load();
  };
  uint64_t GetTotalBytesWritten() { return bytes_written_.load(); };

  // Adds a new file to the appropriate level and position of levelwise_file_list_
  // Called after the whole file is written
  void AddZoneFileRecord(std::shared_ptr<ZoneFile> zonefile_ptr);
  // Searches for and removes a particular file from levelwise_file_list_
  // Called when a file is marked as deleted
  void RemoveZoneFileRecord(std::shared_ptr<ZoneFile> zonefile_ptr);
  // Re-computes the compensated file sizes for all recorded files
  void RecomputeCompensatedFileSizes();

 private:
  IOStatus GetZoneDeferredStatus();
  bool GetActiveIOZoneTokenIfAvailable();
  void WaitForOpenIOZoneToken(bool prioritized);
  IOStatus ApplyFinishThreshold();
  IOStatus FinishCheapestIOZone();
  
  // Related to choosing the best existing zone for a ZoneFile object
  // Tries to find the zone that best fits the given zonefile's policy
  // Goes to the fallback policy if primary policy fails
  IOStatus GetBestOpenZoneMatch(std::shared_ptr<ZoneFile> zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);

  // Implementations of all File Placement policies
  // ZenFS default, tries to put the file in a zone with equal or higher lifetime
  // Levels 0 and 1 have WLTH_MEDIUM, level-2 has WLTH_LONG, levels 3 and onward have WLTH_EXTREME
  // RocksDB assigns those before writing
  IOStatus MatchLifetimeBased(std::shared_ptr<ZoneFile> zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  // Looks for files with overlapping keyranges in the upper and lower level
  // Chooses the zone that holds the most amount of data from the overlapping files
  IOStatus MatchCAZA(std::shared_ptr<ZoneFile> zonefile,
                      Env::WriteLifeTimeHint file_lifetime,
                      unsigned int *best_diff_out, Zone **zone_out,
                      uint32_t min_capacity = 0);
  // Chooses the zone that holds the most data from adjacent files
  // Closer files have higher weight compared to farther files
  IOStatus MatchSameLevelNearbyKeys(std::shared_ptr<ZoneFile> zonefile,
                                    Env::WriteLifeTimeHint file_lifetime,
                                    unsigned int *best_diff_out, Zone **zone_out,
                                    uint32_t min_capacity = 0);
  // Fills a zone in order of arrival (per-level)
  IOStatus MatchArrivalTimeBased(std::shared_ptr<ZoneFile> zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  // Puts files that exceed the given tombstone ratio (tunable parameter) into a special zone
  IOStatus MatchTombstoneDensity(std::shared_ptr<ZoneFile> zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  // Placeholder, forwards to MatchLifetimeBased
  IOStatus MatchTombstoneTTL(std::shared_ptr<ZoneFile> zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  // Puts all files with the kClusterTogether policy in the same zone, doesn't consider any other factor
  IOStatus MatchClusterTogether(std::shared_ptr<ZoneFile> zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  // Creates a ranking of all files in a level based on how much a file overlaps with level+1
  // Tries to put the new file in the same zone with similar-ranked files in its level
  // Closer files (rankwise) have higher weight than farther files
  IOStatus MatchOverlapChildren(std::shared_ptr<ZoneFile> zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  // Same as MatchOverlapChildren, but for level+2
  IOStatus MatchOverlapGrandchildren(std::shared_ptr<ZoneFile> zonefile,
                                      Env::WriteLifeTimeHint file_lifetime,
                                      unsigned int *best_diff_out, Zone **zone_out,
                                      uint32_t min_capacity = 0);
  // Creates a ranking of all files in a level based on compensated file size
  // Tries to put the new file in the same zone with similar-ranked files in its level
  // Closer files (rankwise) have higher weight than farther files
  IOStatus MatchCompensatedSize(std::shared_ptr<ZoneFile> zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
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
  // Returns an array of pointers to files that overlap with zonefile_ptr in a target_level
  void GetOverlappingFiles(std::vector<std::shared_ptr<ZoneFile>>& ret_container, std::shared_ptr<ZoneFile> zonefile_ptr, int target_level);
  // Returns the total number of bytes across files that overlap with zonefile_ptr in target_level
  uint64_t GetOverlapCount(std::shared_ptr<ZoneFile> zonefile_ptr, int target_level);
  // container (output) is a list of ZoneFile pointers, sorted by its compaction rank
  void OverlapRankHelper(std::vector<std::shared_ptr<ZoneFile>>& container, int own_level, int target_level);
  // Returns true if the given files overlap
  bool IsOverlapping(std::shared_ptr<ZoneFile> a, std::shared_ptr<ZoneFile> b, InternalKeyComparator& icmp);
  // Returns true if the ratio of tombstones in the given file is higher than the threshold
  bool IsHighTombstone(std::shared_ptr<ZoneFile> zonefile_ptr);

  // result (output) contains the amount of data each zone holds from the given file(s)
  // the map's key is a zone's first address, and the value is how many bytes of data from the file(s) that zone holds
  // factor is multiplied with the byte count for the file(s), used for giving different weights to different files
  void GetPerZoneContribution(std::vector<std::shared_ptr<ZoneFile>>& files, std::map<uint64_t, uint64_t>& result, double factor=1.0);
  void GetPerZoneContribution(std::shared_ptr<ZoneFile> zonefile, std::map<uint64_t, uint64_t>& result, double factor=1.0);

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
};

// the zone allocator must open a new zone, finishing existing zones if needed
#define LIFETIME_DIFF_NOT_GOOD (100)
// the zone allocator should open a new zone if it can be done without finishing an existing zone
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

// Used by MatchLifetimeBased
unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
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

}  // namespace ROCKSDB_NAMESPACE

// #endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)
