#pragma once

#include "common.h"
#include "path_utils.h"

struct TransferState {
  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> deletable_files;
  std::string source_dir;
  std::string dest_dir;
  std::string direction;
  std::string headline;
  std::string detail;
  std::string progress;
  std::string current_file;
  uint64_t total_bytes = 0;
  uint64_t processed_bytes = 0;
  int files_total = 0;
  int files_done = 0;
  int files_skipped = 0;
  int files_deletable = 0;
  int files_deleted = 0;
  int progress_pct = 0;
  bool available = false;
  bool running = false;
  bool deleting = false;
  bool error = false;
  bool done = false;
  bool can_start = false;
  bool can_delete = false;
};

struct TransferSession {
  std::mutex mutex;
  TransferState state;
  std::thread thread;
  std::atomic<bool> thread_running{false};
};

std::string FormatBytesCompact(uint64_t bytes);
bool CollectTransferFiles(const std::string& dir,
                          std::vector<std::filesystem::path>* files,
                          uint64_t* total_bytes);
void JoinTransferThreadIfDone(TransferSession& transfer);
void RefreshTransferState(TransferSession& transfer, bool using_external_storage,
                          const std::string& active_recordings_dir);
bool StartTransfer(TransferSession& transfer);
bool DeleteTransferredFiles(TransferSession& transfer);
