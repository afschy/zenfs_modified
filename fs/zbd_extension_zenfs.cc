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
namespace ROCKSDB_NAMESPACE {

IOStatus ZonedBlockDevice::AllocateEmptyZoneDefault(Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;

    if (z->IsEmpty()) {
      allocated_zone = z;
      break;
    } 
    else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }
  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZoneSequential(Zone **zone_out) {
  static int zone_pos = 0;
  int curr_traversed = 0;
  
  IOStatus s;
  Zone *allocated_zone = nullptr;
  while (curr_traversed < io_zones.size()) {
    Zone* z = io_zones[zone_pos];
    curr_traversed++;
    zone_pos++;
    if(zone_pos >= io_zones.size())
      zone_pos = 0;

    if (!z->Acquire())
      continue;

    if (z->IsEmpty()) {
      allocated_zone = z;
      break;
    } 
    else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }
  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZoneLeastWear(Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  
  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;

    if(!z->IsEmpty()) {
      s = z->CheckRelease();
      if (!s.ok()) return s;
      continue;
    }

    // Chose this zone if no zone has been chosen yet
    if (allocated_zone == nullptr) {
      allocated_zone = z;
      continue;
    }

    // Skip this zone if it's not better than the already chosen zone
    if(allocated_zone->reset_count_ < z->reset_count_) {
      s = z->CheckRelease();
      if (!s.ok()) return s;
      continue;
    }

    // A zone with a smaller reset count is found
    s = allocated_zone->CheckRelease();
    if (!s.ok()) return s;
    allocated_zone = z;
  }

  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZoneRandom(Zone **zone_out) {
  std::vector<Zone*> shuffled_io_zones(io_zones);
  static auto rng = std::default_random_engine {};
  std::shuffle(std::begin(shuffled_io_zones), std::end(shuffled_io_zones), rng);

  IOStatus s;
  Zone *allocated_zone = nullptr;
  for (const auto z : shuffled_io_zones) {
    if (!z->Acquire())
      continue;

    if (z->IsEmpty()) {
      allocated_zone = z;
      break;
    } 
    else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }
  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZoneHotnessBased(Zone **zone_out, int level) {
  if (level <= 0)
    return AllocateEmptyZoneLeastWear(zone_out);

  std::vector<Zone *> sorted_io_zones(io_zones);
  std::sort(sorted_io_zones.begin(), sorted_io_zones.end(), [](const Zone* &a, const Zone* &b)
            { return a->reset_count_ < b->reset_count_; });

  // divide the sorted list of zones into max_level number of intervals
  // each possible level gets its own "perfect" interval
  double interval = 1.00 * sorted_io_zones.size() / zenfs_parameters_.max_level;
  int lower_index = interval * (level-1), upper_index = ceil(interval * level);
  lower_index = std::max(lower_index, 0);
  upper_index = std::min(upper_index, int(sorted_io_zones.size()));

  // try to find a zone in the perfect interval
  IOStatus s;
  Zone* allocated_zone = nullptr;
  for (int i = lower_index; i < upper_index; i++) {
    Zone* z = sorted_io_zones[i];
    if (!z->Acquire())
      continue;

    if(z->IsEmpty()) {
      allocated_zone = z;
      break;
    }

    s = z->CheckRelease();
    if(!s.ok()) return s;
  }

  if (allocated_zone != nullptr) {
    *zone_out = allocated_zone;
    return IOStatus::OK();
  }

  // failed to find a zone in the perfect interval
  // try to find a zone closest to the perfect interval
  Zone* upper_alloc = nullptr;
  int upper_alloc_index = sorted_io_zones.size()-1;
  
  for (int i = upper_index; i < sorted_io_zones.size(); i++) {
    Zone* z = sorted_io_zones[i];
    if (!z->Acquire())
      continue;

    if(z->IsEmpty()) {
      upper_alloc = z;
      upper_alloc_index = i;
      break;
    }

    s = z->CheckRelease();
    if(!s.ok()) return s;
  }

  Zone* lower_alloc = nullptr;
  int lower_alloc_index = 0;
  
  for (int i = lower_index-1; i >= 0; i--) {
    Zone* z = sorted_io_zones[i];
    if (!z->Acquire())
      continue;

    if(z->IsEmpty()) {
      lower_alloc = z;
      lower_alloc_index = i;
      break;
    }

    s = z->CheckRelease();
    if(!s.ok()) return s;
  }

  // failed, fallback to default
  if(lower_alloc == nullptr && upper_alloc == nullptr)
    return AllocateEmptyZoneDefault(zone_out);
  
  if (lower_alloc == nullptr)
    *zone_out = upper_alloc;
  else if (upper_alloc == nullptr)
    *zone_out = lower_alloc;
  // lower_alloc is closer to the perfect range than upper_alloc
  else if (lower_index - lower_alloc_index < upper_alloc_index - upper_index) {
    *zone_out = lower_alloc;
    upper_alloc->CheckRelease();
  }
  // upper_alloc is closer to the perfect range than lower_alloc
  else {
    *zone_out = upper_alloc;
    lower_alloc->CheckRelease();
  }

  return IOStatus::OK();
}

void ZonedBlockDevice::AddZoneFileRecord(std::shared_ptr<ZoneFile> zonefile_ptr) {
  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  
  if(!zonefile_ptr->has_keys_) return;
  if(zonefile_ptr->level_ < 0) return;
  if(zonefile_ptr->GetIOType() != IOType::kData) return;    // not an SST file
  
  int level = zonefile_ptr->level_;
  if(level >= levelwise_file_list_.size())
    levelwise_file_list_.resize(level + 1);

  for(auto it=levelwise_file_list_[level].begin(); it != levelwise_file_list_[level].end(); ++it) {
    // look for a file whose smallest key is larger than the new file's smallest key
    if (zonefile_ptr->icmp_.Compare(zonefile_ptr->smallest_, (*it)->smallest_) >= 0)
      continue;
    
    levelwise_file_list_[level].insert(it, zonefile_ptr);
    return;
  }

  // largest key in this level
  levelwise_file_list_[level].emplace_back(zonefile_ptr);
}

void ZonedBlockDevice::RemoveZoneFileRecord(std::shared_ptr<ZoneFile> zonefile_ptr) {
  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  for(int i=0; i<levelwise_file_list_.size(); i++)
    levelwise_file_list_[i].remove(zonefile_ptr);
}

void ZonedBlockDevice::RecomputeCompensatedFileSizes() {
  for(int i=0; i<levelwise_file_list_.size(); i++)
    for(auto it=levelwise_file_list_[i].begin(); it != levelwise_file_list_[i].end(); ++it)
      (*it)->ComputeCompensatedSize();
}

void ZonedBlockDevice::GetOverlappingFiles(
    std::vector<std::shared_ptr<ZoneFile>>& ret_container,
    std::shared_ptr<ZoneFile> zonefile_ptr,
    int target_level) {
  if(target_level < 0 || target_level >= levelwise_file_list_.size())
    return;

  for(auto it=levelwise_file_list_[target_level].begin(); it != levelwise_file_list_[target_level].end(); ++it) {
    if(zonefile_ptr != *it && IsOverlapping(zonefile_ptr, *it, zonefile_ptr->icmp_))
      ret_container.emplace_back(*it);
  }
}

uint64_t ZonedBlockDevice::GetOverlapCount(std::shared_ptr<ZoneFile> zonefile_ptr, int target_level) {
  uint64_t count = 0;
  if(target_level < 0 || target_level >= levelwise_file_list_.size())
    return count;

  for(auto it=levelwise_file_list_[target_level].begin(); it != levelwise_file_list_[target_level].end(); ++it) {
    if(zonefile_ptr != *it && IsOverlapping(zonefile_ptr, *it, zonefile_ptr->icmp_))
      count += (*it)->GetFileSizeMeta();
  }

  return count;
}

IOStatus
ZonedBlockDevice::MatchLifetimeBased(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {

  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  // choose the zone with the lowest lifetime difference with the given file
  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
      if (diff <= best_diff) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        best_diff = diff;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchCAZA(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  
  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  int level = zonefile->level_;

  std::vector<std::shared_ptr<ZoneFile>> overlapping_files;
  if(GetPolicy(level-1) == kCAZA || GetSecondaryPolicy(level-1) == kCAZA)
    GetOverlappingFiles(overlapping_files, zonefile, level-1);
  if(GetPolicy(level+1) == kCAZA || GetSecondaryPolicy(level+1) == kCAZA)
    GetOverlappingFiles(overlapping_files, zonefile, level+1);
  
  // no overlapping files yet, can't apply CAZA
  if(!overlapping_files.size()) {
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
    return IOStatus::OK();
  }

  std::map<uint64_t, uint64_t> overlap_map;
  GetPerZoneContribution(overlapping_files, overlap_map);

  Zone* allocated_zone = nullptr;
  uint64_t highest_contribution = 0;
  IOStatus s;

  // allocate the zone with the highest number of overlapping bytes
  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      uint64_t contribution = overlap_map[z->start_];
      if (contribution > highest_contribution) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        highest_contribution = contribution;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  if(allocated_zone == nullptr)
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  else {
    *best_diff_out = 0;
    *zone_out = allocated_zone;
  }

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchSameLevelNearbyKeys(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  
  int level = zonefile->level_;
  *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  // level doesn't exist
  if(level < 0 || level >= levelwise_file_list_.size())
    return IOStatus::OK();

  // find the correct spot for the given file
  auto it_middle = levelwise_file_list_[level].begin();
  int index_middle = 0;
  while(it_middle != levelwise_file_list_[level].end()) {
    if(zonefile->icmp_.Compare(zonefile->smallest_, (*it_middle)->smallest_) >= 0)
      break;
    it_middle++;
    index_middle++;
  }

  std::map<uint64_t, uint64_t> contribution_map;
  
  auto it_forward = it_middle;
  double factor = 1.0;
  for(int i = index_middle; i<levelwise_file_list_[level].size(); i++) {
    if(*it_forward == zonefile) continue;
    GetPerZoneContribution(*it_forward, contribution_map, factor);
    factor *= 0.8;  // the farther the file is from the perfect spot, the less important it becomes
    if(factor < 0.2)
      factor = 0.2;
    it_forward++;
  }

  auto it_back = it_middle;
  it_back--;
  factor = 1.0;
  for(int i = index_middle-1; i >= 0; i--) {
    GetPerZoneContribution(*it_back, contribution_map, factor);
    factor *= 0.8;  // the farther the file is from the perfect spot, the less important it becomes
    if(factor < 0.2)
      factor = 0.2;
    it_back--;
  }

  Zone* allocated_zone = nullptr;
  uint64_t highest_contribution = 0;
  IOStatus s;

  // choose the zone with the highest weighted contribution from same-level files
  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      uint64_t contribution = contribution_map[z->start_];
      if (contribution > highest_contribution) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        highest_contribution = contribution;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  if(allocated_zone == nullptr)
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  else {
    *best_diff_out = 0;
    *zone_out = allocated_zone;
  }

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchArrivalTimeBased(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      
      if (z->level_ == zonefile->level_ && z->policy_ == kArrivalTimeBased) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        best_diff = 0;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchTombstoneDensity(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  
  // this file is unfit for this policy, transfer responsibility to fallback/default policy
  if(!IsHighTombstone(zonefile)) {
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
    *zone_out = nullptr;
    return IOStatus::OK();
  }
  
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      MappingPolicyType policy = z->policy_;
      if (policy == kTombstoneDensity) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        best_diff = 0;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchTombstoneTTL(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  // placeholder
  return MatchLifetimeBased(zonefile, file_lifetime, best_diff_out, zone_out, min_capacity);
}

IOStatus
ZonedBlockDevice::MatchClusterTogether(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      MappingPolicyType policy = z->policy_;
      if (policy == kClusterTogether) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        best_diff = 0;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchOverlapChildren(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {

  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  
  zonefile->ComputeCompensatedSize();
  RecomputeCompensatedFileSizes();

  int level = zonefile->level_;
  // trivial moves can happen, simulating those
  while(level < zenfs_parameters_.max_level) {
    if(GetOverlapCount(zonefile, level+1))
      break;
    level++;
  }

  std::vector<std::shared_ptr<ZoneFile>> own_level_ranking;
  // copy levelwise_file_list_[level]
  own_level_ranking.push_back(zonefile);
  for(auto it = levelwise_file_list_[level].begin(); it != levelwise_file_list_[level].end(); ++it) {
    if((*it) == zonefile) continue;
    own_level_ranking.emplace_back(*it);
  }
  // compute ranking based on overlap (lower overlap -> higher ranking)
  OverlapRankHelper(own_level_ranking, level, level+1);

  // also compute the ranking of upper level files
  std::vector<std::shared_ptr<ZoneFile>> upper_level_ranking;
  if(level-1 > 0) {
    for(auto it = levelwise_file_list_[level-1].begin(); it != levelwise_file_list_[level-1].end(); ++it) {
      if((*it) == zonefile) continue;
      upper_level_ranking.emplace_back(*it);
    }
  }
  OverlapRankHelper(upper_level_ranking, level-1, level);

  // A file can participate in compaction in two ways:
  // 1. The file is chosen for compaction
  // 2. The file is victim of a compaction from the upper level
  // So, the ranking of the overlapping files in the upper level is also taken into account for each file
  for(int i=0; i<own_level_ranking.size(); i++) {
    for(int j=0; j<upper_level_ranking.size(); j++) {
      if(IsOverlapping(own_level_ranking[i], upper_level_ranking[j], zonefile->icmp_)) {
        own_level_ranking[i]->rank_ += upper_level_ranking[j]->rank_;
        break;
      }
    }
  }

  std::sort(own_level_ranking.begin(), own_level_ranking.end(), [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right){
    return left->rank_ < right->rank_;
  });

  // get the ranking index of the new file
  int zonefile_index = 0;
  while(zonefile_index < own_level_ranking.size()) {
    if(own_level_ranking[zonefile_index] == zonefile)
      break;
    zonefile_index++;
  }

  // get the amount of data from closely-ranked files held by each zone
  std::map<uint64_t, uint64_t> contribution_map;
  double factor = 1.0;  // closer files have more weight than farther ones
  for(int i = zonefile_index+1; i < own_level_ranking.size(); i++) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  factor = 1.0;
  for(int i = zonefile_index-1; i >= 0; i--) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  Zone* allocated_zone = nullptr;
  uint64_t highest_contribution = 0;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      uint64_t contribution = contribution_map[z->start_];
      if (contribution > highest_contribution) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        highest_contribution = contribution;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  if(allocated_zone == nullptr)
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  else {
    *best_diff_out = 0;
    *zone_out = allocated_zone;
  }

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchOverlapGrandchildren(
    std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {

  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  
  zonefile->ComputeCompensatedSize();
  RecomputeCompensatedFileSizes();

  int level = zonefile->level_;
  while(level < zenfs_parameters_.max_level) {
    if(GetOverlapCount(zonefile, level+1))
      break;
    level++;
  }

  std::vector<std::shared_ptr<ZoneFile>> own_level_ranking;
  own_level_ranking.push_back(zonefile);
  for(auto it = levelwise_file_list_[level].begin(); it != levelwise_file_list_[level].end(); ++it) {
    if((*it) == zonefile) continue;
    own_level_ranking.emplace_back(*it);
  }
  OverlapRankHelper(own_level_ranking, level, level+2);

  std::vector<std::shared_ptr<ZoneFile>> upper_level_ranking;
  if(level-1 > 0) {
    for(auto it = levelwise_file_list_[level-1].begin(); it != levelwise_file_list_[level-1].end(); ++it) {
      if((*it) == zonefile) continue;
      upper_level_ranking.emplace_back(*it);
    }
  }
  OverlapRankHelper(upper_level_ranking, level-1, level+1);

  for(int i=0; i<own_level_ranking.size(); i++) {
    for(int j=0; j<upper_level_ranking.size(); j++) {
      if(IsOverlapping(own_level_ranking[i], upper_level_ranking[j], zonefile->icmp_)) {
        own_level_ranking[i]->rank_ += upper_level_ranking[j]->rank_;
        break;
      }
    }
  }

  std::sort(own_level_ranking.begin(), own_level_ranking.end(), [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right){
    return left->rank_ < right->rank_;
  });

  int zonefile_index = 0;
  while(zonefile_index < own_level_ranking.size()) {
    if(own_level_ranking[zonefile_index] == zonefile)
      break;
    zonefile_index++;
  }

  std::map<uint64_t, uint64_t> contribution_map;
  double factor = 1.0;
  for(int i = zonefile_index+1; i < own_level_ranking.size(); i++) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  factor = 1.0;
  for(int i = zonefile_index-1; i >= 0; i--) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  Zone* allocated_zone = nullptr;
  uint64_t highest_contribution = 0;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      uint64_t contribution = contribution_map[z->start_];
      if (contribution > highest_contribution) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        highest_contribution = contribution;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  if(allocated_zone == nullptr)
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  else {
    *best_diff_out = 0;
    *zone_out = allocated_zone;
  }

  return IOStatus::OK();
}

IOStatus
ZonedBlockDevice::MatchCompensatedSize(std::shared_ptr<ZoneFile> zonefile,
    Env::WriteLifeTimeHint file_lifetime,
    unsigned int *best_diff_out, Zone **zone_out,
    uint32_t min_capacity = 0) {

  std::lock_guard<std::mutex> lock(levelwise_files_mtx_);
  
  zonefile->ComputeCompensatedSize();
  RecomputeCompensatedFileSizes();

  int level = zonefile->level_;
  // trivial moves can happen, simulating those
  while(level < zenfs_parameters_.max_level) {
    if(GetOverlapCount(zonefile, level+1))
      break;
    level++;
  }

  std::vector<std::shared_ptr<ZoneFile>> own_level_ranking;
  // copy levelwise_file_list_[level]
  own_level_ranking.push_back(zonefile);
  for(auto it = levelwise_file_list_[level].begin(); it != levelwise_file_list_[level].end(); ++it) {
    if((*it) == zonefile) continue;
    own_level_ranking.emplace_back(*it);
  }
  // compute ranking based on compensated size (higher size -> ranking closer to 0)
  std::sort(own_level_ranking.begin(), own_level_ranking.end(),
      [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right) {
    return left->compensated_file_size_ > right->compensated_file_size_;
  });
  for(int i=0; i<own_level_ranking.size(); i++)
    own_level_ranking[i]->rank_ = i+1;

  // also compute the ranking of upper level files
  std::vector<std::shared_ptr<ZoneFile>> upper_level_ranking;
  if(level-1 > 0) {
    for(auto it = levelwise_file_list_[level-1].begin(); it != levelwise_file_list_[level-1].end(); ++it) {
      if((*it) == zonefile) continue;
      upper_level_ranking.emplace_back(*it);
    }
  }
  std::sort(upper_level_ranking.begin(), upper_level_ranking.end(),
      [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right) {
    return left->compensated_file_size_ > right->compensated_file_size_;
  });
  for(int i=0; i<upper_level_ranking.size(); i++)
    upper_level_ranking[i]->rank_ = i+1;

  // A file can participate in compaction in two ways:
  // 1. The file is chosen for compaction
  // 2. The file is victim of a compaction from the upper level
  // So, the ranking of the overlapping files in the upper level is also taken into account for each file
  for(int i=0; i<own_level_ranking.size(); i++) {
    for(int j=0; j<upper_level_ranking.size(); j++) {
      if(IsOverlapping(own_level_ranking[i], upper_level_ranking[j], zonefile->icmp_)) {
        own_level_ranking[i]->rank_ += upper_level_ranking[j]->rank_;
        break;
      }
    }
  }

  std::sort(own_level_ranking.begin(), own_level_ranking.end(), [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right){
    return left->rank_ < right->rank_;
  });

  // get the ranking index of the new file
  int zonefile_index = 0;
  while(zonefile_index < own_level_ranking.size()) {
    if(own_level_ranking[zonefile_index] == zonefile)
      break;
    zonefile_index++;
  }

  // get the amount of data from closely-ranked files held by each zone
  std::map<uint64_t, uint64_t> contribution_map;
  double factor = 1.0;  // closer files have more weight than farther ones
  for(int i = zonefile_index+1; i < own_level_ranking.size(); i++) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  factor = 1.0;
  for(int i = zonefile_index-1; i >= 0; i--) {
    GetPerZoneContribution(own_level_ranking[i], contribution_map, factor);
    factor *= 0.5;
  }

  Zone* allocated_zone = nullptr;
  uint64_t highest_contribution = 0;
  IOStatus s;

  for (const auto z : io_zones) {
    if (!z->Acquire())
      continue;
      
    if ((z->used_capacity_ > 0) && !z->IsFull() &&
        z->capacity_ >= min_capacity) {
      uint64_t contribution = contribution_map[z->start_];
      if (contribution > highest_contribution) {
        if (allocated_zone != nullptr) {
          s = allocated_zone->CheckRelease();
          if (!s.ok()) {
            IOStatus s_ = z->CheckRelease();
            if (!s_.ok()) return s_;
            return s;
          }
        }
        allocated_zone = z;
        highest_contribution = contribution;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    } else {
      s = z->CheckRelease();
      if (!s.ok()) return s;
    }
  }

  if(allocated_zone == nullptr)
    *best_diff_out = LIFETIME_DIFF_NOT_GOOD;
  else {
    *best_diff_out = 0;
    *zone_out = allocated_zone;
  }

  return IOStatus::OK();  
}

void ZonedBlockDevice::GetPerZoneContribution(
    std::vector<std::shared_ptr<ZoneFile>>& files,
    std::map<uint64_t, uint64_t>& result,
    double factor=1.0) {
  
  for(int i=0; i<files.size(); i++)
    GetPerZoneContribution(files[i], result, factor);
}

void ZonedBlockDevice::GetPerZoneContribution(
    std::shared_ptr<ZoneFile> zonefile,
    std::map<uint64_t, uint64_t>& result,
    double factor=1.0) {
  
  ZoneFileSnapshot snapshot(*zonefile);
  std::vector<ZoneExtentSnapshot> extents = snapshot.extents;

  for(int j=0; j<extents.size(); j++) {
    ZoneExtentSnapshot curr_extent = extents[j];
    if(result.count(curr_extent.zone_start) == 0)
      result[curr_extent.zone_start] = 0;
    result[curr_extent.zone_start] += factor * curr_extent.length;
  }
}

void ZonedBlockDevice::OverlapRankHelper(
    std::vector<std::shared_ptr<ZoneFile>>& container,
    int own_level, int target_level) {
  
  if(own_level < 0 || own_level >= levelwise_file_list_.size() || target_level < 0 || target_level >= levelwise_file_list_.size()) {
    for(int i=0; i<container.size(); i++)
      container[i]->rank_ = 0;
    return;
  }
  
  for(int i=0; i<container.size(); i++) {
    uint64_t count = GetOverlapCount(container[i], target_level);
    container[i]->rank_ = count * 1024U / container[i]->compensated_file_size_;
  }

  std::sort(container.begin(), container.end(), [](std::shared_ptr<ZoneFile>& left, std::shared_ptr<ZoneFile>& right){
    return left->rank_ < right->rank_;
  });
  for(int i=0; i<container.size(); i++)
    container[i]->rank_ = i+1;
}

bool ZonedBlockDevice::IsOverlapping(
    std::shared_ptr<ZoneFile> a,
    std::shared_ptr<ZoneFile> b,
    InternalKeyComparator& icmp) {
  
  if(icmp.Compare(a->largest_, b->smallest_) < 0) return false;
  if(icmp.Compare(a->smallest_, b->largest_) > 0) return false;
  return true;
}

bool ZonedBlockDevice::IsHighTombstone(std::shared_ptr<ZoneFile> zonefile_ptr) {
  double delete_ratio = 0;
  if(zonefile_ptr->num_entries_)
    delete_ratio = 1.00 * zonefile_ptr->num_deletions_ / zonefile_ptr->num_entries_;
  
  if(delete_ratio >= zenfs_parameters_.tombstone_density)
    return true;
  return false;
}

}  // namespace ROCKSDB_NAMESPACE
#endif