#include "client/file_system_client.h"
#include "common/logging.h"
#include "common/types.h"

#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

static void PrintUsage() {
  std::cout << "Usage: anycache-cli [--master <address>] <command> [args...]\n"
            << "\nOptions:\n"
            << "  --master <address>  Master server address "
               "(default: localhost:19999)\n"
            << "\nCommands:\n"
            << "  ls <path>           List directory contents\n"
            << "  mkdir <path>        Create directory\n"
            << "  stat <path>         Get file/directory info\n"
            << "  rm <path>           Delete file/directory\n"
            << "  mv <src> <dst>      Rename/move\n"
            << "  cat <path>          Output file content to stdout\n"
            << "  head <path> [-n N]  Output first N bytes (default 1024)\n"
            << "  tail <path> [-n N]  Output last N bytes (default 1024)\n"
            << "  touch <path>        Create empty file\n"
            << "  write <path> [--input FILE]  Write from stdin or file\n"
            << "  cp <src> <dst>      Copy file or directory\n"
            << "  location <path>     Show block locations\n"
            << "  test [-e|-d|-f] <path>  Test path, exit 0/1\n"
            << "  mount <path> <ufs_uri>  Mount UFS to anycache path\n"
            << "  unmount <path>          Unmount a path\n"
            << "  mountTable              List all mount points\n"
            << "  help                    Show this message\n";
}

