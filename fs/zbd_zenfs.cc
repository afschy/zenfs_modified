// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <random>

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "snapshot.h"
#include "zbdlib_zenfs.h"
#include "zonefs_zenfs.h"
#include "db/dbformat.h"

#define KB (1024)
#define MB (1024 * KB)

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
           std::unique_ptr<ZoneList> &zones, unsigned int idx)
    : zbd_(zbd),
      zbd_be_(zbd_be),
      busy_(false),
      start_(zbd_be->ZoneStart(zones, idx)),
      max_capacity_(zbd_be->ZoneMaxCapacity(zones, idx)),
      wp_(zbd_be->ZoneWp(zones, idx)) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  reset_count_ = 0;
  finish_count_ = 0;
  policy_ = kLifetimeBased;
  level_ = -1;
  if (zbd_be->ZoneIsWritable(zones, idx))
    capacity_ = max_capacity_ - (wp_ - start_);
}

bool Zone::IsUsed() { return (used_capacity_ > 0); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_;
  json_stream << "\"reset_count\":" << reset_count_;
  json_stream << "\"finish_count\":" << finish_count_;
  json_stream << "}";
}

IOStatus Zone::Reset() {
  bool offline;
  uint64_t max_capacity;

  assert(!IsUsed());
  assert(IsBusy());

  IOStatus ios = zbd_be_->Reset(start_, &offline, &max_capacity);
  if (ios != IOStatus::OK()) return ios;

  if (offline)
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = max_capacity;

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;
  reset_count_++;

  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  assert(IsBusy());

  IOStatus ios = zbd_be_->Finish(start_);
  if (ios != IOStatus::OK()) return ios;

  capacity_ = 0;
  wp_ = start_ + zbd_->GetZoneSize();
  finish_count_++;

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  assert(IsBusy());

  if (!(IsEmpty() || IsFull())) {
    IOStatus ios = zbd_be_->Close(start_);
    if (ios != IOStatus::OK()) return ios;
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_ZONE_WRITE_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportThroughput(ZENFS_ZONE_WRITE_THROUGHPUT, size);
  char *ptr = data;
  uint32_t left = size;
  int ret;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  while (left) {
    ret = zbd_be_->Write(ptr, left, wp_);
    if (ret < 0) {
      return IOStatus::IOError(strerror(errno));
    }

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
    zbd_->AddBytesWritten(ret);
  }

  return IOStatus::OK();
}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zbd_be_->GetZoneSize()))
      return z;
  return nullptr;
}

ZonedBlockDevice::ZonedBlockDevice(std::string path, ZbdBackendType backend,
                                   std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ZenFSMetrics> metrics)
    : logger_(logger), metrics_(metrics) {
  if (backend == ZbdBackendType::kBlockDev) {
    zbd_be_ = std::unique_ptr<ZbdlibBackend>(new ZbdlibBackend(path));
    Info(logger_, "New Zoned Block Device: %s", zbd_be_->GetFilename().c_str());
  } else if (backend == ZbdBackendType::kZoneFS) {
    zbd_be_ = std::unique_ptr<ZoneFsBackend>(new ZoneFsBackend(path));
    Info(logger_, "New zonefs backing: %s", zbd_be_->GetFilename().c_str());
  }
  zenfs_parameters_.LoadParamsFromFile();
}

IOStatus ZonedBlockDevice::Open(bool readonly, bool exclusive) {
  std::unique_ptr<ZoneList> zone_rep;
  unsigned int max_nr_active_zones;
  unsigned int max_nr_open_zones;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  // Reserve one zone for metadata and another one for extent migration
  int reserved_zones = 2;

  if (!readonly && !exclusive)
    return IOStatus::InvalidArgument("Write opens must be exclusive");

  IOStatus ios = zbd_be_->Open(readonly, exclusive, &max_nr_active_zones,
                               &max_nr_open_zones);
  if (ios != IOStatus::OK()) return ios;

  if (zbd_be_->GetNrZones() < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned backend (" +
                                  std::to_string(ZENFS_MIN_ZONES) +
                                  " required)");
  }

  if (max_nr_active_zones == 0)
    max_nr_active_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

  if (max_nr_open_zones == 0)
    max_nr_open_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_open_io_zones_ = max_nr_open_zones - reserved_zones;

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       zbd_be_->GetNrZones(), max_nr_active_zones, max_nr_open_zones);

  zone_rep = zbd_be_->ListZones();
  if (zone_rep == nullptr || zone_rep->ZoneCount() != zbd_be_->GetNrZones()) {
    Error(logger_, "Failed to list zones");
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_META_ZONES && i < zone_rep->ZoneCount()) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        meta_zones.push_back(new Zone(this, zbd_be_.get(), zone_rep, i));
      }
      m++;
    }
    i++;
  }

  active_io_zones_ = 0;
  open_io_zones_ = 0;

  for (; i < zone_rep->ZoneCount(); i++) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        Zone *newZone = new Zone(this, zbd_be_.get(), zone_rep, i);
        if (!newZone->Acquire()) {
          assert(false);
          return IOStatus::Corruption("Failed to set busy flag of zone " +
                                      std::to_string(newZone->GetZoneNr()));
        }
        io_zones.push_back(newZone);
        if (zbd_be_->ZoneIsActive(zone_rep, i)) {
          active_io_zones_++;
          if (zbd_be_->ZoneIsOpen(zone_rep, i)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
        IOStatus status = newZone->CheckRelease();
        if (!status.ok()) {
          return status;
        }
      }
    }
  }

  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    if (z->IsFull()) reclaimable += (z->max_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

void ZonedBlockDevice::LogGarbageInfo() {
  // Log zone garbage stats vector.
  //
  // The values in the vector represents how many zones with target garbage
  // percent. Garbage percent of each index: [0%, <10%, < 20%, ... <100%, 100%]
  // For example `[100, 1, 2, 3....]` means 100 zones are empty, 1 zone has less
  // than 10% garbage, 2 zones have  10% ~ 20% garbage ect.
  //
  // We don't need to lock io_zones since we only read data and we don't need
  // the result to be precise.
  int zone_gc_stat[12] = {0};
  for (auto z : io_zones) {
    if (!z->Acquire()) {
      continue;
    }

    if (z->IsEmpty()) {
      zone_gc_stat[0]++;
      z->Release();
      continue;
    }

    double garbage_rate = 0;
    if (z->IsFull()) {
      garbage_rate =
          double(z->max_capacity_ - z->used_capacity_) / z->max_capacity_;
    } else {
      garbage_rate =
          double(z->wp_ - z->start_ - z->used_capacity_) / z->max_capacity_;
    }
    assert(garbage_rate >= 0);
    int idx = int((garbage_rate + 0.1) * 10);
    zone_gc_stat[idx]++;

    z->Release();
  }

  std::stringstream ss;
  ss << "Zone Garbage Stats: [";
  for (int i = 0; i < 12; i++) {
    ss << zone_gc_stat[i] << " ";
  }
  ss << "]";
  Info(logger_, "%s", ss.str().data());
}

ZonedBlockDevice::~ZonedBlockDevice() {
  for (const auto z : meta_zones) {
    delete z;
  }

  for (const auto z : io_zones) {
    delete z;
  }
}

IOStatus ZonedBlockDevice::AllocateMetaZone(Zone **out_meta_zone) {
  assert(out_meta_zone);
  *out_meta_zone = nullptr;
  ZenFSMetricsLatencyGuard guard(metrics_, ZENFS_META_ALLOC_LATENCY,
                                 Env::Default());
  metrics_->ReportQPS(ZENFS_META_ALLOC_QPS, 1);

  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (z->Acquire()) {
      if (!z->IsUsed()) {
        if (!z->IsEmpty() && !z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          IOStatus status = z->CheckRelease();
          if (!status.ok()) return status;
          continue;
        }
        *out_meta_zone = z;
        return IOStatus::OK();
      }
    }
  }
  assert(true);
  Error(logger_, "Out of metadata zones, we should go to read only now.");
  return IOStatus::NoSpace("Out of metadata zones");
}

