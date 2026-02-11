// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "io_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

ZonedWritableFile::ZonedWritableFile(ZonedBlockDevice* zbd, bool _buffered,
                                     std::shared_ptr<ZoneFile> zoneFile) {
  assert(zoneFile->IsOpenForWR());
  wp = zoneFile->GetFileSize();

  buffered = _buffered;
  block_sz = zbd->GetBlockSize();
  zoneFile_ = zoneFile;
  buffer_pos = 0;
  sparse_buffer = nullptr;
  buffer = nullptr;

  buffer_size_megabytes = zoneFile->GetZbd()->zenfs_parameters_.buffer_size_megabytes;
  if(max_buffer_count == INT32_MAX)
    zoneFile->GetZbd()->zenfs_parameters_.buffer_size_megabytes;

  if (buffered) {
    // if maximum buffer capacity is reached, wait for a buffer to be de-allocated
    std::unique_lock lk(buffer_count_mtx_);
    buffer_count_condvar_.wait(lk, [this]{ return curr_buffer_count < max_buffer_count; });
    curr_buffer_count++;
    lk.unlock();

    if (zoneFile->IsSparse()) {
      size_t sparse_buffer_sz;

      sparse_buffer_sz =
          buffer_size_megabytes * 1024 * 1024 + block_sz; /* one extra block size for padding */
      int ret = posix_memalign((void**)&sparse_buffer, sysconf(_SC_PAGESIZE),
                               sparse_buffer_sz);

      if (ret) sparse_buffer = nullptr;

      assert(sparse_buffer != nullptr);

      buffer_sz = sparse_buffer_sz - ZoneFile::SPARSE_HEADER_SIZE - block_sz;
      buffer = sparse_buffer + ZoneFile::SPARSE_HEADER_SIZE;
    } else {
      buffer_sz = buffer_size_megabytes * 1024 * 1024;
      int ret =
          posix_memalign((void**)&buffer, sysconf(_SC_PAGESIZE), buffer_sz);

      if (ret) buffer = nullptr;
      assert(buffer != nullptr);
    }
  }

  open = true;
}

ZonedWritableFile::~ZonedWritableFile() {
  IOStatus s = CloseInternal();
  if (buffered) {
    if (sparse_buffer != nullptr) {
      free(sparse_buffer);
    } else {
      free(buffer);
    }
    {
      std::unique_lock lk(buffer_count_mtx_);
      if(curr_buffer_count > 0)
        curr_buffer_count--;
    }
    buffer_count_condvar_.notify_all();
  }

  if (!s.ok()) {
    zoneFile_->GetZbd()->SetZoneDeferredStatus(s);
  }
}

MetadataWriter::~MetadataWriter() {}

IOStatus ZonedWritableFile::Truncate(uint64_t size,
                                     const IOOptions& /*options*/,
                                     IODebugContext* /*dbg*/) {
  zoneFile_->SetFileSize(size);
  return IOStatus::OK();
}

IOStatus ZonedWritableFile::DataSync() {
  if (buffered) {
    IOStatus s;
    buffer_mtx_.lock();
    /* Flushing the buffer will result in a new extent added to the list*/
    s = FlushBuffer();
    buffer_mtx_.unlock();
    if (!s.ok()) {
      return s;
    }

    /* We need to persist the new extent, if the file is not sparse,
     * as we can't use the active zone WP, which is block-aligned, to recover
     * the file size */
    if (!zoneFile_->IsSparse()) return zoneFile_->PersistMetadata();
  } else {
    /* For direct writes, there is no buffer to flush, we just need to push
       an extent for the latest written data */
    zoneFile_->PushExtent();
  }

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Fsync(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_SYNC_LATENCY
                                     : ZENFS_NON_WAL_SYNC_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_SYNC_QPS, 1);

  s = DataSync();
  if (!s.ok()) return s;

  /* As we've already synced the metadata in DataSync, no need to do it again */
  if (buffered && !zoneFile_->IsSparse()) return IOStatus::OK();

  return zoneFile_->PersistMetadata();
}

