# pragma once
#include <fstream>
#include <pwd.h>
#include <stdio.h>
#include <string>
#include <sys/types.h>
#include <cstdint>
#include <unistd.h>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <vector>
namespace ROCKSDB_NAMESPACE {

/** Strategy for selecting which empty zone to open when a new zone is needed. */
enum EmptyZoneAllocType {
  kDefault,           ///< First available empty zone.
  kSequential,        ///< Round-robin ordering across empty zones.
  kRandom,            ///< Random empty zone selection.
  kLeastWear,         ///< Empty zone with the lowest reset count (least wear).
  kHotnessBased,      ///< Wear-aware: low-wear zones for upper levels, high-wear for lower levels.
  kUserDefinedAlloc,  ///< Space for the user to write their own new zone allocator implementation.
};

/** Strategy for choosing which open zone to place a newly created SST file in. */
enum PlacementPolicyType {
  kLifetimeBased,             ///< ZenFS default; places files by write lifetime hint.
  kLeveledLifetimeBased,      ///< LIZA variant from the ZoneKV paper; used as a fallback.
  kCAZA,                      ///< Co-locates with neighboring-level files that have overlapping key ranges.
  kPlazaBase,                 ///< Co-locates with the nearest same-level files by key range (weighted).
  KPlazaIntermediate,         ///< Slightly more complicated version of PLAZA.
  kPlazaAdvanced,             ///< PLAZA with heavy modifications in terms of opening new zones.
  kSameLevelNearbyKeysSimple, ///< Simplified nearest same-level key range variant.
  kArrivalTimeBased,          ///< Fills zones in file-arrival order per level (ZoneKV).
  kTombstoneDensity,          ///< Routes high-tombstone-density files to a dedicated zone.
  kTombstoneTTL,              ///< Co-locates files with similar TTL.
  kClusterTogether,           ///< Puts all files into the same zone; naive baseline.
  kOverlapChildren,           ///< Rank-based placement by byte overlap with level+1 (expanded).
  kOverlapGrandchildren,      ///< Rank-based placement by byte overlap with level+2.
  kCompensatedSize,           ///< Rank-based placement by tombstone-adjusted file size.
  kOAZA,                      ///< Rank-based placement by byte overlap with level+1.
  kUserDefinedPlacement,      ///< Space for the user to write their own file placement implementation.
};

/** Garbage collection algorithm used to reclaim space from zones with invalid data. */
enum GCType {
  kDefaultGC,       ///< Standard ZenFS garbage collector.
  kImprovedGC,      ///< Enhanced GC with smarter zone selection.
  kUserDefinedGC,   ///< Space for the user to write their own zone selector for GC
};

/** Holds all tunable ZenFS runtime parameters; populated at startup via LoadParamsFromFile(). */
struct ZenfsParamContainer {
  double nearest_newzone_threshold = 0.0;
  ///< Advanced plaza sends the file to an empty zone if that zone contains higher than this fraction of empty space
  double zone_fill_threshold = 0.8;

  EmptyZoneAllocType empty_zone_allocator = kDefault;
  uint8_t real_caza = 1;
  uint8_t real_oaza = 1;
  uint8_t real_zonekv = 1;
  uint8_t dynamic_level_adjustment = 1; ///< When the file composition of a zone changes, its level label will also change.
  std::vector<uint8_t> zones_to_open = std::vector<uint8_t>(10, 0); ///< For kPlazaAdvanced, the number of zones to keep open during allocation, per-level

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

  std::string logname = "zarc_default.log";
  uint8_t log_zonestats_placement_delete = 1;
  uint8_t log_zonestats_placement_delete_verbose = 0;