IOStatus ZonedBlockDevice::ResetUnusedIOZones() {
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (!z->IsEmpty() && !z->IsUsed()) {
        bool full = z->IsFull();
        IOStatus reset_status = z->Reset();
        IOStatus release_status = z->CheckRelease();
        if (!reset_status.ok()) return reset_status;
        if (!release_status.ok()) return release_status;
        if (!full) PutActiveIOZoneToken();
      } else {
        IOStatus release_status = z->CheckRelease();
        if (!release_status.ok()) return release_status;
      }
    }
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::WaitForOpenIOZoneToken(bool prioritized) {
  long allocator_open_limit;

  /* Avoid non-priortized allocators from starving prioritized ones */
  if (prioritized) {
    allocator_open_limit = max_nr_open_io_zones_;
  } else {
    allocator_open_limit = max_nr_open_io_zones_ - 1;
  }

  /* Wait for an open IO Zone token - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutOpenIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  zone_resources_.wait(lk, [this, allocator_open_limit] {
    if (open_io_zones_.load() < allocator_open_limit) {
      open_io_zones_++;
      return true;
    } else {
      return false;
    }
  });
}

bool ZonedBlockDevice::GetActiveIOZoneTokenIfAvailable() {
  /* Grap an active IO Zone token if available - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutActiveIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  if (active_io_zones_.load() < max_nr_active_io_zones_) {
    active_io_zones_++;
    return true;
  }
  return false;
}

void ZonedBlockDevice::PutOpenIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    open_io_zones_--;
  }
  zone_resources_.notify_one();
}

void ZonedBlockDevice::PutActiveIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    active_io_zones_--;
  }
  zone_resources_.notify_one();
}

IOStatus ZonedBlockDevice::ApplyFinishThreshold() {
  IOStatus s;

  if (finish_threshold_ == 0) return IOStatus::OK();

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      bool within_finish_threshold =
          z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100);
      if (!(z->IsEmpty() || z->IsFull()) && within_finish_threshold) {
        /* If there is less than finish_threshold_% remaining capacity in a
         * non-open-zone, finish the zone */
        s = z->Finish();
        if (!s.ok()) {
          z->Release();
          Debug(logger_, "Failed finishing zone");
          return s;
        }
        s = z->CheckRelease();
        if (!s.ok()) return s;
        PutActiveIOZoneToken();
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::FinishCheapestIOZone() {
  IOStatus s;
  Zone *finish_victim = nullptr;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty() || z->IsFull()) {
        s = z->CheckRelease();
        if (!s.ok()) return s;
        continue;
      }
      if (finish_victim == nullptr) {
        finish_victim = z;
        continue;
      }
      if (finish_victim->capacity_ > z->capacity_) {
        s = finish_victim->CheckRelease();
        if (!s.ok()) return s;
        finish_victim = z;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  // If all non-busy zones are empty or full, we should return success.
  if (finish_victim == nullptr) {
    Info(logger_, "All non-busy zones are empty or full, skip.");
    return IOStatus::OK();
  }

  s = finish_victim->Finish();
  IOStatus release_status = finish_victim->CheckRelease();

  if (s.ok()) {
    PutActiveIOZoneToken();
  }

  if (!release_status.ok()) {
    return release_status;
  }

  return s;
}

IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out,
    Zone **zone_out, uint32_t min_capacity) {
  
  // If not SST file or required info missing, use the default
  if(zonefile->level_ < 0 || !zonefile->has_keys_ || zonefile->GetIOType() != IOType::kData)
    return MatchLifetimeBased(zonefile, file_lifetime, best_diff_out, zone_out);
  
  // function pointer type that can hold any instance of a Match.* function
  typedef IOStatus (ZonedBlockDevice::*MatchFunctionType)(std::shared_ptr<ZoneFile> zonefile, Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out, Zone **zone_out, uint32_t min_capacity);
  static std::unordered_map<MappingPolicyType, MatchFunctionType> function_map;

  function_map = {
    {kLifetimeBased, &ZonedBlockDevice::MatchLifetimeBased},
    {kCAZA, &ZonedBlockDevice::MatchCAZA},
    {kSameLevelNearbyKeys, &ZonedBlockDevice::MatchSameLevelNearbyKeys},
    {kArrivalTimeBased, &ZonedBlockDevice::MatchArrivalTimeBased},
    {kTombstoneDensity, &ZonedBlockDevice::MatchTombstoneDensity},
    {kTombstoneTTL, &ZonedBlockDevice::MatchTombstoneTTL},
    {kClusterTogether, &ZonedBlockDevice::MatchClusterTogether},
    {kOverlapChildren, &ZonedBlockDevice::MatchOverlapChildren},
    {kOverlapGrandchildren, &ZonedBlockDevice::MatchOverlapGrandchildren},
    {kCompensatedSize, &ZonedBlockDevice::MatchCompensatedSize},
  };

  MappingPolicyType primary_policy;
  MappingPolicyType fallback_policy;
  if(zonefile->level_ <= zenfs_parameters_.min_boundary) {
    primary_policy = zenfs_parameters_.upper_level_policy;
    fallback_policy = zenfs_parameters_.upper_level_policy_fallback;
  }
  else if(zonefile->level_ >= zenfs_parameters_.max_boundary) {
    primary_policy = zenfs_parameters_.lower_level_policy;
    fallback_policy = zenfs_parameters_.lower_level_policy_fallback;
  }
  else {
    primary_policy = zenfs_parameters_.middle_level_policy;
    fallback_policy = zenfs_parameters_.middle_level_policy_fallback;
  }

  MatchFunctionType primary_policy_function = function_map[primary_policy];
  MatchFunctionType fallback_policy_function = function_map[fallback_policy];

  IOStatus s;
  Zone* old_zone = *zone_out;
  
  s = (this->*primary_policy_function)(zonefile, file_lifetime, best_diff_out, zone_out, min_capacity);
  if(*zone_out != old_zone) // found a zone
    return s;

  s = (this->*fallback_policy_function)(zonefile, file_lifetime, best_diff_out, zone_out, min_capacity);
  // primary policy needs a special zone but didn't find one, should open a new zone for that purpose
  if(primary_policy == kArrivalTimeBased || primary_policy == kClusterTogether)
    *best_diff_out = std::max((unsigned int)LIFETIME_DIFF_COULD_BE_WORSE, *best_diff_out);
  if(primary_policy == kTombstoneDensity && IsHighTombstone(zonefile))
    *best_diff_out = std::max((unsigned int)LIFETIME_DIFF_COULD_BE_WORSE, *best_diff_out);
  if(*zone_out != old_zone)
    return s;

  s = MatchLifetimeBased(zonefile, file_lifetime, best_diff_out, zone_out, min_capacity);
  *best_diff_out = std::max((unsigned int)LIFETIME_DIFF_COULD_BE_WORSE, *best_diff_out);
  return s;
}