IOStatus ZonedWritableFile::Sync(const IOOptions& /*options*/,
                                 IODebugContext* /*dbg*/) {
  return DataSync();
}

IOStatus ZonedWritableFile::Flush(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  return IOStatus::OK();
}

IOStatus ZonedWritableFile::RangeSync(uint64_t offset, uint64_t nbytes,
                                      const IOOptions& /*options*/,
                                      IODebugContext* /*dbg*/) {
  if (wp < offset + nbytes) return DataSync();

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Close(const IOOptions& /*options*/,
                                  IODebugContext* /*dbg*/) {
  return CloseInternal();
}

IOStatus ZonedWritableFile::CloseInternal() {
  if (!open) {
    return IOStatus::OK();
  }

  IOStatus s = DataSync();
  if (!s.ok()) return s;

  s = zoneFile_->CloseWR();
  if (!s.ok()) return s;

  open = false;
  return s;
}

IOStatus ZonedWritableFile::FlushBuffer() {
  IOStatus s;

  if (buffer_pos == 0) return IOStatus::OK();

  if (zoneFile_->IsSparse()) {
    s = zoneFile_->SparseAppend(sparse_buffer, buffer_pos);
  } else {
    s = zoneFile_->BufferedAppend(buffer, buffer_pos);
  }

  if (!s.ok()) {
    return s;
  }

  wp += buffer_pos;
  buffer_pos = 0;

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::BufferedWrite(const Slice& slice) {
  uint32_t data_left = slice.size();
  char* data = (char*)slice.data();
  IOStatus s;

  while (data_left) {
    uint32_t buffer_left = buffer_sz - buffer_pos;
    uint32_t to_buffer;

    if (!buffer_left) {
      s = FlushBuffer();
      if (!s.ok()) return s;
      buffer_left = buffer_sz;
    }

    to_buffer = data_left;
    if (to_buffer > buffer_left) {
      to_buffer = buffer_left;
    }

    memcpy(buffer + buffer_pos, data, to_buffer);
    buffer_pos += to_buffer;
    data_left -= to_buffer;
    data += to_buffer;
  }

  return IOStatus::OK();
}

IOStatus ZonedWritableFile::Append(const Slice& data,
                                   const IOOptions& /*options*/,
                                   IODebugContext* /*dbg*/) {
  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_WRITE_LATENCY
                                     : ZENFS_NON_WAL_WRITE_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_WRITE_QPS, 1);
  zoneFile_->GetZBDMetrics()->ReportThroughput(ZENFS_WRITE_THROUGHPUT,
                                               data.size());

  if (buffered) {
    buffer_mtx_.lock();
    s = BufferedWrite(data);
    buffer_mtx_.unlock();
  } else {
    s = zoneFile_->Append((void*)data.data(), data.size());
    if (s.ok()) wp += data.size();
  }

  return s;
}

IOStatus ZonedWritableFile::PositionedAppend(const Slice& data, uint64_t offset,
                                             const IOOptions& /*options*/,
                                             IODebugContext* /*dbg*/) {
  IOStatus s;
  ZenFSMetricsLatencyGuard guard(zoneFile_->GetZBDMetrics(),
                                 zoneFile_->GetIOType() == IOType::kWAL
                                     ? ZENFS_WAL_WRITE_LATENCY
                                     : ZENFS_NON_WAL_WRITE_LATENCY,
                                 Env::Default());
  zoneFile_->GetZBDMetrics()->ReportQPS(ZENFS_WRITE_QPS, 1);
  zoneFile_->GetZBDMetrics()->ReportThroughput(ZENFS_WRITE_THROUGHPUT,
                                               data.size());

  if (offset != wp) {
    assert(false);
    return IOStatus::IOError("positioned append not at write pointer");
  }

  if (buffered) {
    buffer_mtx_.lock();
    s = BufferedWrite(data);
    buffer_mtx_.unlock();
  } else {
    s = zoneFile_->Append((void*)data.data(), data.size());
    if (s.ok()) wp += data.size();
  }

  return s;
}

void ZonedWritableFile::SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) {
  zoneFile_->SetWriteLifeTimeHint(hint);
}

