#include "transfer.h"

#include "playback.h"

std::string FormatBytesCompact(uint64_t bytes) {
  const char* units[] = {"B", "K", "M", "G", "T"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llub",
                  static_cast<unsigned long long>(bytes));
  } else if (value >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%.0f%s", value, units[unit]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f%s", value, units[unit]);
  }
  return buf;
}

bool CollectTransferFiles(const std::string& dir,
                          std::vector<std::filesystem::path>* files,
                          uint64_t* total_bytes) {
  if (!files || !total_bytes) {
    return false;
  }
  files->clear();
  *total_bytes = 0;
  std::error_code ec;
  const std::filesystem::path root(dir);
  if (!std::filesystem::exists(root, ec) ||
      !std::filesystem::is_directory(root, ec)) {
    return true;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.') {
      continue;
    }
    if (ToLowerCopy(entry.path().extension().string()) == ".part") {
      continue;
    }
    const auto size = entry.file_size(ec);
    if (ec) {
      continue;
    }
    files->push_back(entry.path());
    *total_bytes += static_cast<uint64_t>(size);
  }
  std::sort(files->begin(), files->end(), [](const auto& a, const auto& b) {
    return a.filename().string() < b.filename().string();
  });
  return true;
}

void JoinTransferThreadIfDone(TransferSession& transfer) {
  if (transfer.thread.joinable() && !transfer.thread_running.load()) {
    transfer.thread.join();
  }
}

