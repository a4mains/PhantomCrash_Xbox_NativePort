#include "install.hpp"

#include "binary.hpp"
#include "convert_steps.hpp"
#include "parallel.hpp"
#include "phantomcrash/game_data.hpp"
#include "phantomcrash/game_text.hpp"
#include "phantomcrash/json.hpp"
#include "sha256.hpp"
#include "xbe.hpp"
#include "xbe_profile.hpp"
#include "xdvdfs.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

namespace phantomcrash::extract {
namespace {

namespace fs = std::filesystem;

// Staging folders, beside the folders they replace; only this tool writes them.
constexpr const char *extract_staging = "extract.partial";
constexpr const char *convert_staging = "assets.partial";
constexpr const char *replaced_suffix = ".old";
// Room the converted assets need beyond the disc's files (about 0.4 GB).
constexpr std::uintmax_t assets_space = 1ull << 30;
constexpr std::size_t hash_chunk = 8 * 1024 * 1024;

void remove_owned(const fs::path &path) {
  std::error_code error;
  fs::remove_all(path, error);
  if (error) {
    throw std::runtime_error(
        std::format("Could not remove {}: {}", path.string(), error.message()));
  }
}

// Moves `staged` into place; the old `target` is moved aside first and put
// back if the move fails.
void replace_directory(const fs::path &staged, const fs::path &target) {
  const fs::path old = target.parent_path() / (target.filename().string() + replaced_suffix);
  remove_owned(old);
  std::error_code error;
  if (fs::exists(target)) {
    fs::rename(target, old, error);
    if (error) {
      throw std::runtime_error(
          std::format("Could not replace {} ({}). Close the game and try again.", target.string(),
                      error.message()));
    }
  }
  fs::rename(staged, target, error);
  if (error) {
    std::error_code restore;
    if (fs::exists(old)) {
      fs::rename(old, target, restore);
    }
    throw std::runtime_error(
        std::format("Could not move {} into place: {}", staged.string(), error.message()));
  }
  remove_owned(old);
}

void require_space(const fs::path &folder, const std::uintmax_t needed) {
  std::error_code error;
  const auto space = fs::space(folder, error);
  if (!error && space.available < needed) {
    throw std::runtime_error(
        std::format("Not enough free disk space in {}: {:.1f} GB needed, {:.1f} GB free",
                    folder.string(), needed / 1e9, space.available / 1e9));
  }
}

// Staging and replaced folders left by an interrupted run.
void remove_leftovers(const GameDataPaths &paths) {
  for (const fs::path &folder : {paths.europe / extract_staging, paths.europe / convert_staging}) {
    remove_owned(folder);
  }
  for (const fs::path &target : {paths.files, paths.metadata, paths.assets}) {
    const fs::path old = target.parent_path() / (target.filename().string() + replaced_suffix);
    if (!fs::exists(target) && fs::exists(old)) {
      fs::rename(old, target); // the run stopped between its two renames
    }
    remove_owned(old);
  }
}

void extract(const GameDataPaths &paths, const fs::path &disc, const Progress &report) {
  if (!fs::is_regular_file(disc)) {
    throw std::runtime_error("Disc image not found: " + disc.string());
  }
  fs::create_directories(paths.europe);
  const fs::path staging = paths.europe / extract_staging;
  remove_owned(staging);
  require_space(paths.europe, fs::file_size(disc) + assets_space);
  report("Extracting " + disc.filename().string());
  extract_disc(disc, staging, report);
  replace_directory(staging / "files", paths.files);
  replace_directory(staging / "metadata", paths.metadata);
  remove_owned(staging);
}

void convert(const fs::path &root, const GameDataPaths &paths, const Progress &report) {
  const fs::path staging = paths.europe / convert_staging;
  remove_owned(staging);
  report("Converting the game data");
  convert_assets(paths.files, staging, false, game_text_sources(root), report);
  replace_directory(staging, paths.assets);
}

std::string file_sha256(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  Sha256 hash;
  Bytes chunk(hash_chunk);
  while (in) {
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    hash.update({chunk.data(), static_cast<std::size_t>(in.gcount())});
  }
  return hash.hex();
}

std::string lower_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::optional<fs::path> find_child_ci(const fs::path &dir, std::string_view name, bool expect_dir) {
  std::error_code error;
  const fs::path exact = dir / name;
  if (expect_dir ? fs::is_directory(exact, error) : fs::is_regular_file(exact, error)) {
    return exact;
  }
  const std::string needle = lower_ascii(std::string(name));
  for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
    if (lower_ascii(utf8_name(it->path())) == needle &&
        (expect_dir ? it->is_directory(error) : it->is_regular_file(error))) {
      return it->path();
    }
  }
  return std::nullopt;
}

std::string detect_region(const fs::path &root, const InstallOptions &options) {
  if (options.source_dir) {
    const auto xbe_path = find_child_ci(*options.source_dir, "default.xbe", false);
    if (!xbe_path) {
      throw std::runtime_error("Source directory does not contain default.xbe: " +
                               options.source_dir->string());
    }
    return require_xbe_profile(read_file(*xbe_path)).artifacts_dir;
  }
  std::error_code error;
  if (options.disc && fs::is_regular_file(*options.disc, error)) {
    XdvdfsImage disc_image(*options.disc);
    for (const auto &entry : disc_image.files()) {
      if (lower_ascii(entry.path) == "/default.xbe") {
        return require_xbe_profile(disc_image.read(entry)).artifacts_dir;
      }
    }
    throw std::runtime_error("Disc image does not contain /default.xbe: " +
                             options.disc->string());
  }
  return game_data_paths(root).region;
}

void install_from_source(const GameDataPaths &paths, const fs::path &source_dir,
                         const Progress &report) {
  const auto xbe_src = find_child_ci(source_dir, "default.xbe", false);
  const auto media_src = find_child_ci(source_dir, "Media", true);
  if (!xbe_src || !media_src) {
    throw std::runtime_error(source_dir.string() +
                             " does not look like an extracted disc (default.xbe and Media/)");
  }
  fs::create_directories(paths.europe);
  const fs::path staging = paths.europe / extract_staging;
  remove_owned(staging);
  const fs::path files_root = staging / "files";
  fs::create_directories(files_root / "Media");
  report("Copying game files from " + source_dir.string());

  struct Entry {
    fs::path source;
    fs::path target;
    std::string disc_path;
  };
  std::vector<Entry> entries{{*xbe_src, files_root / "default.xbe", "/default.xbe"}};
  for (const auto &item : fs::recursive_directory_iterator(*media_src)) {
    if (!item.is_regular_file()) {
      continue;
    }
    const fs::path rel = item.path().lexically_relative(*media_src);
    entries.push_back({item.path(), files_root / "Media" / rel,
                       "/Media/" + rel.generic_string()});
  }
  std::ranges::sort(entries, [](const Entry &a, const Entry &b) {
    return a.disc_path < b.disc_path;
  });

  Json records = array();
  std::uint64_t total = 0;
  for (const auto &entry : entries) {
    fs::create_directories(entry.target.parent_path());
    fs::copy_file(entry.source, entry.target, fs::copy_options::overwrite_existing);
    const std::uint64_t size = fs::file_size(entry.target);
    total += size;
    push(records, object({{"path", text(entry.disc_path)},
                          {"size", integer(size)},
                          {"sha256", text(file_sha256(entry.target))}}));
  }

  write_json(staging / "metadata" / "disc.json",
             object({{"image", text(python_path(source_dir))},
                     {"file_count", integer(entries.size())},
                     {"total_bytes", integer(total)},
                     {"files", records}}),
             2);
  const Bytes executable = read_file(files_root / "default.xbe");
  Json header = parse_xbe(executable).to_json();
  put(header, "sha256", text(sha256_hex(executable)));
  write_json(staging / "metadata" / "xbe.json", header, 2);

  replace_directory(staging / "files", paths.files);
  replace_directory(staging / "metadata", paths.metadata);
  remove_owned(staging);
}

std::string verify_files_at(const GameDataPaths &paths) {
  const json::JsonValue record = json::read_file(paths.metadata / "disc.json");
  const auto &files = record.find("files")->array();
  std::vector<std::string> mismatches(files.size());
  parallel_for(files.size(), [&](const std::size_t index) {
    const std::string &name = files[index].find("path")->string();
    const fs::path path = paths.files / utf8_path(std::string_view(name).substr(1));
    if (file_sha256(path) != files[index].find("sha256")->string()) {
      mismatches[index] = name;
    }
  });
  for (const std::string &name : mismatches) {
    if (!name.empty()) {
      return name;
    }
  }
  return {};
}

} // namespace

