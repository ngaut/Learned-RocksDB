//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2012 Facebook.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ROCKSDB_LITE

#include "rocksdb/utilities/checkpoint.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <string>
#include "db/wal_manager.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/transaction_log.h"
#include "util/file_util.h"
#include "util/filename.h"
#include "util/sync_point.h"

namespace rocksdb {

class CheckpointImpl : public Checkpoint {
 public:
  // Creates a Checkpoint object to be used for creating openable snapshots
  explicit CheckpointImpl(DB* db) : db_(db) {}

  // Builds an openable snapshot of RocksDB on the same disk, which
  // accepts an output directory on the same disk, and under the directory
  // (1) hard-linked SST files pointing to existing live SST files
  // SST files will be copied if output directory is on a different filesystem
  // (2) a copied manifest files and other files
  // The directory should not already exist and will be created by this API.
  // The directory will be an absolute path
  using Checkpoint::CreateCheckpoint;
  virtual Status CreateCheckpoint(const std::string& checkpoint_dir,
                                  uint64_t log_size_for_flush) override;

 private:
  DB* db_;
};

Status Checkpoint::Create(DB* db, Checkpoint** checkpoint_ptr) {
  *checkpoint_ptr = new CheckpointImpl(db);
  return Status::OK();
}

Status Checkpoint::CreateCheckpoint(const std::string& checkpoint_dir,
                                    uint64_t log_size_for_flush) {
  return Status::NotSupported("");
}

// Builds an openable snapshot of RocksDB
Status CheckpointImpl::CreateCheckpoint(const std::string& checkpoint_dir,
                                        uint64_t log_size_for_flush) {
  Status s;
  std::vector<std::string> live_files;
  uint64_t manifest_file_size = 0;
  DBOptions db_options = db_->GetDBOptions();
  uint64_t min_log_num = port::kMaxUint64;
  uint64_t sequence_number = db_->GetLatestSequenceNumber();
  bool same_fs = true;
  VectorLogPtr live_wal_files;

  s = db_->GetEnv()->FileExists(checkpoint_dir);
  if (s.ok()) {
    return Status::InvalidArgument("Directory exists");
  } else if (!s.IsNotFound()) {
    assert(s.IsIOError());
    return s;
  }

  s = db_->DisableFileDeletions();
  bool flush_memtable = true;
  if (s.ok()) {
    if (!db_options.allow_2pc) {
      // If out standing log files are small, we skip the flush.
      s = db_->GetSortedWalFiles(live_wal_files);

      if (!s.ok()) {
        db_->EnableFileDeletions(false);
        return s;
      }

      // Don't flush column families if total log size is smaller than
      // log_size_for_flush. We copy the log files instead.
      // We may be able to cover 2PC case too.
      uint64_t total_wal_size = 0;
      for (auto& wal : live_wal_files) {
        total_wal_size += wal->SizeFileBytes();
      }
      if (total_wal_size < log_size_for_flush) {
        flush_memtable = false;
      }
      live_wal_files.clear();
    }

    // this will return live_files prefixed with "/"
    s = db_->GetLiveFiles(live_files, &manifest_file_size, flush_memtable);

    if (s.ok() && db_options.allow_2pc) {
      // If 2PC is enabled, we need to get minimum log number after the flush.
      // Need to refetch the live files to recapture the snapshot.
      if (!db_->GetIntProperty(DB::Properties::kMinLogNumberToKeep,
                               &min_log_num)) {
        db_->EnableFileDeletions(false);
        return Status::InvalidArgument(
            "2PC enabled but cannot fine the min log number to keep.");
      }
      // We need to refetch live files with flush to handle this case:
      // A previous 000001.log contains the prepare record of transaction tnx1.
      // The current log file is 000002.log, and sequence_number points to this
      // file.
      // After calling GetLiveFiles(), 000003.log is created.
      // Then tnx1 is committed. The commit record is written to 000003.log.
      // Now we fetch min_log_num, which will be 3.
      // Then only 000002.log and 000003.log will be copied, and 000001.log will
      // be skipped. 000003.log contains commit message of tnx1, but we don't
      // have respective prepare record for it.
      // In order to avoid this situation, we need to force flush to make sure
      // all transactions commited before getting min_log_num will be flushed
      // to SST files.
      // We cannot get min_log_num before calling the GetLiveFiles() for the
      // first time, because if we do that, all the logs files will be included,
      // far more than needed.
      s = db_->GetLiveFiles(live_files, &manifest_file_size, /* flush */ true);
    }

    TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles1");
    TEST_SYNC_POINT("CheckpointImpl::CreateCheckpoint:SavedLiveFiles2");
  }
  // if we have more than one column family, we need to also get WAL files
  if (s.ok()) {
    s = db_->GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    db_->EnableFileDeletions(false);
    return s;
  }

  size_t wal_size = live_wal_files.size();
  ROCKS_LOG_INFO(
      db_options.info_log,
      "Started the snapshot process -- creating snapshot in directory %s",
      checkpoint_dir.c_str());

  std::string full_private_path = checkpoint_dir + ".tmp";

  // create snapshot directory
  s = db_->GetEnv()->CreateDir(full_private_path);

  // copy/hard link live_files
  std::string manifest_fname, current_fname;
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(live_files[i], &number, &type);
    if (!ok) {
      s = Status::Corruption("Can't parse file name. This is very bad");
      break;
    }
    // we should only get sst, options, manifest and current files here
    assert(type == kTableFile || type == kDescriptorFile ||
           type == kCurrentFile || type == kOptionsFile);
    assert(live_files[i].size() > 0 && live_files[i][0] == '/');
    if (type == kCurrentFile) {
      // We will craft the current file manually to ensure it's consistent with
      // the manifest number. This is necessary because current's file contents
      // can change during checkpoint creation.
      current_fname = live_files[i];
      continue;
    } else if (type == kDescriptorFile) {
      manifest_fname = live_files[i];
    }
    std::string src_fname = live_files[i];

    // rules:
    // * if it's kTableFile, then it's shared
    // * if it's kDescriptorFile, limit the size to manifest_file_size
    // * always copy if cross-device link
    if ((type == kTableFile) && same_fs) {
      ROCKS_LOG_INFO(db_options.info_log, "Hard Linking %s", src_fname.c_str());
      s = db_->GetEnv()->LinkFile(db_->GetName() + src_fname,
                                  full_private_path + src_fname);
      if (s.IsNotSupported()) {
        same_fs = false;
        s = Status::OK();
      }
    }
    if ((type != kTableFile) || (!same_fs)) {
      ROCKS_LOG_INFO(db_options.info_log, "Copying %s", src_fname.c_str());
      s = CopyFile(db_->GetEnv(), db_->GetName() + src_fname,
                   full_private_path + src_fname,
                   (type == kDescriptorFile) ? manifest_file_size : 0,
                   db_options.use_fsync);
    }
  }
  if (s.ok() && !current_fname.empty() && !manifest_fname.empty()) {
    s = CreateFile(db_->GetEnv(), full_private_path + current_fname,
                   manifest_fname.substr(1) + "\n");
  }
  ROCKS_LOG_INFO(db_options.info_log, "Number of log files %" ROCKSDB_PRIszt,
                 live_wal_files.size());