IOStatus ZonedBlockDevice::AllocateEmptyZone(Zone **zone_out, int level) {
  zenfs_parameters_.lock_max_level.lock();
  if(level>0 && level>(int)zenfs_parameters_.max_level)
    zenfs_parameters_.max_level = level;
  zenfs_parameters_.lock_max_level.unlock();

  switch(zenfs_parameters_.empty_zone_allocator) {
    case kDefault:
      return AllocateEmptyZoneDefault(zone_out);
    case kSequential:
      return AllocateEmptyZoneSequential(zone_out);
    case kRandom:
      return AllocateEmptyZoneRandom(zone_out);
    case kLeastWear:
      return AllocateEmptyZoneLeastWear(zone_out);
    case kHotnessBased:
      return AllocateEmptyZoneHotnessBased(zone_out, level);
    default:
      return AllocateEmptyZoneSequential(zone_out);
  }
}

IOStatus ZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  int ret = zbd_be_->InvalidateCache(pos, size);

  if (ret) {
    return IOStatus::IOError("Failed to invalidate cache");
  }
  return IOStatus::OK();
}

int ZonedBlockDevice::Read(char *buf, uint64_t offset, int n, bool direct) {
  int ret = 0;
  int left = n;
  int r = -1;

  while (left) {
    r = zbd_be_->Read(buf, left, offset, direct);
    if (r <= 0) {
      if (r == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
    ret += r;
    buf += r;
    left -= r;
    offset += r;
  }

  if (r < 0) return r;
  return ret;
}

IOStatus ZonedBlockDevice::ReleaseMigrateZone(Zone *zone) {
  IOStatus s = IOStatus::OK();
  {
    std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
    migrating_ = false;
    if (zone != nullptr) {
      s = zone->CheckRelease();
      Info(logger_, "ReleaseMigrateZone: %lu", zone->start_);
    }
  }
  migrate_resource_.notify_one();
  return s;
}

IOStatus ZonedBlockDevice::TakeMigrateZone(Zone **out_zone,
                                           std::shared_ptr<ZoneFile> zonefile,
                                           Env::WriteLifeTimeHint file_lifetime,
                                           uint32_t min_capacity) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  migrate_resource_.wait(lock, [this] { return !migrating_; });

  migrating_ = true;

  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  auto s =
      GetBestOpenZoneMatch(zonefile, file_lifetime, &best_diff, out_zone, min_capacity);
  if (s.ok() && (*out_zone) != nullptr) {
    Info(logger_, "TakeMigrateZone: %lu", (*out_zone)->start_);
  } else {
    migrating_ = false;
  }

  return s;
}

IOStatus ZonedBlockDevice::AllocateIOZone(Env::WriteLifeTimeHint file_lifetime,
                                          IOType io_type,
                                          Zone **out_zone,
                                          std::shared_ptr<ZoneFile> zonefile) {

  zenfs_parameters_.lock_max_level.lock();
  if(zonefile->level_ > zenfs_parameters_.max_level)
    zenfs_parameters_.max_level = zonefile->level_;
  zenfs_parameters_.lock_max_level.unlock();

  Zone *allocated_zone = nullptr;
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  int new_zone = 0;
  IOStatus s;

  auto tag = ZENFS_WAL_IO_ALLOC_LATENCY;
  if (io_type != IOType::kWAL) {
    // L0 flushes have lifetime MEDIUM
    if (file_lifetime == Env::WLTH_MEDIUM) {
      tag = ZENFS_L0_IO_ALLOC_LATENCY;
    } else {
      tag = ZENFS_NON_WAL_IO_ALLOC_LATENCY;
    }
  }

  ZenFSMetricsLatencyGuard guard(metrics_, tag, Env::Default());
  metrics_->ReportQPS(ZENFS_IO_ALLOC_QPS, 1);

  // Check if a deferred IO error was set
  s = GetZoneDeferredStatus();
  if (!s.ok()) {
    return s;
  }

  if (io_type != IOType::kWAL) {
    s = ApplyFinishThreshold();
    if (!s.ok()) {
      return s;
    }
  }

  WaitForOpenIOZoneToken(io_type == IOType::kWAL);

  /* Try to fill an already open zone(with the best life time diff) */
  s = GetBestOpenZoneMatch(zonefile, file_lifetime, &best_diff, &allocated_zone);
  if (!s.ok()) {
    PutOpenIOZoneToken();
    return s;
  }

  // Holding allocated_zone if != nullptr

  if (best_diff >= LIFETIME_DIFF_COULD_BE_WORSE) {
    bool got_token = GetActiveIOZoneTokenIfAvailable();

    /* If we did not get a token, try to use the best match, even if the life
     * time diff not good but a better choice than to finish an existing zone
     * and open a new one
     */
    if (allocated_zone != nullptr) {
      if (!got_token && best_diff == LIFETIME_DIFF_COULD_BE_WORSE) {
        Debug(logger_,
              "Allocator: avoided a finish by relaxing lifetime diff "
              "requirement\n");
      } else {
        s = allocated_zone->CheckRelease();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          if (got_token) PutActiveIOZoneToken();
          return s;
        }
        allocated_zone = nullptr;
      }
    }

    /* If we haven't found an open zone to fill, open a new zone */
    if (allocated_zone == nullptr) {
      /* We have to make sure we can open an empty zone */
      while (!got_token && !GetActiveIOZoneTokenIfAvailable()) {
        s = FinishCheapestIOZone();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          return s;
        }
      }

      s = AllocateEmptyZone(&allocated_zone, zonefile->level_);
      if (!s.ok()) {
        PutActiveIOZoneToken();
        PutOpenIOZoneToken();
        return s;
      }

      if (allocated_zone != nullptr) {
        assert(allocated_zone->IsBusy());
        allocated_zone->lifetime_ = file_lifetime;
        allocated_zone->policy_ = GetPolicy(zonefile->level_);
        allocated_zone->level_ = zonefile->level_;
        new_zone = true;
      } else {
        PutActiveIOZoneToken();
      }
    }
  }

  if (allocated_zone) {
    assert(allocated_zone->IsBusy());
    Debug(logger_,
          "Allocating zone(new=%d) start: 0x%lx wp: 0x%lx lt: %d file lt: %d\n",
          new_zone, allocated_zone->start_, allocated_zone->wp_,
          allocated_zone->lifetime_, file_lifetime);
  } else {
    PutOpenIOZoneToken();
  }

  if (io_type != IOType::kWAL) {
    LogZoneStats();
  }

  *out_zone = allocated_zone;

  metrics_->ReportGeneral(ZENFS_OPEN_ZONES_COUNT, open_io_zones_);
  metrics_->ReportGeneral(ZENFS_ACTIVE_ZONES_COUNT, active_io_zones_);

  return IOStatus::OK();
}

