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

/** Wraps a raw buffer of zone report entries returned by the ZBD backend. */
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

/** Represents a single sequential-write zone on the ZNS device, tracking its write pointer, capacity, lifetime hint, and busy state. */
class Zone {
  ZonedBlockDevice *zbd_;
  ZonedBlockDeviceBackend *zbd_be_;
  std::atomic_bool busy_;

 public:
  explicit Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
                std::unique_ptr<ZoneList> &zones, unsigned int idx);

  uint64_t start_;  ///< Starting logical address of this zone
  uint64_t capacity_; ///< Remaining capacity in bytes
  uint64_t max_capacity_; ///< Maximum capacity in bytes
  uint64_t wp_; ///< Logical address where the next data will be written.
  Env::WriteLifeTimeHint lifetime_;
  std::atomic<uint64_t> used_capacity_;
  uint32_t reset_count_;  ///< Incremented on each call to Reset().
  uint32_t finish_count_; ///< Incremented on each call to Finish().
  PlacementPolicyType policy_;  ///< Placement policy of the first ZoneFile that wrote to this zone.
  bool flag_gc_reset_ = false;
  bool open_ = false;
  
  std::vector<int> bytes_of_level;
  int level_; ///< LSM level of the first ZoneFile that wrote to this zone.
  void UpdateBytesOfLevel(int level, int bytes);

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

/** Abstract backend interface to the ZNS device library; performs raw zone resets, finishes, reads, and writes. */
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

/** Selects the underlying storage backend used by ZonedBlockDevice. */
enum class ZbdBackendType {
  kBlockDev,
  kZoneFS,
};

/**
 * Manages all zones on a ZNS device: zone allocation, file placement policies,
 * garbage collection, space accounting, and per-level file tracking.
 */
class ZonedBlockDevice {
 private:
  std::unique_ptr<ZonedBlockDeviceBackend> zbd_be_;
  std::vector<Zone *> io_zones;   ///< Zones allowed to store SST/WAL files.
  std::vector<Zone *> meta_zones; ///< Zones reserved for storing metadata.
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

  std::atomic<long> active_io_zones_;
  std::atomic<long> open_io_zones_;
  /// Protects the zone_resources_ condition variable; notifies changes in open_io_zones_.
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

  /// Per-level lists of known ZoneFile pointers, sorted by ZoneFile::smallest_ InternalKey.
  /// levelwise_file_list_[i] is a list of all files in level-i.
  /// Each file is represented by a pointer to a ZoneFile object, which contains all file metadata.
  /// The list is sorted in the same order as the LSM tree.
  std::vector< std::list< ZoneFile* > > levelwise_file_list_;

  void EncodeJsonZone(std::ostream &json_stream,
                      const std::vector<Zone *> zones);

 public:
  uint64_t alloc_count_ = 0;    ///< Number of times GetBestOpenZoneMatch() has been called.
  uint64_t failure_count_ = 0;  ///< Number of times the default policy was invoked by GetBestOpenZoneMatch().
  
  uint64_t total_reset_count_ = 0;
  uint64_t gc_reset_count_ = 0;
  
  uint64_t nearest_real_success = 0;
  uint64_t nearest_need_new = 0;
  uint64_t nearest_got_new = 0;
  bool need_flag = false;

  /// All tunable platform parameters, loaded from param_loader.h.
  ZenfsParamContainer zenfs_parameters_;
  FILE* logfile_;  ///< Log file for minimal GC-related output.
  FILE* zonestate_logfile_; ///< Log file for detailed zone state snapshots at each GC iteration.

  /// Mutex to acquire when accessing or modifying levelwise_file_list_.
  std::mutex levelwise_files_mtx_;
  /// Mutex to acquire before taking file placement decisions to avoid race conditions
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