  void LoadParamsFromFile(bool print=true) {
    static std::unordered_map<std::string, EmptyZoneAllocType>
      empty_zone_allocator_map = {
        {"kDefault", kDefault},
        {"kSequential", kSequential},
        {"kRandom", kRandom},
        {"kLeastWear", kLeastWear},
        {"kHotnessBased", kHotnessBased},
        {"kUserDefinedAlloc", kUserDefinedAlloc},
      };

    static std::unordered_map<std::string, PlacementPolicyType>
      placement_policy_map = {
        {"kLifetimeBased", kLifetimeBased},
        {"kLeveledLifetimeBased", kLeveledLifetimeBased},
        {"kCAZA", kCAZA},
        {"kSameLevelNearbyKeys", kPlazaBase},
        {"kPlazaBase", kPlazaBase},
        {"KPlazaIntermediate", KPlazaIntermediate},
        {"kPlazaAdvanced", kPlazaAdvanced},
        {"kSameLevelNearbyKeysSimple", kSameLevelNearbyKeysSimple},
        {"kArrivalTimeBased", kArrivalTimeBased},
        {"kTombstoneDensity", kTombstoneDensity},
        {"kTombstoneTTL", kTombstoneTTL},
        {"kClusterTogether", kClusterTogether},
        {"kOverlapChildren", kOverlapChildren},
        {"kOverlapGrandchildren", kOverlapGrandchildren},
        {"kCompensatedSize", kCompensatedSize},
        {"kOAZA", kOAZA},
        {"kUserDefinedPlacement", kUserDefinedPlacement},
      };

    static std::unordered_map<std::string, GCType>
      gc_type_map = {
        {"kDefaultGC", kDefaultGC},
        {"kImprovedGC", kImprovedGC},
        {"kUserDefinedGC", kUserDefinedGC},
      };

    auto get_home = []() -> std::string {
      FILE* f = fopen("/proc/self/loginuid", "r");
      if (f) {
        uid_t uid = (uid_t)-1;
        bool ok = fscanf(f, "%u", &uid) == 1 && uid != (uid_t)-1;
        fclose(f);
        if (ok) {
          struct passwd* pw = getpwuid(uid);
          if (pw && pw->pw_dir) return std::string(pw->pw_dir);
        }
      }
      const char* su = getenv("SUDO_USER");
      struct passwd* pw = su ? getpwnam(su) : getpwuid(getuid());
      return (pw && pw->pw_dir) ? std::string(pw->pw_dir) : "/tmp";
    };
    const char* env_path = getenv("ZENFS_PARAMS");
    std::string param_file_path = env_path
        ? std::string(env_path)
        : get_home() + "/RocksDB-Wrapper/lib/rocksdb/plugin/zenfs/params.txt";
    fprintf(stdout, "PARAM_FILE_PATH %s\n", param_file_path.c_str());
    std::ifstream infile(param_file_path);
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

      else if(type == "zone_fill_threshold") {
        double value_double = std::stod(value);
        if(value_double >= 0.0 && value_double <= 1.0)
          zone_fill_threshold = value_double;
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

      else if(type == "dynamic_level_adjustment") {
        int value_int = std::stoi(value);
        if(value_int)
          dynamic_level_adjustment = 1;
        else
          dynamic_level_adjustment = 0;
      }

      else if(type == "zones_to_open") {
        std::vector<uint8_t> parsed_zones_to_open;
        std::stringstream value_stream(value);
        std::string token;
        while(std::getline(value_stream, token, ',')) {
          if(token.empty())
            continue;
          int value_int = std::stoi(token);
          if(value_int >= 0 && value_int <= 255)
            parsed_zones_to_open.push_back(static_cast<uint8_t>(value_int));
        }
        if(!parsed_zones_to_open.empty())
          zones_to_open = parsed_zones_to_open;
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

      else if(type == "log_zonestats_placement_delete") {
        uint32_t value_int = std::stoul(value);
        log_zonestats_placement_delete = value_int;
      }

      else if(type == "log_zonestats_placement_delete_verbose") {
        uint32_t value_int = std::stoul(value);
        log_zonestats_placement_delete_verbose = value_int;
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
    fprintf(fp, "nearest_newzone_threshold = %0.2lf\n", nearest_newzone_threshold);
    fprintf(fp, "zone_fill_threshold = %0.2lf\n", zone_fill_threshold);
    fprintf(fp, "dynamic_level_adjustment = %u\n", dynamic_level_adjustment);
    fprintf(fp, "zones_to_open: ");
    for(int i=0; i<10; i++)
      fprintf(fp, " %u", zones_to_open[i]);
    fprintf(fp, "\n");
  }
};



}   // namespace ROCKSDB_NAMESPACE