std::string verify_extracted_files(const fs::path &root) {
  return verify_files_at(game_data_paths(root));
}

InstallResult install_game_data(const fs::path &root, const InstallOptions &options,
                                const Progress &progress) {
  const Progress report = progress ? progress : [](const std::string &) {};
  const std::string region = detect_region(root, options);
  const GameDataPaths paths = game_data_paths_for_region(root, region);
  remove_leftovers(paths);
  const GameDataStatus status = game_data_status(root, region);
  InstallResult result;
  result.region = region;
  bool files_valid = status.files;
  if (files_valid && options.verify) {
    report("Verifying the extracted files");
    if (const std::string changed = verify_files_at(paths); !changed.empty()) {
      report("Changed since extraction: " + changed);
      files_valid = false;
    }
  }
  if (!files_valid) {
    if (options.source_dir) {
      install_from_source(paths, *options.source_dir, report);
      result.extracted = true;
    } else if (options.disc) {
      extract(paths, *options.disc, report);
      result.extracted = true;
    } else {
      throw DiscNeeded(
          status.files
              ? "The extracted files have changed; the disc image is needed to extract them again."
              : "The disc image is needed: " + status.problem + ".");
    }
  }
  if (result.extracted || !status.assets || options.convert) {
    if (!status.assets && !result.extracted) {
      report("Updating the converted assets: " + status.problem);
    }
    convert(root, paths, report);
    result.converted = true;
  }
  return result;
}

} // namespace phantomcrash::extract
