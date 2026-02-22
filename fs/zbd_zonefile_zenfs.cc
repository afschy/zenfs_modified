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
  IOStatus s = zbd_->AllocateIOZone(lifetime_, io_type_, &zone, this);

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
  alloc_size_ = data_size;

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
      alloc_size_ = left;
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
  alloc_size_ = data_size;

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
      alloc_size_ = left;
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
  alloc_size_ = data_size;

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

      alloc_size_ = left;
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
    zbd_->RemoveZoneFileRecord(this);
  zbd_->AddZoneFileRecord(this);
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