std::string ZonedBlockDevice::GetFilename() { return zbd_be_->GetFilename(); }

uint32_t ZonedBlockDevice::GetBlockSize() { return zbd_be_->GetBlockSize(); }

uint64_t ZonedBlockDevice::GetZoneSize() { return zbd_be_->GetZoneSize(); }

uint32_t ZonedBlockDevice::GetNrZones() { return zbd_be_->GetNrZones(); }

void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const std::vector<Zone *> zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, meta_zones);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones);
  json_stream << "}";
}

IOStatus ZonedBlockDevice::GetZoneDeferredStatus() {
  std::lock_guard<std::mutex> lock(zone_deferred_status_mutex_);
  return zone_deferred_status_;
}

void ZonedBlockDevice::SetZoneDeferredStatus(IOStatus status) {
  std::lock_guard<std::mutex> lk(zone_deferred_status_mutex_);
  if (!zone_deferred_status_.ok()) {
    zone_deferred_status_ = status;
  }
}

void ZonedBlockDevice::GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot) {
  for (auto *zone : io_zones) {
    snapshot.emplace_back(*zone);
  }
}

ZoneExtent::ZoneExtent(uint64_t start, uint64_t length, Zone* zone, int level)
    : start_(start), length_(length), zone_(zone), level_(level) {}

Status ZoneExtent::DecodeFrom(Slice* input) {
  if (input->size() != (sizeof(start_) + sizeof(length_)))
    return Status::Corruption("ZoneExtent", "Error: length missmatch");

  GetFixed64(input, &start_);
  GetFixed64(input, &length_);
  return Status::OK();
}

void ZoneExtent::EncodeTo(std::string* output) {
  PutFixed64(output, start_);
  PutFixed64(output, length_);
}

void ZoneExtent::EncodeJson(std::ostream& json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"length\":" << length_;
  json_stream << "}";
}

enum ZoneFileTag : uint32_t {
  kFileID = 1,
  kFileNameDeprecated = 2,
  kFileSize = 3,
  kWriteLifeTimeHint = 4,
  kExtent = 5,
  kModificationTime = 6,
  kActiveExtentStart = 7,
  kIsSparse = 8,
  kLinkedFilename = 9,
};

void ZoneFile::EncodeTo(std::string* output, uint32_t extent_start) {
  PutFixed32(output, kFileID);
  PutFixed64(output, file_id_);

  PutFixed32(output, kFileSize);
  PutFixed64(output, file_size_);

  PutFixed32(output, kWriteLifeTimeHint);
  PutFixed32(output, (uint32_t)lifetime_);

  for (uint32_t i = extent_start; i < extents_.size(); i++) {
    std::string extent_str;

    PutFixed32(output, kExtent);
    extents_[i]->EncodeTo(&extent_str);
    PutLengthPrefixedSlice(output, Slice(extent_str));
  }

  PutFixed32(output, kModificationTime);
  PutFixed64(output, (uint64_t)m_time_);

  /* We store the current extent start - if there is a crash
   * we know that this file wrote the data starting from
   * active extent start up to the zone write pointer.
   * We don't need to store the active zone as we can look it up
   * from extent_start_ */
  PutFixed32(output, kActiveExtentStart);
  PutFixed64(output, extent_start_);

  if (is_sparse_) {
    PutFixed32(output, kIsSparse);
  }

  for (uint32_t i = 0; i < linkfiles_.size(); i++) {
    PutFixed32(output, kLinkedFilename);
    PutLengthPrefixedSlice(output, Slice(linkfiles_[i]));
  }
}

