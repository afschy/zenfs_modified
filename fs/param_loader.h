# pragma once
#include <fstream>
#include <string>
#include <unordered_map>
namespace ROCKSDB_NAMESPACE {

enum EmptyZoneAllocType {
  kSequential,
  kRandom,
  kLeastWear,
  kHotnessBased
};

struct ZenfsParamContainer {
  EmptyZoneAllocType empty_zone_allocator = kSequential;

  void LoadParamsFromFile() {
    std::ifstream infile("../params.txt");
    std::string type, value;

    while(infile >> type >> value) {
      static std::unordered_map<std::string, EmptyZoneAllocType>
      empty_zone_allocator_map = {
        {"kSequential", kSequential},
        {"kRandom", kRandom},
        {"kLeastWear", kLeastWear},
        {"kHotnessBased", kHotnessBased}
      };

      if(type == "empty_zone_allocator" && empty_zone_allocator_map.find(value) != empty_zone_allocator_map.end())
        empty_zone_allocator = empty_zone_allocator_map[value];
      
    }
}
};



}   // namespace ROCKSDB_NAMESPACE