void RefreshTransferState(TransferSession& transfer, bool using_external_storage,
                          const std::string& active_recordings_dir) {
  std::lock_guard<std::mutex> lock(transfer.mutex);
  if (transfer.thread_running.load()) {
    return;
  }
  TransferState next;
  next.direction = using_external_storage ? "EXT -> SD" : "SD -> EXT";
  next.source_dir = using_external_storage ? active_recordings_dir
                                           : std::string(kDefaultRecordingsDir);
  next.dest_dir = using_external_storage ? std::string(kDefaultRecordingsDir)
                                         : DetectExternalRecordingsDir();
  CollectTransferFiles(next.source_dir, &next.files, &next.total_bytes);
  next.files_total = static_cast<int>(next.files.size());
  next.detail = next.direction;

  if (next.dest_dir.empty()) {
    next.error = true;
    next.headline = using_external_storage ? "NO SD TARGET" : "NO EXT DRIVE";
    next.detail = using_external_storage ? "CHECK SD STORAGE" : "INSERT EXFAT USB";
    transfer.state = std::move(next);
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(next.dest_dir, ec);
  if (ec) {
    next.error = true;
    next.headline = "TARGET ERROR";
    next.detail = "CREATE DIR FAILED";
    transfer.state = std::move(next);
    return;
  }

  next.available = true;
  next.can_start = next.files_total > 0;
  if (next.files_total == 0) {
    next.headline = "NO FILES";
    next.detail = next.direction;
  } else {
    next.headline = std::to_string(next.files_total) + " FILES " +
                    FormatBytesCompact(next.total_bytes);
    next.progress = "0/" + std::to_string(next.files_total);
    next.detail = std::string("TARGET ") + (using_external_storage ? "SD" : "EXT");
  }
  transfer.state = std::move(next);
}

bool StartTransfer(TransferSession& transfer) {
  JoinTransferThreadIfDone(transfer);
  std::string dest_dir;
  std::vector<std::filesystem::path> files;
  uint64_t total_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(transfer.mutex);
    if (transfer.thread_running.load() || !transfer.state.can_start) {
      return false;
    }
    dest_dir = transfer.state.dest_dir;
    files = transfer.state.files;
    total_bytes = transfer.state.total_bytes;
    transfer.state.running = true;
    transfer.state.deleting = false;
    transfer.state.error = false;
    transfer.state.done = false;
    transfer.state.can_delete = false;
    transfer.state.files_done = 0;
    transfer.state.files_skipped = 0;
    transfer.state.files_deletable = 0;
    transfer.state.files_deleted = 0;
    transfer.state.processed_bytes = 0;
    transfer.state.progress_pct = 0;
    transfer.state.progress = "0/" + std::to_string(transfer.state.files_total);
    transfer.state.current_file.clear();
    transfer.state.deletable_files.clear();
    transfer.state.headline = "COPYING";
    transfer.state.detail = transfer.state.direction;
  }

  transfer.thread_running.store(true);
  transfer.thread = std::thread([&, dest_dir, files, total_bytes]() {
    constexpr size_t kChunkBytes = 1024 * 1024;
    std::vector<char> chunk(kChunkBytes);
    bool failed = false;
    std::string fail_headline;
    std::string fail_detail;
    int files_done = 0;
    int files_skipped = 0;
    std::vector<std::filesystem::path> deletable_files;
    uint64_t processed_bytes = 0;

    for (const auto& src_path : files) {
      std::error_code ec;
      const std::string filename = src_path.filename().string();
      const uint64_t file_size = static_cast<uint64_t>(
          std::filesystem::file_size(src_path, ec));
      if (ec) {
        failed = true;
        fail_headline = "SIZE FAILED";
        fail_detail = TruncateDisplayName(filename, 16);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(transfer.mutex);
        transfer.state.current_file = TruncateDisplayName(filename, 18);
      }
      const std::filesystem::path dest_path =
          std::filesystem::path(dest_dir) / src_path.filename();
      const std::filesystem::path tmp_path =
          std::filesystem::path(dest_path.string() + ".part");

      if (std::filesystem::exists(dest_path, ec) && !ec) {
        bool safe_to_delete = false;
        const auto dest_size = std::filesystem::file_size(dest_path, ec);
        if (!ec) {
          safe_to_delete = static_cast<uint64_t>(dest_size) == file_size;
        }
        ++files_done;
        ++files_skipped;
        if (safe_to_delete) {
          deletable_files.push_back(src_path);
        }
        processed_bytes += file_size;
        std::lock_guard<std::mutex> lock(transfer.mutex);
        transfer.state.files_done = files_done;
        transfer.state.files_skipped = files_skipped;
        transfer.state.files_deletable = static_cast<int>(deletable_files.size());
        transfer.state.processed_bytes = processed_bytes;
        transfer.state.progress_pct =
            total_bytes == 0 ? 100
                             : static_cast<int>((processed_bytes * 100) / total_bytes);
        transfer.state.progress = std::to_string(files_done) + "/" +
                                  std::to_string(transfer.state.files_total) +
                                  " " + std::to_string(transfer.state.progress_pct) + "%";
        continue;
      }

      std::filesystem::remove(tmp_path, ec);
      std::ifstream in(src_path, std::ios::binary);
      std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
      if (!in.is_open() || !out.is_open()) {
        failed = true;
        fail_headline = "COPY FAILED";
        fail_detail = TruncateDisplayName(filename, 16);
        break;
      }

      while (in.good()) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
          break;
        }
        out.write(chunk.data(), got);
        if (!out.good()) {
          failed = true;
          fail_headline = "WRITE FAILED";
          fail_detail = TruncateDisplayName(filename, 16);
          break;
        }
        processed_bytes += static_cast<uint64_t>(got);
        std::lock_guard<std::mutex> lock(transfer.mutex);
        transfer.state.processed_bytes = processed_bytes;
        transfer.state.progress_pct =
            total_bytes == 0 ? 100
                             : static_cast<int>((processed_bytes * 100) / total_bytes);
        transfer.state.progress = std::to_string(files_done) + "/" +
                                  std::to_string(transfer.state.files_total) +
                                  " " + std::to_string(transfer.state.progress_pct) + "%";
      }
      in.close();
      out.close();
      if (failed) {
        std::filesystem::remove(tmp_path, ec);
        break;
      }

      std::filesystem::rename(tmp_path, dest_path, ec);
      if (ec) {
        std::filesystem::remove(tmp_path, ec);
        failed = true;
        fail_headline = "RENAME FAILED";
        fail_detail = TruncateDisplayName(filename, 16);
        break;
      }

      ++files_done;
      deletable_files.push_back(src_path);
      std::lock_guard<std::mutex> lock(transfer.mutex);
      transfer.state.files_done = files_done;
      transfer.state.files_deletable = static_cast<int>(deletable_files.size());
      transfer.state.progress = std::to_string(files_done) + "/" +
                                std::to_string(transfer.state.files_total) +
                                " " + std::to_string(transfer.state.progress_pct) + "%";
    }

    {
      std::lock_guard<std::mutex> lock(transfer.mutex);
      transfer.state.running = false;
      transfer.state.deleting = false;
      transfer.state.can_start = false;
      transfer.state.current_file.clear();
      transfer.state.files_done = files_done;
      transfer.state.files_skipped = files_skipped;
      transfer.state.files_deletable = static_cast<int>(deletable_files.size());
      transfer.state.files_deleted = 0;
      transfer.state.deletable_files = deletable_files;
      transfer.state.processed_bytes = processed_bytes;
      transfer.state.progress_pct =
          total_bytes == 0 ? 100
                           : static_cast<int>((processed_bytes * 100) / total_bytes);
      if (failed) {
        transfer.state.error = true;
        transfer.state.done = false;
        transfer.state.can_delete = false;
        transfer.state.headline = fail_headline;
        transfer.state.detail = fail_detail;
      } else {
        transfer.state.error = false;
        transfer.state.done = true;
        transfer.state.can_delete = !deletable_files.empty();
        transfer.state.headline = "TRANSFER DONE";
        transfer.state.detail = std::to_string(files_done - files_skipped) +
                                " COPIED " + std::to_string(files_skipped) +
                                " SKIP";
        transfer.state.progress = std::to_string(files_done) + "/" +
                                  std::to_string(transfer.state.files_total) +
                                  " 100%";
        if (transfer.state.can_delete) {
          transfer.state.current_file =
              std::to_string(transfer.state.files_deletable) + " SAFE TO DEL";
        }
      }
    }
    transfer.thread_running.store(false);
  });
  return true;
}