void ZoneFile::EncodeJson(std::ostream& json_stream) {
  json_stream << "{";
  json_stream << "\"id\":" << file_id_ << ",";
  json_stream << "\"size\":" << file_size_ << ",";
  json_stream << "\"hint\":" << lifetime_ << ",";
  json_stream << "\"filename\":[";

  bool first_element = true;
  for (const auto& name : GetLinkFiles()) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    json_stream << "\"" << name << "\"";
  }
  json_stream << "],";

  json_stream << "\"extents\":[";

  first_element = true;
  for (ZoneExtent* extent : extents_) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    extent->EncodeJson(json_stream);
  }
  json_stream << "]}";
}

Status ZoneFile::DecodeFrom(Slice* input) {
  uint32_t tag = 0;

  GetFixed32(input, &tag);
  if (tag != kFileID || !GetFixed64(input, &file_id_))
    return Status::Corruption("ZoneFile", "File ID missing");

  while (true) {
    Slice slice;
    ZoneExtent* extent;
    Status s;

    if (!GetFixed32(input, &tag)) break;

    switch (tag) {
      case kFileSize:
        if (!GetFixed64(input, &file_size_))
          return Status::Corruption("ZoneFile", "Missing file size");
        break;
      case kWriteLifeTimeHint:
        uint32_t lt;
        if (!GetFixed32(input, &lt))
          return Status::Corruption("ZoneFile", "Missing life time hint");
        lifetime_ = (Env::WriteLifeTimeHint)lt;
        break;
      case kExtent:
        extent = new ZoneExtent(0, 0, nullptr, -1);
        GetLengthPrefixedSlice(input, &slice);
        s = extent->DecodeFrom(&slice);
        if (!s.ok()) {
          delete extent;
          return s;
        }
        extent->zone_ = zbd_->GetIOZone(extent->start_);
        if (!extent->zone_)
          return Status::Corruption("ZoneFile", "Invalid zone extent");
        extent->zone_->used_capacity_ += extent->length_;
        extents_.push_back(extent);
        break;
      case kModificationTime:
        uint64_t ct;
        if (!GetFixed64(input, &ct))
          return Status::Corruption("ZoneFile", "Missing creation time");
        m_time_ = (time_t)ct;
        break;
      case kActiveExtentStart:
        uint64_t es;
        if (!GetFixed64(input, &es))
          return Status::Corruption("ZoneFile", "Active extent start");
        extent_start_ = es;
        break;
      case kIsSparse:
        is_sparse_ = true;
        break;
      case kLinkedFilename:
        if (!GetLengthPrefixedSlice(input, &slice))
          return Status::Corruption("ZoneFile", "LinkFilename missing");

        if (slice.ToString().length() == 0)
          return Status::Corruption("ZoneFile", "Zero length Linkfilename");

        linkfiles_.push_back(slice.ToString());
        break;
      default:
        return Status::Corruption("ZoneFile", "Unexpected tag");
    }
  }

  MetadataSynced();
  return Status::OK();
}

Status ZoneFile::MergeUpdate(std::shared_ptr<ZoneFile> update, bool replace) {
  if (file_id_ != update->GetID())
    return Status::Corruption("ZoneFile update", "ID missmatch");

  SetFileSize(update->GetFileSize());
  SetWriteLifeTimeHint(update->GetWriteLifeTimeHint());
  SetFileModificationTime(update->GetFileModificationTime());

  if (replace) {
    ClearExtents();
  }

  std::vector<ZoneExtent*> update_extents = update->GetExtents();
  for (long unsigned int i = 0; i < update_extents.size(); i++) {
    ZoneExtent* extent = update_extents[i];
    Zone* zone = extent->zone_;
    zone->used_capacity_ += extent->length_;
    extents_.push_back(new ZoneExtent(extent->start_, extent->length_, zone, -1));
  }
  extent_start_ = update->GetExtentStart();
  is_sparse_ = update->IsSparse();
  MetadataSynced();

  linkfiles_.clear();
  for (const auto& name : update->GetLinkFiles()) linkfiles_.push_back(name);

  return Status::OK();
}

ZoneFile::ZoneFile(ZonedBlockDevice* zbd, uint64_t file_id,
                   MetadataWriter* metadata_writer)
    : zbd_(zbd),
      active_zone_(NULL),
      extent_start_(NO_EXTENT),
      extent_filepos_(0),
      lifetime_(Env::WLTH_NOT_SET),
      io_type_(IOType::kUnknown),
      file_size_(0),
      file_id_(file_id),
      nr_synced_extents_(0),
      m_time_(0),
      metadata_writer_(metadata_writer) {}

std::string ZoneFile::GetFilename() { return linkfiles_[0]; }
time_t ZoneFile::GetFileModificationTime() { return m_time_; }

uint64_t ZoneFile::GetFileSize() { return file_size_; }
uint64_t ZoneFile::GetFileSizeMeta() {
  if(file_size_meta_ == 0)
    file_size_meta_ = file_size_;
  return file_size_meta_;
}
void ZoneFile::SetFileSize(uint64_t sz) { file_size_ = sz; }
void ZoneFile::SetFileModificationTime(time_t mt) { m_time_ = mt; }
void ZoneFile::SetIOType(IOType io_type) { io_type_ = io_type; }

ZoneFile::~ZoneFile() { ClearExtents(); }

void ZoneFile::ClearExtents() {
  for (auto e = std::begin(extents_); e != std::end(extents_); ++e) {
    Zone* zone = (*e)->zone_;

    assert(zone && zone->used_capacity_ >= (*e)->length_);
    zone->used_capacity_ -= (*e)->length_;
    delete *e;
  }
  extents_.clear();
}

