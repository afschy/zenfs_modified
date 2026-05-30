# pragma once
#include <fstream>
#include <string>
#include <unordered_map>
namespace ROCKSDB_NAMESPACE {

/** Strategy for selecting which empty zone to open when a new zone is needed. */
enum EmptyZoneAllocType {
  kDefault,      ///< First available empty zone.
  kSequential,   ///< Round-robin ordering across empty zones.
  kRandom,       ///< Random empty zone selection.
  kLeastWear,    ///< Empty zone with the lowest reset count (least wear).
  kHotnessBased  ///< Wear-aware: low-wear zones for upper levels, high-wear for lower levels.
};

/** Strategy for choosing which open zone to place a newly created SST file in. */
enum PlacementPolicyType {
  kLifetimeBased,             ///< ZenFS default; places files by write lifetime hint.
  kLeveledLifetimeBased,      ///< LIZA variant from the ZoneKV paper; used as a fallback.
  kCAZA,                      ///< Co-locates with neighboring-level files that have overlapping key ranges.
  kSameLevelNearbyKeys,       ///< Co-locates with the nearest same-level files by key range (weighted).
  kSameLevelNearbyKeysSimple, ///< Simplified nearest same-level key range variant.
  kArrivalTimeBased,          ///< Fills zones in file-arrival order per level (ZoneKV).
  kTombstoneDensity,          ///< Routes high-tombstone-density files to a dedicated zone.
  kTombstoneTTL,              ///< Co-locates files with similar TTL.
  kClusterTogether,           ///< Puts all files into the same zone; naive baseline.
  kOverlapChildren,           ///< Rank-based placement by byte overlap with level+1 (expanded).
  kOverlapGrandchildren,      ///< Rank-based placement by byte overlap with level+2.
  kCompensatedSize,           ///< Rank-based placement by tombstone-adjusted file size.
  kOAZA,                      ///< Rank-based placement by byte overlap with level+1.
};

/** Garbage collection algorithm used to reclaim space from zones with invalid data. */
enum GCType {
  kDefaultGC,   ///< Standard ZenFS garbage collector.
  kImprovedGC,  ///< Enhanced GC with smarter zone selection.
};

/** Holds all tunable ZenFS runtime parameters; populated at startup via LoadParamsFromFile(). */
struct ZenfsParamContainer {
  double nearest_newzone_threshold = 0.0;

  EmptyZoneAllocType empty_zone_allocator = kDefault;
  uint8_t real_caza = 1;
  uint8_t real_oaza = 1;
  uint8_t real_zonekv = 1;

  uint64_t average_value_size = 4080;
  
  uint8_t max_level = 1;
  std::mutex lock_max_level;
  uint8_t min_boundary = 2; ///< Levels <= this use the upper-level policy.
  uint8_t max_boundary = 6; ///< Levels >= this use the lower-level policy.

  double tombstone_density = 0.5;

  PlacementPolicyType upper_level_policy = kClusterTogether;          ///< Primary policy for levels <= min_boundary.
  PlacementPolicyType upper_level_policy_fallback = kClusterTogether; ///< Fallback policy for levels <= min_boundary.

  PlacementPolicyType lower_level_policy = kClusterTogether;          ///< Primary policy for levels >= max_boundary.
  PlacementPolicyType lower_level_policy_fallback = kClusterTogether; ///< Fallback policy for levels >= max_boundary.

  PlacementPolicyType middle_level_policy = kClusterTogether;          ///< Primary policy for all other levels.
  PlacementPolicyType middle_level_policy_fallback = kClusterTogether; ///< Fallback policy for all other levels.
  // PlacementPolicyType fallback_policy = kLifetimeBased; // When a specific policy can't be applied

  uint8_t fragmentation_enabled = 1; ///< When non-zero, allows a file to span multiple zones.

  GCType gc_type = kDefaultGC;
  uint64_t gc_start_level = 20; ///< GC activates when free space falls below this percentage.
  //TODO: gc trigger based on invalid amount of data per zone and/or capacity percentage per zone
  //can mix the two potentially
  uint64_t gc_stop_level = 25;  ///< GC stops when free space recovers to this percentage.
  uint64_t gc_slope = 3;        ///< GC aggressiveness multiplier.
  uint64_t gc_pause_seconds = 10; ///< Interval in seconds between successive GC activations.
  uint8_t cold_migration = 0;     ///< When non-zero, migrates long-lived files from low-wear to high-wear zones.
  uint64_t reserve_zone_count = 10; ///< Number of zones held in reserve; only used when no other zones are available.

  uint32_t buffer_size_megabytes = 2;
  uint32_t buffer_count_max = 20;

  std::string logname = "unizns_default.log";