  /**
   * Hands over a zone to the ZoneFile for writing data.
   * Tries to hand over an already open zone (File Placement Policy).
   * If that's not possible/desirable, opens a new zone (New Zone Allocation Policy).
   * @param[in] file_lifetime write lifetime hint for the requesting file
   * @param[in] io_type IO type of the requesting file
   * @param[out] out_zone receives the allocated zone
   * @param[in] zonefile the zone file requesting a zone
   * @return IOStatus
   */
  IOStatus AllocateIOZone(Env::WriteLifeTimeHint file_lifetime, IOType io_type,
                          Zone **out_zone, ZoneFile* zonefile);
  IOStatus AllocateMetaZone(Zone **out_meta_zone);

  uint64_t GetFreeSpace();
  uint64_t GetUsedSpace();
  uint64_t GetReclaimableSpace();

  std::string GetFilename();
  uint32_t GetBlockSize();

  /// Resets all zones that don't contain any valid files, and whose capacity is full.
  IOStatus ResetUnusedIOZones(bool gc=false);
  void LogZoneStats();
  void LogZoneUsage();
  void LogGarbageInfo();
  void LogDetailedZoneState();
  void LogLevelwiseZoneStats();

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

  /// Adds a new file to the appropriate level and position of levelwise_file_list_.
  /// Called after the whole file is written.
  /// @param[in] zonefile_ptr pointer to the zone file to record
  void AddZoneFileRecord(ZoneFile* zonefile_ptr);
  void AddZoneFileRecordNoLock(ZoneFile* zonefile_ptr);
  /// Searches for and removes a particular file from levelwise_file_list_.
  /// Called when a file is marked as deleted.
  /// @param[in] zonefile_ptr pointer to the zone file to remove
  void RemoveZoneFileRecord(ZoneFile* zonefile_ptr);
  void RemoveZoneFileRecordNoLock(ZoneFile* zonefile_ptr);
  /// Re-computes the compensated file sizes for all recorded files.
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
  
  /**
   * Tries to find the best open zone for the given ZoneFile.
   * Goes to the fallback policy if the primary policy fails.
   * Goes to LIZA if even the fallback policy fails.
   * Called from @ref AllocateIOZone .
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the lifetime difference between zone_out and the file.
   * If best_diff_out is set to LIFETIME_DIFF_NOT_GOOD, @ref AllocateIOZone will try to open a new zone for the file.
   * If set to LIFETIME_DIFF_COULD_BE_WORSE, @ref AllocateIOZone will try to open a new zone if the open zone limit hasn't been reached.
   * @param[in,out] zone_out on entry, the previously held zone (used to detect change); on exit, the best matching zone
   * @param[in] min_capacity the minimum amount of free space the chosen zone needs to have.
   * @return IOStatus
   */
  IOStatus GetBestOpenZoneMatch(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);