IOStatus ZoneFile::CloseActiveZone() {
  IOStatus s = IOStatus::OK();
  if (active_zone_) {
    bool full = active_zone_->IsFull();
    s = active_zone_->Close();
    ReleaseActiveZone();
    if (!s.ok()) {
      return s;
    }
    zbd_->PutOpenIOZoneToken();
    if (full) {
      zbd_->PutActiveIOZoneToken();
    }
  }
  return s;
}

void ZoneFile::AcquireWRLock() {
  open_for_wr_mtx_.lock();
  open_for_wr_ = true;
}

bool ZoneFile::TryAcquireWRLock() {
  if (!open_for_wr_mtx_.try_lock()) return false;
  open_for_wr_ = true;
  return true;
}

void ZoneFile::ReleaseWRLock() {
  assert(open_for_wr_);
  open_for_wr_ = false;
  open_for_wr_mtx_.unlock();
}

bool ZoneFile::IsOpenForWR() { return open_for_wr_; }

IOStatus ZoneFile::CloseWR() {
  CreateOrUpdateRecord();

  IOStatus s;
  /* Mark up the file as being closed */
  extent_start_ = NO_EXTENT;
  s = PersistMetadata();
  if (!s.ok()) return s;
  ReleaseWRLock();
  return CloseActiveZone();
}

IOStatus ZoneFile::PersistMetadata() {
  assert(metadata_writer_ != NULL);
  return metadata_writer_->Persist(this);
}

ZoneExtent* ZoneFile::GetExtent(uint64_t file_offset, uint64_t* dev_offset) {
  for (unsigned int i = 0; i < extents_.size(); i++) {
    if (file_offset < extents_[i]->length_) {
      *dev_offset = extents_[i]->start_ + file_offset;
      return extents_[i];
    } else {
      file_offset -= extents_[i]->length_;
    }
  }
  return NULL;
}

IOStatus ZoneFile::InvalidateCache(uint64_t pos, uint64_t size) {
  ReadLock lck(this);
  uint64_t offset = pos;
  uint64_t left = size;
  IOStatus s = IOStatus::OK();

  if (left == 0) {
    left = GetFileSize();
  }

  while (left) {
    uint64_t dev_offset;
    ZoneExtent* extent = GetExtent(offset, &dev_offset);

    if (!extent) {
      s = IOStatus::IOError("Extent not found while invalidating cache");
      break;
    }

    uint64_t extent_end = extent->start_ + extent->length_;
    uint64_t invalidate_size = std::min(left, extent_end - dev_offset);

    s = zbd_->InvalidateCache(dev_offset, invalidate_size);
    if (!s.ok()) break;

    left -= invalidate_size;
    offset += invalidate_size;
  }

  return s;
}

IOStatus ZoneFile::PositionedRead(uint64_t offset, size_t n, Slice* result,
                                  char* scratch, bool direct) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_READ_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportQPS(ZENFS_READ_QPS, 1);

  ReadLock lck(this);

  char* ptr;
  uint64_t r_off;
  size_t r_sz;
  ssize_t r = 0;
  size_t read = 0;
  ZoneExtent* extent;
  uint64_t extent_end;
  IOStatus s;

  if (offset >= file_size_) {
    *result = Slice(scratch, 0);
    return IOStatus::OK();
  }

  r_off = 0;
  extent = GetExtent(offset, &r_off);
  if (!extent) {
    /* read start beyond end of (synced) file data*/
    *result = Slice(scratch, 0);
    return s;
  }
  extent_end = extent->start_ + extent->length_;

  /* Limit read size to end of file */
  if ((offset + n) > file_size_)
    r_sz = file_size_ - offset;
  else
    r_sz = n;

  ptr = scratch;

  while (read != r_sz) {
    size_t pread_sz = r_sz - read;

    if ((pread_sz + r_off) > extent_end) pread_sz = extent_end - r_off;

    /* We may get some unaligned direct reads due to non-aligned extent lengths,
     * so increase read request size to be aligned to next blocksize boundary.
     */
    bool aligned = (pread_sz % zbd_->GetBlockSize() == 0);

    size_t bytes_to_align = 0;
    if (direct && !aligned) {
      bytes_to_align = zbd_->GetBlockSize() - (pread_sz % zbd_->GetBlockSize());
      pread_sz += bytes_to_align;
      aligned = true;
    }

    r = zbd_->Read(ptr, r_off, pread_sz, direct && aligned);
    if (r <= 0) break;

    /* Verify and update the the bytes read count (if read size was incremented,
     * for alignment purposes).
     */
    if ((size_t)r <= pread_sz - bytes_to_align)
      pread_sz = (size_t)r;
    else
      pread_sz -= bytes_to_align;

    ptr += pread_sz;
    read += pread_sz;
    r_off += pread_sz;

    if (read != r_sz && r_off == extent_end) {
      extent = GetExtent(offset + read, &r_off);
      if (!extent) {
        /* read beyond end of (synced) file data */
        break;
      }
      r_off = extent->start_;
      extent_end = extent->start_ + extent->length_;
    }
  }

  if (r < 0) {
    s = IOStatus::IOError("pread error\n");
    read = 0;
  }

  *result = Slice((char*)scratch, read);
  return s;
}

void ZoneFile::PushExtent() {
  uint64_t length;

  assert(file_size_ >= extent_filepos_);

  if (!active_zone_) return;

  length = file_size_ - extent_filepos_;
  if (length == 0) return;

  assert(length <= (active_zone_->wp_ - extent_start_));
  extents_.push_back(new ZoneExtent(extent_start_, length, active_zone_, level_));

  active_zone_->used_capacity_ += length;
  extent_start_ = active_zone_->wp_;
  extent_filepos_ = file_size_;
}

IOStatus ZoneFile::AllocateNewZone() {
  Zone* zone;
  IOStatus s = zbd_->AllocateIOZone(lifetime_, io_type_, &zone, std::shared_ptr<ZoneFile>(this));

  if (!s.ok()) return s;
  if (!zone) {
    return IOStatus::NoSpace("Zone allocation failure\n");
  }
  SetActiveZone(zone);
  extent_start_ = active_zone_->wp_;
  extent_filepos_ = file_size_;

  /* Persist metadata so we can recover the active extent using
     the zone write pointer in case there is a crash before syncing */
  return PersistMetadata();
}

