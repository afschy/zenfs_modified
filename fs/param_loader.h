# pragma once
#include <fstream>
#include <string>
#include <unordered_map>
namespace ROCKSDB_NAMESPACE {

enum EmptyZoneAllocType { // When an empty zone is needed, how is it chosen from all available zones?
  kDefault,
  kSequential,
  kRandom,
  kLeastWear,
  kHotnessBased
};

enum MappingPolicyType {  // In which zone to put a newly created file
  kLifetimeBased, // ZenFS default, no change
  kCAZA, // Files are put in the same zones as other files in neighboring level with overlapping keyranges
  kSameLevelNearbyKeys, // ZoneKV
  kArrivalTimeBased, // Files are put in order of arrival, greedily filling up available zones
  kTombstoneDensity, // Files having tombstone densities higher than a threshold go to a dedicated zone
  kTombstoneTTL, // Files having similar TTL should go to the same zones
  kClusterTogether // Try to put everything in the same zone
};

struct ZenfsParamContainer {
  EmptyZoneAllocType empty_zone_allocator = kSequential;
  
  uint8_t max_level = 1;
  uint8_t min_boundary = 1; // levels <= this will follow a special policy
  uint8_t max_boundary = 1; // levels >= this will follow a special policy

  MappingPolicyType upper_level_policy = kClusterTogether; // for levels <= min_boundary
  MappingPolicyType lower_level_policy = kSameLevelNearbyKeys; // for levels >= max_boundary
  MappingPolicyType middle_level_policy = kCAZA; // for all other levels

  uint64_t gc_start_level = 20; // GC kicks in when free space is lower than this percentage
  uint64_t gc_slope = 3; // GC aggresiveness

  void LoadParamsFromFile() {
    std::ifstream infile("../params.txt");
    std::string type, value;

    while(infile >> type >> value) {
      static std::unordered_map<std::string, EmptyZoneAllocType>
      empty_zone_allocator_map = {
        {"kDefault", kDefault},
        {"kSequential", kSequential},
        {"kRandom", kRandom},
        {"kLeastWear", kLeastWear},
        {"kHotnessBased", kHotnessBased}
      };

      static std::unordered_map<std::string, MappingPolicyType>
      mapping_policy_map = {
        {"kLifetimeBased", kLifetimeBased},
        {"kCAZA", kCAZA},
        {"kSameLevelNearbyKeys", kSameLevelNearbyKeys},
        {"kArrivalTimeBased", kArrivalTimeBased},
        {"kTombstoneDensity", kTombstoneDensity},
        {"kTombstoneTTL", kTombstoneTTL},
        {"kClusterTogether", kClusterTogether}
      };

      if(type == "empty_zone_allocator" && empty_zone_allocator_map.find(value) != empty_zone_allocator_map.end())
        empty_zone_allocator = empty_zone_allocator_map[value];
      
      else if(type == "upper_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        upper_level_policy = mapping_policy_map[value];

      else if(type == "lower_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        lower_level_policy = mapping_policy_map[value];

      else if(type == "middle_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        middle_level_policy = mapping_policy_map[value];

      else if(type == "gc_start_level") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 100)
          gc_start_level = value_int;
      }

      else if(type == "gc_slope") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 100)
          gc_slope = value_int;
      }
    }
  }
};



}   // namespace ROCKSDB_NAMESPACE