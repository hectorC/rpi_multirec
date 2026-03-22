#pragma once

#include "common.h"
#include "types.h"

inline constexpr const char* kDefaultRecordingsDir = "/srv/rpi_multirec/recordings";
inline std::string g_recordings_dir = kDefaultRecordingsDir;

inline const std::string& RecordingsDir() { return g_recordings_dir; }
inline void SetRecordingsDir(const std::string& path) {
  g_recordings_dir = path.empty() ? std::string(kDefaultRecordingsDir) : path;
}

std::string ToLowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string BuildAutoOutPath(MicKind mic) {
  const char* mic_name = MicKindToFilePrefix(mic);
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
#ifdef _WIN32
  localtime_s(&tm_now, &now);
#else
  localtime_r(&now, &tm_now);
#endif
  std::ostringstream oss;
  oss << RecordingsDir() << "/" << mic_name << "_"
      << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".rf64";
  return oss.str();
}

std::string EnsureRecordingsPath(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  const std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  return (std::filesystem::path(RecordingsDir()) / p).string();
}

bool EnsureParentDirectoryExists(const std::string& file_path,
                                 std::string* error) {
  try {
    const std::filesystem::path p(file_path);
    const std::filesystem::path parent = p.parent_path();
    if (parent.empty()) {
      return true;
    }
    std::filesystem::create_directories(parent);
    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
}

bool GetFreeBytesForPath(const std::string& file_path, uint64_t* free_bytes) {
  if (!free_bytes) {
    return false;
  }
#ifdef __linux__
  try {
    std::filesystem::path p(file_path);
    std::filesystem::path dir = p;
    if (!std::filesystem::is_directory(dir)) {
      dir = p.parent_path();
    }
    if (dir.empty()) {
      dir = std::filesystem::path(RecordingsDir());
    }
    struct statvfs s {};
    if (statvfs(dir.c_str(), &s) != 0) {
      return false;
    }
    *free_bytes =
        static_cast<uint64_t>(s.f_bavail) * static_cast<uint64_t>(s.f_frsize);
    return true;
  } catch (...) {
    return false;
  }
#else
  (void)file_path;
  return false;
#endif
}

std::string DecodeMountPath(const std::string& path) {
  std::string decoded;
  decoded.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '\\' && i + 3 < path.size() &&
        std::isdigit(static_cast<unsigned char>(path[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(path[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(path[i + 3]))) {
      const int value =
          (path[i + 1] - '0') * 64 + (path[i + 2] - '0') * 8 + (path[i + 3] - '0');
      decoded.push_back(static_cast<char>(value));
      i += 3;
    } else {
      decoded.push_back(path[i]);
    }
  }
  return decoded;
}

std::string DetectExternalRecordingsDir() {
#ifdef __linux__
  std::ifstream mounts("/proc/mounts");
  if (!mounts.is_open()) {
    return {};
  }

  std::string device;
  std::string mount_point;
  std::string fs_type;
  std::string options;
  while (mounts >> device >> mount_point >> fs_type >> options) {
    std::string rest;
    std::getline(mounts, rest);
    if (fs_type != "exfat" && fs_type != "exfat-fuse") {
      continue;
    }

    const std::filesystem::path mount_path(DecodeMountPath(mount_point));
    std::error_code ec;
    if (!std::filesystem::exists(mount_path, ec) ||
        !std::filesystem::is_directory(mount_path, ec)) {
      continue;
    }
    const std::filesystem::path candidate = mount_path / "rpi_multirec";
    std::filesystem::create_directories(candidate, ec);
    if (ec) {
      continue;
    }
    const auto perms = std::filesystem::status(candidate, ec).permissions();
    if (ec || perms == std::filesystem::perms::unknown ||
        perms == std::filesystem::perms::none) {
      continue;
    }
    if (access(candidate.c_str(), W_OK) != 0) {
      continue;
    }
    return candidate.string();
  }
#endif
  return {};
}

std::string BuildManualTakePath(const std::string& base_path, int take_index) {
  if (take_index <= 1) {
    return base_path;
  }
  const size_t slash = base_path.find_last_of("/\\");
  const size_t dot = base_path.find_last_of('.');
  const bool has_ext = (dot != std::string::npos) &&
                       (slash == std::string::npos || dot > slash);
  const std::string stem = has_ext ? base_path.substr(0, dot) : base_path;
  const std::string ext = has_ext ? base_path.substr(dot) : "";
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "_take%03d", take_index);
  return stem + suffix + ext;
}