/* Byte-aligned writes without a sparse header */
IOStatus ZoneFile::BufferedAppend(char* buffer, uint32_t data_size) {
  uint32_t left = data_size;
  uint32_t wr_size;
  uint32_t block_sz = GetBlockSize();
  IOStatus s;

  if (active_zone_ == NULL) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    wr_size = left;
    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;

    /* Pad to the next block boundary if needed */
    uint32_t align = wr_size % block_sz;
    uint32_t pad_sz = 0;

    if (align) pad_sz = block_sz - align;

    /* the buffer size s aligned on block size, so this is ok*/
    if (pad_sz) memset(buffer + wr_size, 0x0, pad_sz);

    uint64_t extent_length = wr_size;

    s = active_zone_->Append(buffer, wr_size + pad_sz);
    if (!s.ok()) return s;

    extents_.push_back(
        new ZoneExtent(extent_start_, extent_length, active_zone_, level_));

    extent_start_ = active_zone_->wp_;
    active_zone_->used_capacity_ += extent_length;
    file_size_ += extent_length;
    left -= extent_length;

    if (active_zone_->capacity_ == 0) {
      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }
      if (left) {
        memmove((void*)(buffer), (void*)(buffer + wr_size), left);
      }
      s = AllocateNewZone();
      if (!s.ok()) return s;
    }
  }

  return IOStatus::OK();
}

/* Byte-aligned, sparse writes with inline metadata
   the caller reserves 8 bytes of data for a size header */
IOStatus ZoneFile::SparseAppend(char* sparse_buffer, uint32_t data_size) {
  uint32_t left = data_size;
  uint32_t wr_size;
  uint32_t block_sz = GetBlockSize();
  IOStatus s;

  if (active_zone_ == NULL) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    wr_size = left + ZoneFile::SPARSE_HEADER_SIZE;
    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;

    /* Pad to the next block boundary if needed */
    uint32_t align = wr_size % block_sz;
    uint32_t pad_sz = 0;

    if (align) pad_sz = block_sz - align;

    /* the sparse buffer has block_sz extra bytes tail allocated for padding, so
     * this is safe */
    if (pad_sz) memset(sparse_buffer + wr_size, 0x0, pad_sz);

    uint64_t extent_length = wr_size - ZoneFile::SPARSE_HEADER_SIZE;
    EncodeFixed64(sparse_buffer, extent_length);

    s = active_zone_->Append(sparse_buffer, wr_size + pad_sz);
    if (!s.ok()) return s;

    extents_.push_back(
        new ZoneExtent(extent_start_ + ZoneFile::SPARSE_HEADER_SIZE,
                       extent_length, active_zone_, level_));

    extent_start_ = active_zone_->wp_;
    active_zone_->used_capacity_ += extent_length;
    file_size_ += extent_length;
    left -= extent_length;

    if (active_zone_->capacity_ == 0) {
      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }
      if (left) {
        memmove((void*)(sparse_buffer + ZoneFile::SPARSE_HEADER_SIZE),
                (void*)(sparse_buffer + wr_size), left);
      }
      s = AllocateNewZone();
      if (!s.ok()) return s;
    }
  }

  return IOStatus::OK();
}

/* Assumes that data and size are block aligned */
IOStatus ZoneFile::Append(void* data, int data_size) {
  uint32_t left = data_size;
  uint32_t wr_size, offset = 0;
  IOStatus s = IOStatus::OK();

  if (!active_zone_) {
    s = AllocateNewZone();
    if (!s.ok()) return s;
  }

  while (left) {
    if (active_zone_->capacity_ == 0) {
      PushExtent();

      s = CloseActiveZone();
      if (!s.ok()) {
        return s;
      }

      s = AllocateNewZone();
      if (!s.ok()) return s;
    }

    wr_size = left;
    if (wr_size > active_zone_->capacity_) wr_size = active_zone_->capacity_;

    s = active_zone_->Append((char*)data + offset, wr_size);
    if (!s.ok()) return s;

    file_size_ += wr_size;
    left -= wr_size;
    offset += wr_size;
  }

  return IOStatus::OK();
}

IOStatus ZoneFile::RecoverSparseExtents(uint64_t start, uint64_t end,
                                        Zone* zone) {
  /* Sparse writes, we need to recover each individual segment */
  IOStatus s;
  uint32_t block_sz = GetBlockSize();
  uint64_t next_extent_start = start;
  char* buffer;
  int recovered_segments = 0;
  int ret;

  ret = posix_memalign((void**)&buffer, sysconf(_SC_PAGESIZE), block_sz);
  if (ret) {
    return IOStatus::IOError("Out of memory while recovering");
  }

  while (next_extent_start < end) {
    uint64_t extent_length;

    ret = zbd_->Read(buffer, next_extent_start, block_sz, false);
    if (ret != (int)block_sz) {
      s = IOStatus::IOError("Unexpected read error while recovering");
      break;
    }

    extent_length = DecodeFixed64(buffer);
    if (extent_length == 0) {
      s = IOStatus::IOError("Unexpected extent length while recovering");
      break;
    }
    recovered_segments++;

    zone->used_capacity_ += extent_length;
    extents_.push_back(new ZoneExtent(next_extent_start + SPARSE_HEADER_SIZE,
                                      extent_length, zone, level_));

    uint64_t extent_blocks = (extent_length + SPARSE_HEADER_SIZE) / block_sz;
    if ((extent_length + SPARSE_HEADER_SIZE) % block_sz) {
      extent_blocks++;
    }
    next_extent_start += extent_blocks * block_sz;
  }

  free(buffer);
  return s;
}