  /**
   * ZenFS default file placement policy.
   * Tries to put the file in a zone with equal or higher write lifetime hint.
   * Levels 0-1 map to WLTH_MEDIUM, level 2 to WLTH_LONG, levels 3+ to WLTH_EXTREME.
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the best match score found
   * @param[out] zone_out the matched zone
   * @param[in] min_capacity minimum zone capacity required
   * @return IOStatus
   */
  IOStatus MatchLifetimeBased(ZoneFile* zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  /**
   * Looks for files with overlapping key ranges in the upper and lower level.
   * Chooses the zone that holds the most data from the overlapping files.
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the best match score found
   * @param[out] zone_out the matched zone
   * @param[in] min_capacity minimum zone capacity required
   * @return IOStatus
   */
  IOStatus MatchCAZA(ZoneFile* zonefile,
                      Env::WriteLifeTimeHint file_lifetime,
                      unsigned int *best_diff_out, Zone **zone_out,
                      uint32_t min_capacity = 0);
  /**
   * Chooses the zone that holds the most data from adjacent same-level files.
   * Closer files (by key range) have higher weight than farther files.
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the best match score found
   * @param[out] zone_out the matched zone
   * @param[in] min_capacity minimum zone capacity required
   * @return IOStatus
   */
  IOStatus MatchSameLevelNearbyKeys(ZoneFile* zonefile,
                                    Env::WriteLifeTimeHint file_lifetime,
                                    unsigned int *best_diff_out, Zone **zone_out,
                                    uint32_t min_capacity = 0);
  IOStatus MatchSameLevelNearbyKeysSimple(ZoneFile* zonefile,
                                          Env::WriteLifeTimeHint file_lifetime,
                                          unsigned int *best_diff_out, Zone **zone_out,
                                          uint32_t min_capacity = 0);
  /// Fills a zone in order of file arrival, per level.
  /// @param[in] zonefile the zone file requesting a zone
  /// @param[in] file_lifetime write lifetime hint for the file
  /// @param[out] best_diff_out the best match score found
  /// @param[out] zone_out the matched zone
  /// @param[in] min_capacity minimum zone capacity required
  /// @return IOStatus
  IOStatus MatchArrivalTimeBased(ZoneFile* zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  /// Puts files whose tombstone ratio exceeds the tunable threshold into a dedicated zone.
  /// @param[in] zonefile the zone file requesting a zone
  /// @param[in] file_lifetime write lifetime hint for the file
  /// @param[out] best_diff_out the best match score found
  /// @param[out] zone_out the matched zone (set to nullptr if the file does not qualify)
  /// @param[in] min_capacity minimum zone capacity required
  /// @return IOStatus
  IOStatus MatchTombstoneDensity(ZoneFile* zonefile,
                                  Env::WriteLifeTimeHint file_lifetime,
                                  unsigned int *best_diff_out, Zone **zone_out,
                                  uint32_t min_capacity = 0);
  /// Placeholder policy that forwards directly to MatchLifetimeBased.
  IOStatus MatchTombstoneTTL(ZoneFile* zonefile,
                              Env::WriteLifeTimeHint file_lifetime,
                              unsigned int *best_diff_out, Zone **zone_out,
                              uint32_t min_capacity = 0);
  /// Puts all files with the kClusterTogether policy in the same zone, ignoring all other factors.
  /// @param[in] zonefile the zone file requesting a zone
  /// @param[in] file_lifetime write lifetime hint for the file
  /// @param[out] best_diff_out the best match score found
  /// @param[out] zone_out the matched zone
  /// @param[in] min_capacity minimum zone capacity required
  /// @return IOStatus
  IOStatus MatchClusterTogether(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  /**
   * Ranks files in a level by overlap with level+1, then co-locates the new file
   * with similarly-ranked peers. Closer files (rank-wise) have higher weight.
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the best match score found
   * @param[out] zone_out the matched zone
   * @param[in] min_capacity minimum zone capacity required
   * @return IOStatus
   */
  IOStatus MatchOverlapChildren(ZoneFile* zonefile,
                                Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0);
  /// Same as MatchOverlapChildren, but ranks by overlap with level+2.
  /// @param[in] zonefile the zone file requesting a zone
  /// @param[in] file_lifetime write lifetime hint for the file
  /// @param[out] best_diff_out the best match score found
  /// @param[out] zone_out the matched zone
  /// @param[in] min_capacity minimum zone capacity required
  /// @return IOStatus
  IOStatus MatchOverlapGrandchildren(ZoneFile* zonefile,
                                      Env::WriteLifeTimeHint file_lifetime,
                                      unsigned int *best_diff_out, Zone **zone_out,
                                      uint32_t min_capacity = 0);
  /**
   * Ranks files in a level by compensated file size, then co-locates the new file
   * with similarly-ranked peers. Closer files (rank-wise) have higher weight.
   * @param[in] zonefile the zone file requesting a zone
   * @param[in] file_lifetime write lifetime hint for the file
   * @param[out] best_diff_out the best match score found
   * @param[out] zone_out the matched zone
   * @param[in] min_capacity minimum zone capacity required
   * @return IOStatus
   */
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
  
  /**
   * Allocates a new empty zone. Only relevant when the underlying ZNS SSD uses
   * static zone-to-block mapping.
   * @param[out] zone_out receives the allocated empty zone
   * @param[in] level the level of the file requesting a zone; influences zone selection policy
   * @return IOStatus
   */
  IOStatus AllocateEmptyZone(Zone **zone_out, int level=-1);
  /// Hands over the first available empty zone.
  /// @param[out] zone_out receives the allocated empty zone
  /// @return IOStatus
  IOStatus AllocateEmptyZoneDefault(Zone **zone_out);
  /// Allocates an empty zone using round-robin ordering.
  /// @param[out] zone_out receives the allocated empty zone
  /// @return IOStatus
  IOStatus AllocateEmptyZoneSequential(Zone **zone_out);
  IOStatus AllocateEmptyZoneRandom(Zone **zone_out);
  /// Greedily allocates the empty zone with the lowest reset count (least wear).
  /// @param[out] zone_out receives the allocated empty zone
  /// @return IOStatus
  IOStatus AllocateEmptyZoneLeastWear(Zone **zone_out);
  /**
   * Wear-aware allocation: places lower-level files on high-wear zones (since they are
   * deleted less frequently) and upper-level files on low-wear zones for the opposite reason.
   * @param[out] zone_out receives the allocated empty zone
   * @param[in] level the level of the file requesting a zone
   * @return IOStatus
   */
  IOStatus AllocateEmptyZoneHotnessBased(Zone **zone_out, int level);

  /// Returns all files in @p target_level whose key range overlaps with @p zonefile_ptr.
  /// @pre levelwise_files_mtx_ must be held by the caller
  /// @param[in,out] ret_container container to append overlapping file pointers into; existing content is preserved
  /// @param[in] zonefile_ptr the file whose key range is used for overlap detection
  /// @param[in] target_level the level to search
  void GetOverlappingFiles(std::vector<ZoneFile*>& ret_container, ZoneFile* zonefile_ptr, int target_level);
  /// Returns pointers to all files at @p target_level.
  /// @param[out] ret_container cleared and filled with all file pointers from the target level
  /// @param[in] target_level the level to retrieve files from
  void GetSameLevelFiles(std::vector<ZoneFile*>& ret_container, int target_level);
  /// @return number of files in @p target_level whose key range overlaps with @p zonefile_ptr
  uint64_t GetOverlapFileCount(ZoneFile* zonefile_ptr, int target_level);
  /// @return total bytes across overlapping files in @p target_level
  uint64_t GetOverlapByteCount(ZoneFile* zonefile_ptr, int target_level);
  /// @return total key count across overlapping files in @p target_level
  uint64_t GetOverlapKeyCount(ZoneFile* zonefile_ptr, int target_level);
  /// Ranks files in @p container by overlap size with @p target_level; sorts in-place and sets each file's rank_ field.
  /// @param[in,out] container files to rank; sorted in ascending rank order on return, rank_ fields updated
  /// @param[in] own_level the level whose files are being ranked
  /// @param[in] target_level the level used to compute overlap size for ranking
  void OverlapRankHelperSizeBased(std::vector<ZoneFile*>& container, int own_level, int target_level);
  /// Ranks files in @p container by overlap key count with @p target_level; sorts in-place and sets each file's rank_ field.
  /// @param[in,out] container files to rank; sorted in ascending rank order on return, rank_ fields updated
  /// @param[in] own_level the level whose files are being ranked
  /// @param[in] target_level the level used to compute overlap key count for ranking
  void OverlapRankHelperKeyBased(std::vector<ZoneFile*>& container, int own_level, int target_level);
  /// @return true if @p a and @p b have overlapping key ranges
  bool IsOverlapping(ZoneFile* a, ZoneFile* b, InternalKeyComparator& icmp);
  /// @return true if the tombstone ratio of @p zonefile_ptr exceeds the configured threshold
  bool IsHighTombstone(ZoneFile* zonefile_ptr);

  /**
   * Computes how many bytes from the given files each zone holds.
   * @param[in] files input files to measure
   * @param[in,out] result map accumulating results: zone start address → weighted byte count; existing entries are incremented
   * @param[in] factor weight multiplier applied to each file's byte count; use to give different files different weights
   */
  void GetPerZoneContribution(std::vector<ZoneFile*>& files, std::map<uint64_t, uint64_t>& result, double factor=1.0);
  /**
   * @overload Single-file variant.
   * @param[in] zonefile single input file to measure
   * @param[in,out] result map accumulating results: zone start address → weighted byte count; existing entries are incremented
   * @param[in] factor weight multiplier applied to the file's byte count
   */
  void GetPerZoneContribution(ZoneFile* zonefile, std::map<uint64_t, uint64_t>& result, double factor=1.0);

 public:
  PlacementPolicyType GetPolicy(int level) {
    if(level < 0) return kLifetimeBased;
    if(level <= zenfs_parameters_.min_boundary) return zenfs_parameters_.upper_level_policy;
    if(level >= zenfs_parameters_.max_boundary) return zenfs_parameters_.lower_level_policy;
    return zenfs_parameters_.middle_level_policy;
  }

  PlacementPolicyType GetSecondaryPolicy(int level) {
    if(level < 0) return kLifetimeBased;
    if(level <= zenfs_parameters_.min_boundary) return zenfs_parameters_.upper_level_policy_fallback;
    if(level >= zenfs_parameters_.max_boundary) return zenfs_parameters_.lower_level_policy_fallback;
    return zenfs_parameters_.middle_level_policy_fallback;
  }

  /// Returns a score indicating how well @p zone_lifetime matches @p file_lifetime.
  /// A lower score is a better match; LIFETIME_DIFF_NOT_GOOD indicates an unsuitable zone.
  /// @param[in] zone_lifetime the zone's current write lifetime hint
  /// @param[in] file_lifetime the file's write lifetime hint
  /// @return match score (0 = exact match, LIFETIME_DIFF_NOT_GOOD = unsuitable)
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

/** Describes a contiguous byte range of a logical file that resides in a single zone. */
class ZoneExtent {
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

/** Interface for durably persisting a ZoneFile's extent metadata to the ZenFS superblock. */
class MetadataWriter {
 public:
  virtual ~MetadataWriter();
  virtual IOStatus Persist(ZoneFile* zoneFile) = 0;
};

/**
 * Represents a logical file stored across one or more zone extents.
 * Tracks key ranges, LSM level, tombstone statistics, and the active write zone.
 */
class ZoneFile {
 private:
  const uint64_t NO_EXTENT = 0xffffffffffffffff;

  ZonedBlockDevice* zbd_;

  std::vector<ZoneExtent*> extents_;  ///< File extents; a file may span multiple zones.
  std::vector<std::string> linkfiles_;

  /// Current write zone; nullptr until allocated via AllocateIOZone().
  Zone* active_zone_;
  uint64_t extent_start_ = NO_EXTENT;
  uint64_t extent_filepos_ = 0;

  Env::WriteLifeTimeHint lifetime_;
  IOType io_type_; ///< IO type; only meaningful during writes — kData for SST files.
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

  int level_ = -1;              ///< LSM level of this SST file (-1 if unset).
  InternalKey smallest_;        ///< Smallest internal key in this file.
  InternalKey largest_;         ///< Largest internal key in this file.
  InternalKeyComparator icmp_;  ///< Comparator for internal keys.

  uint64_t num_entries_ = 0;
  uint64_t num_deletions_ = 0;
  uint64_t num_range_deletions_ = 0;
  uint64_t file_size_meta_ = 0;
  uint64_t compensated_file_size_ = 0;
  uint64_t compensated_range_deletion_size_ = 0;
  SequenceNumber smallest_seqno_ = kMaxSequenceNumber;
  SequenceNumber largest_seqno_ = 0;
  
  bool has_keys_ = false; ///< True after the first call to UpdateInternalKeys() or UpdateInternalKeysRange().
  bool is_recorded_ = false;
  uint64_t key_update_count_ = 0;
  uint64_t key_update_post_alloc_count_ = 0;

  uint64_t rank_ = 100000;
  uint64_t alloc_size_ = 0; ///< Allocation size hint for non-fragmented zone allocation.

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
  /// Adds or updates this file's record in ZonedBlockDevice's levelwise_file_list_.
  /// Called from CloseWR().
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