  void LoadParamsFromFile() {
    static std::unordered_map<std::string, EmptyZoneAllocType>
      empty_zone_allocator_map = {
        {"kDefault", kDefault},
        {"kSequential", kSequential},
        {"kRandom", kRandom},
        {"kLeastWear", kLeastWear},
        {"kHotnessBased", kHotnessBased}
      };

    static std::unordered_map<std::string, PlacementPolicyType>
      placement_policy_map = {
        {"kLifetimeBased", kLifetimeBased},
        {"kLeveledLifetimeBased", kLeveledLifetimeBased},
        {"kCAZA", kCAZA},
        {"kSameLevelNearbyKeys", kSameLevelNearbyKeys},
        {"kSameLevelNearbyKeysSimple", kSameLevelNearbyKeysSimple},
        {"kArrivalTimeBased", kArrivalTimeBased},
        {"kTombstoneDensity", kTombstoneDensity},
        {"kTombstoneTTL", kTombstoneTTL},
        {"kClusterTogether", kClusterTogether},
        {"kOverlapChildren", kOverlapChildren},
        {"kOverlapGrandchildren", kOverlapGrandchildren},
        {"kCompensatedSize", kCompensatedSize},
        {"kOAZA", kOAZA},
      };
    
    static std::unordered_map<std::string, GCType>
      gc_type_map = {
        {"kDefaultGC", kDefaultGC},
        {"kImprovedGC", kImprovedGC},
      };

    std::ifstream infile("/home/afschy/RocksDB-Wrapper/lib/rocksdb/plugin/zenfs/params.txt");
    std::string type, value;

    while(infile >> type >> value) {

      if(type == "logname")
        logname = value;

      else if(type == "gc_type" && gc_type_map.find(value) != gc_type_map.end())
        gc_type = gc_type_map[value];

      else if(type == "empty_zone_allocator" && empty_zone_allocator_map.find(value) != empty_zone_allocator_map.end())
        empty_zone_allocator = empty_zone_allocator_map[value];
      
      else if(type == "upper_level_policy" && placement_policy_map.find(value) != placement_policy_map.end())
        upper_level_policy = placement_policy_map[value];

      else if(type == "upper_level_policy_fallback" && placement_policy_map.find(value) != placement_policy_map.end())
        upper_level_policy_fallback = placement_policy_map[value];

      else if(type == "lower_level_policy" && placement_policy_map.find(value) != placement_policy_map.end())
        lower_level_policy = placement_policy_map[value];
      
      else if(type == "lower_level_policy_fallback" && placement_policy_map.find(value) != placement_policy_map.end())
        lower_level_policy_fallback = placement_policy_map[value];

      else if(type == "middle_level_policy" && placement_policy_map.find(value) != placement_policy_map.end())
        middle_level_policy = placement_policy_map[value];

      else if(type == "middle_level_policy_fallback" && placement_policy_map.find(value) != placement_policy_map.end())
        middle_level_policy_fallback = placement_policy_map[value];

      // else if(type == "fallback_policy" && placement_policy_map.find(value) != placement_policy_map.end())
      //   fallback_policy = placement_policy_map[value];

      else if(type == "average_value_size") {
        int value_int = std::stoi(value);
        if(value_int > 0)
          average_value_size = value_int;
      }

      else if(type == "tombstone_density") {
        double value_double = std::stod(value);
        if(value_double >= 0.0 && value_double <= 1.0)
          tombstone_density = value_double;
      }

      else if(type == "nearest_newzone_threshold") {
        double value_double = std::stod(value);
        if(value_double >= 0.0 && value_double <= 1.0)
          nearest_newzone_threshold = value_double;
      }

      else if(type == "gc_start_level") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 100)
          gc_start_level = value_int;
      }

      else if(type == "gc_stop_level") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 100)
          gc_stop_level = value_int;
      }

      else if(type == "gc_slope") {
        int value_int = std::stoi(value);
        if(value_int > 0 && value_int < 100)
          gc_slope = value_int;
      }

      else if(type == "reserve_zone_count") {
        int value_int = std::stoi(value);
        if(value_int >= 0 && value_int < 100)
          reserve_zone_count = value_int;
      }

      else if(type == "real_caza") {
        int value_int = std::stoi(value);
        if(value_int == 0 || value_int == 1)
          real_caza = value_int;
      }

      else if(type == "real_oaza") {
        int value_int = std::stoi(value);
        if(value_int == 0 || value_int == 1)
          real_oaza = value_int;
      }

      else if(type == "real_zonekv") {
        int value_int = std::stoi(value);
        if(value_int == 0 || value_int == 1)
          real_zonekv = value_int;
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

      else if(type == "fragmentation_enabled") {
        uint64_t value_int = std::stoul(value);
        fragmentation_enabled = value_int;
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

  void PrintStats(FILE *fp) {
    fprintf(fp, "UniZNS parameter initialization complete\n");
    fprintf(fp, "Log file name = %s\n", logname.c_str());
    fprintf(fp, "gc_type = %d\n", gc_type);
    fprintf(fp, "upper_level_policy = %d\n", upper_level_policy);
    fprintf(fp, "middle_level_policy = %d\n", middle_level_policy);
    fprintf(fp, "lower_level_policy = %d\n", lower_level_policy);
    fprintf(fp, "GC start level = %lu\n", gc_start_level);
    fprintf(fp, "GC stop level = %lu\n", gc_stop_level);
    fprintf(fp, "fragmentation = %u\n", fragmentation_enabled);
    fprintf(fp, "nearest_newzone_threshold = %lf\n", nearest_newzone_threshold);
  }
};



}   // namespace ROCKSDB_NAMESPACE