  // Link WAL files. Copy exact size of last one because it is the only one
  // that has changes after the last flush.
  for (size_t i = 0; s.ok() && i < wal_size; ++i) {
    if ((live_wal_files[i]->Type() == kAliveLogFile) &&
        (!flush_memtable ||
         live_wal_files[i]->StartSequence() >= sequence_number ||
         live_wal_files[i]->LogNumber() >= min_log_num)) {
      if (i + 1 == wal_size) {
        ROCKS_LOG_INFO(db_options.info_log, "Copying %s",
                       live_wal_files[i]->PathName().c_str());
        s = CopyFile(db_->GetEnv(),
                     db_options.wal_dir + live_wal_files[i]->PathName(),
                     full_private_path + live_wal_files[i]->PathName(),
                     live_wal_files[i]->SizeFileBytes(), db_options.use_fsync);
        break;
      }
      if (same_fs) {
        // we only care about live log files
        ROCKS_LOG_INFO(db_options.info_log, "Hard Linking %s",
                       live_wal_files[i]->PathName().c_str());
        s = db_->GetEnv()->LinkFile(
            db_options.wal_dir + live_wal_files[i]->PathName(),
            full_private_path + live_wal_files[i]->PathName());
        if (s.IsNotSupported()) {
          same_fs = false;
          s = Status::OK();
        }
      }
      if (!same_fs) {
        ROCKS_LOG_INFO(db_options.info_log, "Copying %s",
                       live_wal_files[i]->PathName().c_str());
        s = CopyFile(db_->GetEnv(),
                     db_options.wal_dir + live_wal_files[i]->PathName(),
                     full_private_path + live_wal_files[i]->PathName(), 0,
                     db_options.use_fsync);
      }
    }
  }

  // we copied all the files, enable file deletions
  db_->EnableFileDeletions(false);

  if (s.ok()) {
    // move tmp private backup to real snapshot directory
    s = db_->GetEnv()->RenameFile(full_private_path, checkpoint_dir);
  }
  if (s.ok()) {
    unique_ptr<Directory> checkpoint_directory;
    db_->GetEnv()->NewDirectory(checkpoint_dir, &checkpoint_directory);
    if (checkpoint_directory != nullptr) {
      s = checkpoint_directory->Fsync();
    }
  }

  if (!s.ok()) {
    // clean all the files we might have created
    ROCKS_LOG_INFO(db_options.info_log, "Snapshot failed -- %s",
                   s.ToString().c_str());
    // we have to delete the dir and all its children
    std::vector<std::string> subchildren;
    db_->GetEnv()->GetChildren(full_private_path, &subchildren);
    for (auto& subchild : subchildren) {
      std::string subchild_path = full_private_path + "/" + subchild;
      Status s1 = db_->GetEnv()->DeleteFile(subchild_path);
      ROCKS_LOG_INFO(db_options.info_log, "Delete file %s -- %s",
                     subchild_path.c_str(), s1.ToString().c_str());
    }
    // finally delete the private dir
    Status s1 = db_->GetEnv()->DeleteDir(full_private_path);
    ROCKS_LOG_INFO(db_options.info_log, "Delete dir %s -- %s",
                   full_private_path.c_str(), s1.ToString().c_str());
    return s;
  }

  // here we know that we succeeded and installed the new snapshot
  ROCKS_LOG_INFO(db_options.info_log, "Snapshot DONE. All is good");
  ROCKS_LOG_INFO(db_options.info_log, "Snapshot sequence number: %" PRIu64,
                 sequence_number);

  return s;
}
}  // namespace rocksdb

#endif  // ROCKSDB_LITE