IOStatus ZoneFile::Recover() {
  /* If there is no active extent, the file was either closed gracefully
     or there were no writes prior to a crash. All good.*/
  if (!HasActiveExtent()) return IOStatus::OK();

  /* Figure out which zone we were writing to */
  Zone* zone = zbd_->GetIOZone(extent_start_);

  if (zone == nullptr) {
    return IOStatus::IOError(
        "Could not find zone for extent start while recovering");
  }

  if (zone->wp_ < extent_start_) {
    return IOStatus::IOError("Zone wp is smaller than active extent start");
  }

  /* How much data do we need to recover? */
  uint64_t to_recover = zone->wp_ - extent_start_;

  /* Do we actually have any data to recover? */
  if (to_recover == 0) {
    /* Mark up the file as having no missing extents */
    extent_start_ = NO_EXTENT;
    return IOStatus::OK();
  }

  /* Is the data sparse or was it writted direct? */
  if (is_sparse_) {
    IOStatus s = RecoverSparseExtents(extent_start_, zone->wp_, zone);
    if (!s.ok()) return s;
  } else {
    /* For non-sparse files, the data is contigous and we can recover directly
       any missing data using the WP */
    zone->used_capacity_ += to_recover;
    extents_.push_back(new ZoneExtent(extent_start_, to_recover, zone, level_));
  }

  /* Mark up the file as having no missing extents */
  extent_start_ = NO_EXTENT;

  /* Recalculate file size */
  file_size_ = 0;
  for (uint32_t i = 0; i < extents_.size(); i++) {
    file_size_ += extents_[i]->length_;
  }

  return IOStatus::OK();
}

void ZoneFile::ReplaceExtentList(std::vector<ZoneExtent*> new_list) {
  assert(IsOpenForWR() && new_list.size() > 0);
  assert(new_list.size() == extents_.size());

  WriteLock lck(this);
  extents_ = new_list;
}

void ZoneFile::AddLinkName(const std::string& linkf) {
  linkfiles_.push_back(linkf);
}

IOStatus ZoneFile::RenameLink(const std::string& src, const std::string& dest) {
  auto itr = std::find(linkfiles_.begin(), linkfiles_.end(), src);
  if (itr != linkfiles_.end()) {
    linkfiles_.erase(itr);
    linkfiles_.push_back(dest);
  } else {
    return IOStatus::IOError("RenameLink: Failed to find the linked file");
  }
  return IOStatus::OK();
}

IOStatus ZoneFile::RemoveLinkName(const std::string& linkf) {
  assert(GetNrLinks());
  auto itr = std::find(linkfiles_.begin(), linkfiles_.end(), linkf);
  if (itr != linkfiles_.end()) {
    linkfiles_.erase(itr);
  } else {
    return IOStatus::IOError("RemoveLinkInfo: Failed to find the link file");
  }
  return IOStatus::OK();
}

IOStatus ZoneFile::SetWriteLifeTimeHint(Env::WriteLifeTimeHint lifetime) {
  lifetime_ = lifetime;
  return IOStatus::OK();
}

void ZoneFile::SetLevel(int level) {
  level_ = level;
  for(const auto& ext: extents_)
    ext->level_ = level;
}

void ZoneFile::UpdateInternalKeys(const Slice& key, SequenceNumber seqno) {
  if (smallest_.size() == 0) {
    smallest_.DecodeFrom(key);
  }
  largest_.DecodeFrom(key);
  has_keys_ = true;

  smallest_seqno_ = std::min(smallest_seqno_, seqno);
  largest_seqno_ = std::max(largest_seqno_, seqno);
}

void ZoneFile::UpdateInternalKeysRange(const Slice& start, const Slice& end, SequenceNumber seqno, const CompareInterface* icmp) {
  if (smallest_.size() == 0 || icmp->Compare(start, smallest_.Encode()) < 0) {
    smallest_.DecodeFrom(start);
  }
  if (largest_.size() == 0 || icmp->Compare(largest_.Encode(), end) < 0) {
    largest_.DecodeFrom(end);
  }
  has_keys_ = true;

  smallest_seqno_ = std::min(smallest_seqno_, seqno);
  largest_seqno_ = std::max(largest_seqno_, seqno);
}

void ZoneFile::UpdateMetadata(const TableProperties& table_properties) {
  num_entries_ = table_properties.num_entries;
  num_deletions_ = table_properties.num_deletions;
  num_range_deletions_ = table_properties.num_range_deletions;
}

void ZoneFile::UpdateMetadata(uint64_t num_entries, uint64_t num_deletions, uint64_t num_range_deletions, uint64_t file_size, uint64_t compensated_range_deletion_size, uint64_t average_value_size) {
  num_entries_ = num_entries;
  num_deletions_ = num_deletions;
  num_range_deletions_ = num_range_deletions;
  file_size_meta_ = std::max(file_size_meta_, file_size_);
  compensated_range_deletion_size_ = std::max(compensated_range_deletion_size_, compensated_range_deletion_size);

  if(average_value_size == 0) return;
  std::lock_guard<std::mutex> lk(zbd_->levelwise_files_mtx_);
  zbd_->zenfs_parameters_.average_value_size = average_value_size;
}

void ZoneFile::SetInternalComparator(CompareInterface* icmp) {
  InternalKeyComparator* icmp_ptr = dynamic_cast<InternalKeyComparator*>(icmp);
  this->icmp_ = *icmp_ptr;
}

void ZoneFile::ReleaseActiveZone() {
  assert(active_zone_ != nullptr);
  bool ok = active_zone_->Release();
  assert(ok);
  (void)ok;
  active_zone_ = nullptr;
}

void ZoneFile::SetActiveZone(Zone* zone) {
  assert(active_zone_ == nullptr);
  assert(zone->IsBusy());
  active_zone_ = zone;
}

void ZoneFile::CreateOrUpdateRecord() {
  if(level_ < 0)
    return;
  if(is_recorded_)
    zbd_->RemoveZoneFileRecord(std::shared_ptr<ZoneFile>(this));
  zbd_->AddZoneFileRecord(std::shared_ptr<ZoneFile>(this));
  is_recorded_ = true;
}

void ZoneFile::ComputeCompensatedSize() {
  uint64_t average_value_size = zbd_->zenfs_parameters_.average_value_size;
  if(file_size_meta_ == 0)
    file_size_meta_ = file_size_;
  compensated_file_size_ = file_size_meta_;
  
  if ((num_deletions_ - num_range_deletions_) * 2 >= num_entries_) {
    compensated_file_size_ +=
        ((num_deletions_ - num_range_deletions_) * 2 - num_entries_) *
        average_value_size * 2;
  }
  compensated_file_size_ += compensated_range_deletion_size_;
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
