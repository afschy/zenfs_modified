#include "fs_zenfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <set>
#include <sstream>
#include <utility>
#include <vector>

#ifdef ZENFS_EXPORT_PROMETHEUS
#include "metrics_prometheus.h"
#endif
#include "rocksdb/utilities/object_registry.h"
#include "snapshot.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace ROCKSDB_NAMESPACE {

inline bool ends_with(std::string const& value, std::string const& ending) {
  if (ending.size() > value.size()) return false;
  return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

IOStatus ZenFS::MigrateExtents(
    const std::vector<ZoneExtentSnapshot*>& extents) {
  IOStatus s;
  // Group extents by their filename
  std::map<std::string, std::vector<ZoneExtentSnapshot*>> file_extents;
  for (auto* ext : extents) {
    std::string fname = ext->filename;
    // We only migrate SST file extents
    if (ends_with(fname, ".sst")) {
      file_extents[fname].emplace_back(ext);
    }
  }

  for (const auto& it : file_extents) {
    s = MigrateFileExtents(it.first, it.second);
    if (!s.ok()) break;
    s = zbd_->ResetUnusedIOZones(true);
    if (!s.ok()) break;
  }
  return s;
}

IOStatus ZenFS::MigrateFileExtents(
    const std::string& fname,
    const std::vector<ZoneExtentSnapshot*>& migrate_exts) {
  IOStatus s = IOStatus::OK();
  Info(logger_, "MigrateFileExtents, fname: %s, extent count: %lu",
       fname.data(), migrate_exts.size());

  // The file may be deleted by other threads, better double check.
  auto zfile = GetFile(fname);
  if (zfile == nullptr) {
    return IOStatus::OK();
  }

  // Don't migrate open for write files and prevent write reopens while we
  // migrate
  if (!zfile->TryAcquireWRLock()) {
    return IOStatus::OK();
  }

  std::vector<ZoneExtent*> new_extent_list;
  std::vector<ZoneExtent*> extents = zfile->GetExtents();
  for (const auto* ext : extents) {
    new_extent_list.push_back(
        new ZoneExtent(ext->start_, ext->length_, ext->zone_, zfile->level_));
  }

  // Modify the new extent list
  for (ZoneExtent* ext : new_extent_list) {
    // Check if current extent need to be migrated
    auto it = std::find_if(migrate_exts.begin(), migrate_exts.end(),
                           [&](const ZoneExtentSnapshot* ext_snapshot) {
                             return ext_snapshot->start == ext->start_ &&
                                    ext_snapshot->length == ext->length_;
                           });

    if (it == migrate_exts.end()) {
      Info(logger_, "Migrate extent not found, ext_start: %lu", ext->start_);
      continue;
    }

    Zone* target_zone = nullptr;

    // Allocate a new migration zone.
    bool alloc_new;
    s = zbd_->TakeMigrateZone(&target_zone, zfile.get(), zfile->GetWriteLifeTimeHint(),
                              ext->length_, &alloc_new);
    if (!s.ok()) {
      continue;
    }

    if (target_zone == nullptr) {
      zbd_->ReleaseMigrateZone(target_zone);
      Info(logger_, "Migrate Zone Acquire Failed, Ignore Task.");
      continue;
    }

    uint64_t target_start = target_zone->wp_;
    if (zfile->IsSparse()) {
      // For buffered write, ZenFS use inlined metadata for extents and each
      // extent has a SPARSE_HEADER_SIZE.
      target_start = target_zone->wp_ + ZoneFile::SPARSE_HEADER_SIZE;
      zfile->MigrateData(ext->start_ - ZoneFile::SPARSE_HEADER_SIZE,
                         ext->length_ + ZoneFile::SPARSE_HEADER_SIZE,
                         target_zone);
      zbd_->AddGCBytesWritten(ext->length_ + ZoneFile::SPARSE_HEADER_SIZE);
    } else {
      zfile->MigrateData(ext->start_, ext->length_, target_zone);
      zbd_->AddGCBytesWritten(ext->length_);
    }

    // If the file doesn't exist, skip
    if (GetFileNoLock(fname) == nullptr) {
      Info(logger_, "Migrate file not exist anymore.");
      zbd_->ReleaseMigrateZone(target_zone, alloc_new);
      break;
    }

    ext->start_ = target_start;
    ext->zone_ = target_zone;
    ext->zone_->used_capacity_ += ext->length_;

    zbd_->ReleaseMigrateZone(target_zone, alloc_new);
  }

  SyncFileExtents(zfile.get(), new_extent_list);
  zfile->ReleaseWRLock();

  Info(logger_, "MigrateFileExtents Finished, fname: %s, extent count: %lu",
       fname.data(), migrate_exts.size());
  return IOStatus::OK();
}

uint32_t ZenFS::SelectGarbageZonesDefault(ZenFSSnapshot& snapshot,
                                          uint64_t free_percent,
                                          std::set<uint64_t>& migrate_zones_start) {
  
  uint64_t threshold = (100 - GC_SLOPE * (GC_START_LEVEL - free_percent));
  uint32_t reset_zone_count = 0;
  for (const auto& zone : snapshot.zones_) {
    if (zone.capacity == 0) {
      uint64_t garbage_percent_approx =
          100 - 100 * zone.used_capacity / zone.max_capacity;
      if (garbage_percent_approx > threshold &&
          garbage_percent_approx < 100) {
        migrate_zones_start.emplace(zone.start);
        reset_zone_count++;
        Zone* z = zbd_->zone_start_to_pointer_map_[zone.start];
        if (z) z->flag_gc_reset_ = false;
      }
    }
  }
  return reset_zone_count;
}

uint32_t ZenFS::SelectGarbageZonesImproved(ZenFSSnapshot& snapshot,
                                           uint64_t free_percent,
                                           std::set<uint64_t>& migrate_zones_start) {
  
  std::vector<ZoneSnapshot> sorted_snapshots = snapshot.zones_;
  std::sort(sorted_snapshots.begin(), sorted_snapshots.end(), [](const ZoneSnapshot& a, const ZoneSnapshot& b) {
    return a.used_capacity < b.used_capacity;
  });
  
  int free_target_percent = zbd_->zenfs_parameters_.gc_stop_level - free_percent;
  // uint64_t empty_zone_count = zbd_->GetEmptyZoneCount();

  if(free_target_percent <= 0)
    return 0;
  uint64_t free_target_bytes = (zbd_->GetUsedSpace() + zbd_->GetReclaimableSpace()) * free_target_percent / 100;
  uint64_t free_target_zones = ceil(1.00 * zbd_->GetNrZones() * free_target_percent / 100);
  fprintf(zbd_->logfile_, "Trying to free %d%%, %lu MB of space\n", free_target_percent, free_target_bytes / (1<<20));
                                            
  uint32_t reset_zone_count = 0;
  // uint64_t freed_bytes = 0;
  for (const auto& zone : sorted_snapshots) {
    if(reset_zone_count >= free_target_zones) break;
    if (zone.capacity != 0) continue;
    
    migrate_zones_start.emplace(zone.start);
    reset_zone_count++;
    // freed_bytes += (zone.max_capacity - zone.used_capacity);
    Zone* z = zbd_->zone_start_to_pointer_map_[zone.start];
    if (z) z->flag_gc_reset_ = false;
  }
  return reset_zone_count;
}

void ZenFS::GCWorker() {
  ZenfsParamContainer container;
  container.LoadParamsFromFile(false);
  GC_START_LEVEL = container.gc_start_level;
  GC_SLOPE = container.gc_slope;
  GC_PAUSE_SECONDS = container.gc_pause_seconds;
  uint64_t counter = 0;
  uint64_t fail_counter = 0;

  while (run_gc_worker_) {
    usleep(1000 * 1000 * GC_PAUSE_SECONDS);
    if(!run_gc_worker_) break;
    std::lock_guard<std::mutex> lk(zbd_->alloc_mutex_);

    uint64_t free_percent = zbd_->GetFreePercent();
    counter++;

    fprintf(zbd_->logfile_, "-----%08lu-----\n", counter);
    fprintf(zbd_->zonestate_logfile_, "-----%08lu-----\n", counter);
    
    zbd_->LogLevelwiseZoneStats();
    fprintf(zbd_->logfile_, "total_reset = %lu, gc_reset = %lu, alloc = %lu, failures = %lu, ",
            zbd_->GetTotalResetCount(), zbd_->GetGCResetCount(), zbd_->GetAllocCount(), zbd_->GetFailureCount());
    fprintf(zbd_->logfile_, "above_thresh = %lu, below_thresh = %lu, below_thresh_s = %lu\n",
            zbd_->nearest_real_success, zbd_->nearest_need_new, zbd_->nearest_got_new);
    fprintf(zbd_->logfile_, "Before GC: %lu%% free, %lu empty zones\n", free_percent, zbd_->GetEmptyZoneCount());
    
    fflush(zbd_->zonestate_logfile_);
    if (free_percent > GC_START_LEVEL) {
      fprintf(zbd_->logfile_, "GC not triggered, free space must be less than %lu%%\n", GC_START_LEVEL);
      fflush(zbd_->logfile_);
      continue;
    }
    fprintf(zbd_->logfile_, "GC triggered\n");
    fflush(zbd_->logfile_);

    uint64_t data_movement_before_gc = zbd_->GetGCBytesWritten();
    uint64_t gc_reset_before_gc = zbd_->GetGCResetCount();

    ZenFSSnapshot snapshot;
    ZenFSSnapshotOptions options;

    options.zone_ = 1;
    options.zone_file_ = 1;
    options.log_garbage_ = 1;
    GetZenFSSnapshot(snapshot, options);

    std::set<uint64_t> migrate_zones_start;
    // uint32_t reset_zone_count = 0;
    switch(container.gc_type) {
      case kImprovedGC:
        SelectGarbageZonesImproved(snapshot, free_percent, migrate_zones_start);
        break;
      default:
        SelectGarbageZonesDefault(snapshot, free_percent, migrate_zones_start);
    }

    std::vector< std::vector<ZoneExtentSnapshot*> > migrate_ext_list;
    migrate_ext_list.resize(migrate_zones_start.size()+1);
    std::map<uint64_t, uint64_t> zone_start_to_idx_map;
    
    size_t index = 0;
    for (auto& zone_start : migrate_zones_start) {
      zone_start_to_idx_map[zone_start] = index;
      index++;
    }

    // std::vector<ZoneExtentSnapshot*> migrate_exts;
    for (auto& ext : snapshot.extents_) {
      if (migrate_zones_start.find(ext.zone_start) !=
          migrate_zones_start.end()) {
        index = zone_start_to_idx_map[ext.zone_start];
        migrate_ext_list[index].push_back(&ext);
        // migrate_exts.push_back(&ext);
      }
    }

    for (index = 0; index < migrate_ext_list.size(); index++) {
      if (migrate_ext_list[index].size() > 0) {
        IOStatus s;
        s = MigrateExtents(migrate_ext_list[index]);
        if (!s.ok()) {
          Error(logger_, "Garbage collection failed");
        }
      }
    }

    uint64_t data_movement_this_iteration = zbd_->GetGCBytesWritten() - data_movement_before_gc;
    uint64_t reset_count_this_iteration = zbd_->GetGCResetCount() - gc_reset_before_gc;

    fprintf(zbd_->logfile_, "Reset %lu zones this iteration, moved %0.2lf MB data\n",
            reset_count_this_iteration, 1.0*data_movement_this_iteration/(1<<20));

    fprintf(zbd_->logfile_, "Total bytes written = %0.2lf MB, Total movement due to GC = %0.2f MB\n",
            1.0*zbd_->GetTotalBytesWritten()/(1<<20), 1.0*zbd_->GetGCBytesWritten()/(1<<20));
    
    free_percent = zbd_->GetFreePercent();
    fprintf(zbd_->logfile_, "After GC: %lu pc of the space is free, %lu zones are empty\n",
            free_percent, zbd_->GetEmptyZoneCount());
    fflush(zbd_->logfile_);

    // fprintf(zbd_->zonestate_logfile_, "------------------\n");
    // zbd_->LogDetailedZoneState();
    // fprintf(zbd_->zonestate_logfile_, "-----%08lu-----\n", counter);
    // fflush(zbd_->zonestate_logfile_);

    if(container.cold_migration) MigrateColdFiles();

    if(data_movement_this_iteration) fail_counter = 0;
    else fail_counter++;
    if (fail_counter >= 10) exit(1);
  }
  fprintf(zbd_->logfile_, "Total GC = %0.2f MB\n", 1.0*zbd_->GetGCBytesWritten()/(1024*1024));
  fflush(zbd_->logfile_);
}

void ZenFS::MigrateColdFiles() {
  ZenFSSnapshot snapshot;
  ZenFSSnapshotOptions options;

  options.zone_ = 1;
  options.zone_file_ = 1;
  GetZenFSSnapshot(snapshot, options);

  // <zone_start, level_sum>, needed to calculate average level of a file
  std::unordered_map<uint64_t, double> level_map;
  // <zone_start, total_size>, needed to calculate average level of a file
  std::unordered_map<uint64_t, uint64_t> size_map;

  for (auto& ext : snapshot.extents_) {
      if(ext.level < 0 || ext.length <= 0) continue;
      if(level_map.find(ext.zone_start) == level_map.end()) {
      level_map[ext.zone_start] = 0;
      size_map[ext.zone_start] = 0;
      }
      level_map[ext.zone_start] += ext.level * ext.length;
      size_map[ext.zone_start] += ext.length;
  }
  for (const auto& it : level_map)
      level_map[it.first] = level_map[it.first] / size_map[it.first];

  std::vector<ZoneSnapshot> sorted_snapshots = snapshot.zones_;
  std::sort(sorted_snapshots.begin(), sorted_snapshots.end(), [](const ZoneSnapshot& a, const ZoneSnapshot& b) {
      return a.reset_count < b.reset_count;
  });
  double interval = 1.00 * sorted_snapshots.size() / zbd_->zenfs_parameters_.max_level;

  int max_diff = 0;
  int zone_start_max_diff = -1;

  for(unsigned int i=0; i<sorted_snapshots.size(); i++) {
      if(size_map.find(sorted_snapshots[i].start) == size_map.end())
      continue;
      int ideal_bin = 1.00 * i / interval;
      int real_bin = level_map[sorted_snapshots[i].start];
      int diff = real_bin - ideal_bin;
      
      if(diff <= max_diff) continue;

      max_diff = diff;
      zone_start_max_diff = sorted_snapshots[i].start;
  }

  if(zone_start_max_diff < 3) return;

  std::vector<ZoneExtentSnapshot*> migrate_exts;
  for (auto& ext : snapshot.extents_) {
      if (zone_start_max_diff >= 0 && ext.zone_start == (unsigned)zone_start_max_diff) {
      migrate_exts.push_back(&ext);
      }
  }

  if (migrate_exts.size() > 0) {
      IOStatus s;
      Info(logger_, "Migrating %d extents of cold files\n",
          (int)migrate_exts.size());
      s = MigrateExtents(migrate_exts);
      if (!s.ok()) {
      Error(logger_, "Cold migration failed");
      }
  }
}

}; //namespace ROCKSDB_NAMESPACE