void ZonedWritableFile::SetLevel(int level) {
  zoneFile_->SetLevel(level);
}

void ZonedWritableFile::UpdateInternalKeys(const Slice& key, SequenceNumber seqno) {
  zoneFile_->UpdateInternalKeys(key, seqno);
}

void ZonedWritableFile::UpdateInternalKeysRange(const Slice& start, const Slice& end, SequenceNumber seqno, const CompareInterface* icmp) {
  zoneFile_->UpdateInternalKeysRange(start, end, seqno, icmp);
}

void ZonedWritableFile::UpdateMetadata(const TableProperties& table_properties) {
  zoneFile_->UpdateMetadata(table_properties);
}

void ZonedWritableFile::UpdateMetadata(uint64_t num_entries, uint64_t num_deletions, uint64_t num_range_deletions, uint64_t file_size, uint64_t compensated_range_deletion_size, uint64_t average_value_size) {
  zoneFile_->UpdateMetadata(num_entries, num_deletions, num_range_deletions, file_size, compensated_range_deletion_size, average_value_size);
}

void ZonedWritableFile::SetInternalComparator(CompareInterface* icmp) {
  zoneFile_->SetInternalComparator(icmp);
}

IOStatus ZonedSequentialFile::Read(size_t n, const IOOptions& /*options*/,
                                   Slice* result, char* scratch,
                                   IODebugContext* /*dbg*/) {
  IOStatus s;

  s = zoneFile_->PositionedRead(rp, n, result, scratch, direct_);
  if (s.ok()) rp += result->size();

  return s;
}

IOStatus ZonedSequentialFile::Skip(uint64_t n) {
  if (rp + n >= zoneFile_->GetFileSize())
    return IOStatus::InvalidArgument("Skip beyond end of file");
  rp += n;
  return IOStatus::OK();
}

IOStatus ZonedSequentialFile::PositionedRead(uint64_t offset, size_t n,
                                             const IOOptions& /*options*/,
                                             Slice* result, char* scratch,
                                             IODebugContext* /*dbg*/) {
  return zoneFile_->PositionedRead(offset, n, result, scratch, direct_);
}

IOStatus ZonedRandomAccessFile::Read(uint64_t offset, size_t n,
                                     const IOOptions& /*options*/,
                                     Slice* result, char* scratch,
                                     IODebugContext* /*dbg*/) const {
  return zoneFile_->PositionedRead(offset, n, result, scratch, direct_);
}

IOStatus ZoneFile::MigrateData(uint64_t offset, uint32_t length,
                               Zone* target_zone) {
  uint32_t step = 128 << 10;
  uint32_t read_sz = step;
  int block_sz = zbd_->GetBlockSize();

  assert(offset % block_sz == 0);
  if (offset % block_sz != 0) {
    return IOStatus::IOError("MigrateData offset is not aligned!\n");
  }

  char* buf;
  int ret = posix_memalign((void**)&buf, block_sz, step);
  if (ret) {
    return IOStatus::IOError("failed allocating alignment write buffer\n");
  }

  int pad_sz = 0;
  while (length > 0) {
    read_sz = length > read_sz ? read_sz : length;
    pad_sz = read_sz % block_sz == 0 ? 0 : (block_sz - (read_sz % block_sz));

    int r = zbd_->Read(buf, offset, read_sz + pad_sz, true);
    if (r < 0) {
      free(buf);
      return IOStatus::IOError(strerror(errno));
    }
    target_zone->Append(buf, r);
    length -= read_sz;
    offset += r;
  }

  free(buf);

  return IOStatus::OK();
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
