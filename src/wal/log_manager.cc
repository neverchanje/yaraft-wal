// Copyright 2017 Wu Tao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wal/log_manager.h"
#include "base/logging.h"
#include "wal/log_writer.h"
#include "wal/readable_log_segment.h"

namespace consensus {
namespace wal {

static bool isWal(const std::string& fname) {
  // TODO(optimize)
  size_t len = fname.length();
  return len > 4 && fname.substr(len - 4, 4) == ".wal";
}

static void parseWalName(const std::string& fname, uint64_t* segId, uint64_t* segStart) {
  sscanf(fname.c_str(), "%zu-%zu.wal", segId, segStart);
}

// Returns: Error::YARaftError / OK
Status AppendToMemStore(yaraft::pb::Entry& e, yaraft::MemoryStorage* memstore) {
  auto& vec = memstore->TEST_Entries();

  if (!vec.empty()) {
    if (e.term() < vec.rbegin()->term()) {
      return FMT_Status(
          YARaftError,
          "new entry [index:{}, term:{}] has lower term than last entry [index:{}, term:{}]",
          e.index(), e.term(), vec.rbegin()->index(), vec.rbegin()->term());
    }

    int del = vec.size() - 1;
    while (del >= 0) {
      if (vec[del].index() < e.index()) {
        break;
      }
      del--;
    }

    size_t sz = vec.size();
    for (int i = del + 1; i < sz; i++) {
      vec.pop_back();
    }
  }

  memstore->Append(e);
  return Status::OK();
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

LogManager::LogManager(const WriteAheadLogOptions& options)
    : lastIndex_(0), options_(options), empty_(false) {}

LogManager::~LogManager() {
  Close();
}

Status LogManager::Recover(const WriteAheadLogOptions& options, yaraft::MemStoreUptr* memstore,
                           LogManagerUPtr* pLogManager) {
  RETURN_NOT_OK_APPEND(Env::Default()->CreateDirIfMissing(options.log_dir),
                       fmt::format(" [log_dir: \"{}\"]", options.log_dir));

  std::vector<std::string> files;
  RETURN_NOT_OK_APPEND(Env::Default()->GetChildren(options.log_dir, &files),
                       fmt::format(" [log_dir: \"{}\"]", options.log_dir));

  // finds all files with suffix ".wal"
  std::map<uint64_t, uint64_t> wals;  // ordered by segId
  for (const auto& f : files) {
    if (isWal(f)) {
      uint64_t segId, segStart;
      parseWalName(f, &segId, &segStart);
      wals[segId] = segStart;
    }
  }

  LogManagerUPtr& m = *pLogManager;
  m.reset(new LogManager(options));
  if (wals.empty()) {
    return Status::OK();
  }
  m->empty_ = false;

  LOG_ASSERT(*memstore == nullptr);
  memstore->reset(new yaraft::MemoryStorage);

  FMT_LOG(INFO, "recovering from {} wals, starts from {}-{}, ends at {}-{}", wals.size(),
          wals.begin()->first, wals.begin()->second, wals.rbegin()->first, wals.rbegin()->second);

  for (auto it = wals.begin(); it != wals.end(); it++) {
    std::string fname = options.log_dir + "/" + SegmentFileName(it->first, it->second);
    SegmentMetaData meta;
    RETURN_NOT_OK(
        ReadSegmentIntoMemoryStorage(fname, memstore->get(), &meta, options.verify_checksum));
    m->files_.push_back(std::move(meta));
  }
  return Status::OK();
}

Status LogManager::Write(const PBEntryVec& entries, const yaraft::pb::HardState* hs) {
  if (entries.empty()) {
    return Status::OK();
  }

  uint64_t beginIdx = entries.begin()->index();
  if (empty_) {
    lastIndex_ = beginIdx - 1;  // start at the first entry received.
    empty_ = false;
  }

  return doWrite(entries.begin(), entries.end(), hs);
}

// Required: begin != end
Status LogManager::doWrite(ConstPBEntriesIterator begin, ConstPBEntriesIterator end,
                           const yaraft::pb::HardState* hs) {
  auto segStart = begin;
  auto it = segStart;
  while (true) {
    if (!current_) {
      LogWriter* w;
      ASSIGN_IF_OK(LogWriter::New(this), w);
      current_.reset(w);
    }

    ASSIGN_IF_OK(current_->Append(segStart, end, hs), it);
    if (it == end) {
      // write complete
      break;
    }

    // hard state must have been written after a batch write completes.
    if (hs) {
      hs = nullptr;
    }

    lastIndex_ = std::prev(it)->index();

    finishCurrentWriter();

    segStart = it;
  }
  return Status::OK();
}

Status LogManager::Sync() {
  if (current_) {
    return current_->Sync();
  }
  return Status::OK();
}

Status LogManager::Close() {
  if (current_) {
    finishCurrentWriter();
  }
  return Status::OK();
}

Status LogManager::GC(WriteAheadLog::CompactionHint* hint) {
  return Status::OK();
}

void LogManager::finishCurrentWriter() {
  SegmentMetaData meta;
  FATAL_NOT_OK(current_->Finish(&meta), "LogWriter::Finish");
  files_.push_back(meta);
  delete current_.release();
}

}  // namespace wal
}  // namespace consensus