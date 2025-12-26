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
  kClusterTogether, // Try to put everything in the same zone
  kOverlapChildren, // Rank based mapping based on overlap with level-i+1
  kOverlapGrandchildren, // Rank based mapping based on overlap with level-i+2
};

struct ZenfsParamContainer {
  EmptyZoneAllocType empty_zone_allocator = kSequential;
  
  uint8_t max_level = 1;
  uint8_t min_boundary = 1; // levels <= this will follow a special policy
  uint8_t max_boundary = 4; // levels >= this will follow a special policy

  double tombstone_density = 0.5;

  MappingPolicyType upper_level_policy = kClusterTogether; // for levels <= min_boundary
  MappingPolicyType upper_level_policy_fallback = kLifetimeBased;

  MappingPolicyType lower_level_policy = kSameLevelNearbyKeys; // for levels >= max_boundary
  MappingPolicyType lower_level_policy_fallback = kSameLevelNearbyKeys;

  MappingPolicyType middle_level_policy = kCAZA; // for all other levels
  MappingPolicyType middle_level_policy_fallback = kCAZA;
  // MappingPolicyType fallback_policy = kLifetimeBased; // When a specific policy can't be applied

  uint64_t gc_start_level = 20; // GC kicks in when free space is lower than this percentage
  //TODO: gc trigger based on invalid amount of data per zone and/or capacity percentage per zone
  //can mix the two potentially
  uint64_t gc_slope = 3; // GC aggresiveness
  bool cold_migration = false; // Periodically transfer cold data into high-wear zones
  uint64_t gc_pause_seconds = 10; // The interval in seconds between GC activations
  uint8_t cold_migration = 0; // Move long-lived files from low-wear to high-wear zones

  uint32_t buffer_size_megabytes = 65;
  uint32_t buffer_count_max = 20;

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
        {"kClusterTogether", kClusterTogether},
        {"kOverlapChildren", kOverlapChildren},
        {"kOverlapGrandchildren", kOverlapGrandchildren},
      };

      if(type == "empty_zone_allocator" && empty_zone_allocator_map.find(value) != empty_zone_allocator_map.end())
        empty_zone_allocator = empty_zone_allocator_map[value];
      
      else if(type == "upper_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        upper_level_policy = mapping_policy_map[value];

      else if(type == "upper_level_policy_fallback" && mapping_policy_map.find(value) != mapping_policy_map.end())
        upper_level_policy_fallback = mapping_policy_map[value];

      else if(type == "lower_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        lower_level_policy = mapping_policy_map[value];
      
      else if(type == "lower_level_policy_fallback" && mapping_policy_map.find(value) != mapping_policy_map.end())
        lower_level_policy = mapping_policy_map[value];

      else if(type == "middle_level_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
        middle_level_policy = mapping_policy_map[value];

      else if(type == "middle_level_policy_fallback" && mapping_policy_map.find(value) != mapping_policy_map.end())
        middle_level_policy_fallback = mapping_policy_map[value];

      // else if(type == "fallback_policy" && mapping_policy_map.find(value) != mapping_policy_map.end())
      //   fallback_policy = mapping_policy_map[value];

      else if(type == "tombstone_density") {
        double value_double = std::stod(value);
        if(value_double >= 0.0 && value_double <= 1.0)
          tombstone_density = value_double;
      }

      else if(type == "cold_migration") {
        int value_int = std::stoi(value);
        cold_migration = value_int;
      }

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

      else if(type == "min_boundary") {
        int value_int = std::stoi(value);
        if(value_int >= 0 && value_int < 100)
          min_boundary = value_int;
      }

      else if(type == "max_boundary") {
        int value_int = std::stoi(value);
        if(value_int >= 0 && value_int < 100)
          max_boundary = value_int;
      }

      else if(type == "gc_pause_seconds") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 1000)
          gc_pause_seconds = value_int;
      }

      else if(type == "cold_migration") {
        int value_int = std::stoi(value);
        cold_migration = value_int;
      }

      else if(type == "buffer_size_megabytes") {
        uint32_t value_int = std::stoul(value);
        if(value_int > 0 && value_int <= 1025)
          buffer_size_megabytes = value_int;
      }

      else if(type == "buffer_count_max") {
        uint32_t value_int = std::stoul(value);
        if(value_int > 0 && value_int <= 100)
          buffer_count_max = value_int;
      }
    }

    if(max_boundary <= min_boundary)
      max_boundary = min_boundary + 1;
  }
};



}   // namespace ROCKSDB_NAMESPACE