bool DeleteTransferredFiles(TransferSession& transfer) {
  JoinTransferThreadIfDone(transfer);
  std::vector<std::filesystem::path> files;
  {
    std::lock_guard<std::mutex> lock(transfer.mutex);
    if (transfer.thread_running.load() || !transfer.state.can_delete ||
        transfer.state.deletable_files.empty()) {
      return false;
    }
    files = transfer.state.deletable_files;
    transfer.state.running = false;
    transfer.state.deleting = true;
    transfer.state.error = false;
    transfer.state.done = false;
    transfer.state.can_start = false;
    transfer.state.can_delete = false;
    transfer.state.files_deleted = 0;
    transfer.state.progress_pct = 0;
    transfer.state.progress =
        "0/" + std::to_string(static_cast<int>(files.size()));
    transfer.state.current_file.clear();
    transfer.state.headline = "DELETING";
    transfer.state.detail = transfer.state.direction;
  }
  transfer.thread_running.store(true);
  transfer.thread = std::thread([&, files]() {
    bool failed = false;
    std::string fail_headline;
    std::string fail_detail;
    int files_deleted = 0;

    for (const auto& src_path : files) {
      std::error_code ec;
      const std::string filename = src_path.filename().string();
      {
        std::lock_guard<std::mutex> lock(transfer.mutex);
        transfer.state.current_file = TruncateDisplayName(filename, 18);
      }
      if (!std::filesystem::exists(src_path, ec) || ec) {
        ++files_deleted;
      } else {
        if (!std::filesystem::remove(src_path, ec) || ec) {
          failed = true;
          fail_headline = "DELETE FAILED";
          fail_detail = TruncateDisplayName(filename, 16);
          break;
        }
        ++files_deleted;
      }
      std::lock_guard<std::mutex> lock(transfer.mutex);
      transfer.state.files_deleted = files_deleted;
      transfer.state.progress_pct =
          static_cast<int>((files_deleted * 100) /
                           std::max(1, static_cast<int>(files.size())));
      transfer.state.progress = std::to_string(files_deleted) + "/" +
                                std::to_string(static_cast<int>(files.size())) +
                                " " + std::to_string(transfer.state.progress_pct) + "%";
    }

    {
      std::lock_guard<std::mutex> lock(transfer.mutex);
      transfer.state.deleting = false;
      transfer.state.current_file.clear();
      if (failed) {
        transfer.state.error = true;
        transfer.state.done = false;
        transfer.state.can_delete = true;
        transfer.state.headline = fail_headline;
        transfer.state.detail = fail_detail;
      } else {
        transfer.state.error = false;
        transfer.state.done = true;
        transfer.state.can_delete = false;
        transfer.state.deletable_files.clear();
        transfer.state.files_deletable = 0;
        transfer.state.files_deleted = files_deleted;
        transfer.state.headline = "DELETE DONE";
        transfer.state.detail = std::to_string(files_deleted) + " REMOVED";
        transfer.state.progress = std::to_string(files_deleted) + "/" +
                                  std::to_string(static_cast<int>(files.size())) +
                                  " 100%";
      }
    }
    transfer.thread_running.store(false);
  });
  return true;
}