int main(int argc, char *argv[]) {
  anycache::Logger::Init("cli", "", spdlog::level::warn);

  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  // Parse options
  std::string master_address = "localhost:19999";
  int arg_start = 1;

  if (std::string(argv[1]) == "--master" && argc >= 3) {
    master_address = argv[2];
    arg_start = 3;
  }

  if (arg_start >= argc) {
    PrintUsage();
    return 1;
  }

  std::string cmd = argv[arg_start];

  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    PrintUsage();
    return 0;
  }

  // Connect to Master via gRPC
  anycache::FileSystemClient client(master_address);

  if (cmd == "ls") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli ls <path>\n";
      return 1;
    }
    std::vector<anycache::ClientFileInfo> entries;
    auto s = client.ListStatus(argv[arg_start + 1], &entries);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    for (auto &e : entries) {
      std::cout << (e.is_directory ? "d" : "-") << " " << e.size << "\t"
                << e.name << "\n";
    }
  } else if (cmd == "mkdir") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli mkdir <path>\n";
      return 1;
    }
    auto s = client.Mkdir(argv[arg_start + 1], true);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Created: " << argv[arg_start + 1] << "\n";
  } else if (cmd == "stat") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli stat <path>\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[arg_start + 1], &info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Name: " << info.name << "\n"
              << "Type: " << (info.is_directory ? "directory" : "file") << "\n"
              << "Size: " << info.size << "\n"
              << "Mode: " << std::oct << info.mode << std::dec << "\n";
  } else if (cmd == "rm") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli rm <path>\n";
      return 1;
    }
    auto s = client.DeleteFile(argv[arg_start + 1], true);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Deleted: " << argv[arg_start + 1] << "\n";
  } else if (cmd == "mv") {
    if (arg_start + 2 >= argc) {
      std::cerr << "Usage: anycache-cli mv <src> <dst>\n";
      return 1;
    }
    auto s = client.RenameFile(argv[arg_start + 1], argv[arg_start + 2]);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Moved: " << argv[arg_start + 1] << " -> "
              << argv[arg_start + 2] << "\n";
  } else if (cmd == "cat") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli cat <path>\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[arg_start + 1], &info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (info.is_directory) {
      std::cerr << "Error: " << argv[arg_start + 1] << " is a directory\n";
      return 1;
    }
    std::vector<char> buf(info.size);
    size_t bytes_read = 0;
    s = client.ReadFile(argv[arg_start + 1], buf.data(), info.size, 0,
                        &bytes_read);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout.write(buf.data(), static_cast<std::streamsize>(bytes_read));
  } else if (cmd == "head") {
    size_t n = 1024;
    int path_idx = arg_start + 1;
    if (path_idx + 1 < argc && std::string(argv[path_idx]) == "-n") {
      n = static_cast<size_t>(std::stoull(argv[path_idx + 1]));
      path_idx += 2;
    }
    if (path_idx >= argc) {
      std::cerr << "Usage: anycache-cli head <path> [-n N]\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[path_idx], &info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (info.is_directory) {
      std::cerr << "Error: " << argv[path_idx] << " is a directory\n";
      return 1;
    }
    size_t to_read = std::min(n, static_cast<size_t>(info.size));
    if (to_read == 0)
      return 0;
    std::vector<char> buf(to_read);
    size_t bytes_read = 0;
    s = client.ReadFile(argv[path_idx], buf.data(), to_read, 0, &bytes_read);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout.write(buf.data(), static_cast<std::streamsize>(bytes_read));
  } else if (cmd == "tail") {
    size_t n = 1024;
    int path_idx = arg_start + 1;
    if (path_idx + 1 < argc && std::string(argv[path_idx]) == "-n") {
      n = static_cast<size_t>(std::stoull(argv[path_idx + 1]));
      path_idx += 2;
    }
    if (path_idx >= argc) {
      std::cerr << "Usage: anycache-cli tail <path> [-n N]\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[path_idx], &info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (info.is_directory) {
      std::cerr << "Error: " << argv[path_idx] << " is a directory\n";
      return 1;
    }
    if (info.size == 0)
      return 0;
    size_t to_read = std::min(n, static_cast<size_t>(info.size));
    off_t offset = static_cast<off_t>(info.size - to_read);
    std::vector<char> buf(to_read);
    size_t bytes_read = 0;
    s = client.ReadFile(argv[path_idx], buf.data(), to_read, offset,
                        &bytes_read);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout.write(buf.data(), static_cast<std::streamsize>(bytes_read));
  } else if (cmd == "touch") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli touch <path>\n";
      return 1;
    }
    anycache::InodeId id;
    anycache::WorkerId wid;
    auto s = client.CreateFileEx(argv[arg_start + 1], 0644, &id, &wid);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    s = client.CompleteFile(id, 0);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Created: " << argv[arg_start + 1] << "\n";
  } else if (cmd == "write") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli write <path> [--input FILE]\n";
      return 1;
    }
    const char *path = argv[arg_start + 1];
    std::vector<char> data;
    if (arg_start + 3 < argc && std::string(argv[arg_start + 2]) == "--input") {
      std::ifstream ifs(argv[arg_start + 3], std::ios::binary);
      if (!ifs) {
        std::cerr << "Error: cannot open " << argv[arg_start + 3] << "\n";
        return 1;
      }
      ifs.seekg(0, std::ios::end);
      size_t sz = static_cast<size_t>(ifs.tellg());
      ifs.seekg(0, std::ios::beg);
      data.resize(sz);
      ifs.read(data.data(), static_cast<std::streamsize>(sz));
    } else {
      char c;
      while (std::cin.get(c))
        data.push_back(c);
    }
    size_t written = 0;
    auto s = client.WriteFile(path, data.data(), data.size(), 0, &written);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    s = client.GetFileInfo(path, &info);
    if (s.ok())
      s = client.CompleteFile(info.inode_id, written);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Written " << written << " bytes to " << path << "\n";
  } else if (cmd == "cp") {
    if (arg_start + 2 >= argc) {
      std::cerr << "Usage: anycache-cli cp <src> <dst>\n";
      return 1;
    }
    anycache::ClientFileInfo src_info;
    auto s = client.GetFileInfo(argv[arg_start + 1], &src_info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (src_info.is_directory) {
      // Recursive directory copy
      std::function<int(const std::string &, const std::string &)> copy_dir;
      copy_dir = [&](const std::string &src_dir,
                     const std::string &dst_dir) -> int {
        auto ms = client.Mkdir(dst_dir, true);
        if (!ms.ok()) {
          std::cerr << "Error mkdir " << dst_dir << ": " << ms.ToString()
                    << "\n";
          return 1;
        }

        std::vector<anycache::ClientFileInfo> children;
        ms = client.ListStatus(src_dir, &children);
        if (!ms.ok()) {
          std::cerr << "Error listing " << src_dir << ": " << ms.ToString()
                    << "\n";
          return 1;
        }

        for (auto &child : children) {
          std::string child_src = src_dir;
          if (child_src.back() != '/')
            child_src += '/';
          child_src += child.name;
          std::string child_dst = dst_dir;
          if (child_dst.back() != '/')
            child_dst += '/';
          child_dst += child.name;

          if (child.is_directory) {
            if (copy_dir(child_src, child_dst) != 0)
              return 1;
          } else {
            // Copy file
            std::vector<char> fbuf(child.size);
            size_t fread = 0;
            ms = client.ReadFile(child_src, fbuf.data(), child.size, 0, &fread);
            if (!ms.ok()) {
              std::cerr << "Error reading " << child_src << ": "
                        << ms.ToString() << "\n";
              return 1;
            }
            size_t fwritten = 0;
            ms = client.WriteFile(child_dst, fbuf.data(), fread, 0, &fwritten);
            if (!ms.ok()) {
              std::cerr << "Error writing " << child_dst << ": "
                        << ms.ToString() << "\n";
              return 1;
            }
            anycache::ClientFileInfo di;
            ms = client.GetFileInfo(child_dst, &di);
            if (ms.ok())
              client.CompleteFile(di.inode_id, fwritten);
          }
        }
        return 0;
      };

      if (copy_dir(argv[arg_start + 1], argv[arg_start + 2]) != 0)
        return 1;
      std::cout << "Copied directory " << argv[arg_start + 1] << " -> "
                << argv[arg_start + 2] << "\n";
    } else {
      // Single file copy
      std::vector<char> buf(src_info.size);
      size_t bytes_read = 0;
      s = client.ReadFile(argv[arg_start + 1], buf.data(), src_info.size, 0,
                          &bytes_read);
      if (!s.ok() || bytes_read != static_cast<size_t>(src_info.size)) {
        std::cerr << "Error: " << s.ToString() << "\n";
        return 1;
      }
      size_t written = 0;
      s = client.WriteFile(argv[arg_start + 2], buf.data(), bytes_read, 0,
                           &written);
      if (!s.ok()) {
        std::cerr << "Error: " << s.ToString() << "\n";
        return 1;
      }
      anycache::ClientFileInfo dst_info;
      s = client.GetFileInfo(argv[arg_start + 2], &dst_info);
      if (!s.ok()) {
        std::cerr << "Error: " << s.ToString() << "\n";
        return 1;
      }
      s = client.CompleteFile(dst_info.inode_id, written);
      if (!s.ok()) {
        std::cerr << "Error: " << s.ToString() << "\n";
        return 1;
      }
      std::cout << "Copied " << argv[arg_start + 1] << " -> "
                << argv[arg_start + 2] << "\n";
    }
  } else if (cmd == "location") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli location <path>\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[arg_start + 1], &info);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (info.is_directory) {
      std::cerr << "Error: " << argv[arg_start + 1] << " is a directory\n";
      return 1;
    }
    uint32_t block_count = anycache::GetBlockCount(info.size);
    if (block_count == 0) {
      std::cout << "File has no blocks\n";
      return 0;
    }
    std::vector<anycache::BlockId> block_ids;
    for (uint32_t i = 0; i < block_count; ++i)
      block_ids.push_back(anycache::MakeBlockId(info.inode_id, i));
    std::vector<anycache::ClientBlockLocation> locs;
    s = client.GetBlockLocations(block_ids, &locs);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    for (const auto &loc : locs) {
      std::cout << "block " << anycache::GetBlockIndex(loc.block_id) << ": "
                << loc.worker_address << " (worker " << loc.worker_id << ", "
                << anycache::TierTypeName(loc.tier) << ")\n";
    }
  } else if (cmd == "test") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli test [-e|-d|-f] <path>\n";
      return 1;
    }
    const char *flag = nullptr;
    int path_idx = arg_start + 1;
    if (path_idx + 1 < argc) {
      std::string a(argv[path_idx]);
      if (a == "-e" || a == "-d" || a == "-f") {
        flag = argv[path_idx];
        path_idx++;
      }
    }
    if (path_idx >= argc) {
      std::cerr << "Usage: anycache-cli test [-e|-d|-f] <path>\n";
      return 1;
    }
    anycache::ClientFileInfo info;
    auto s = client.GetFileInfo(argv[path_idx], &info);
    bool exists = s.ok();
    if (!flag || std::string(flag) == "-e") {
      return exists ? 0 : 1;
    }
    if (std::string(flag) == "-d") {
      return (exists && info.is_directory) ? 0 : 1;
    }
    if (std::string(flag) == "-f") {
      return (exists && !info.is_directory) ? 0 : 1;
    }
    return 1;
  } else if (cmd == "mount") {
    if (arg_start + 2 >= argc) {
      std::cerr << "Usage: anycache-cli mount <path> <ufs_uri>\n";
      return 1;
    }
    auto s = client.Mount(argv[arg_start + 1], argv[arg_start + 2]);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Mounted " << argv[arg_start + 2] << " at "
              << argv[arg_start + 1] << "\n";
  } else if (cmd == "unmount") {
    if (arg_start + 1 >= argc) {
      std::cerr << "Usage: anycache-cli unmount <path>\n";
      return 1;
    }
    auto s = client.Unmount(argv[arg_start + 1]);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "Unmounted: " << argv[arg_start + 1] << "\n";
  } else if (cmd == "mountTable") {
    std::vector<std::pair<std::string, std::string>> mounts;
    auto s = client.GetMountTable(&mounts);
    if (!s.ok()) {
      std::cerr << "Error: " << s.ToString() << "\n";
      return 1;
    }
    if (mounts.empty()) {
      std::cout << "No mount points configured\n";
    } else {
      for (auto &[path, uri] : mounts) {
        std::cout << path << " -> " << uri << "\n";
      }
    }
  } else {
    std::cerr << "Unknown command: " << cmd << "\n";
    PrintUsage();
    return 1;
  }

  return 0;
}
