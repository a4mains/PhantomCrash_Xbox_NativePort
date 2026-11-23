#include "desktop_game_data.hpp"

#include "phantomcrash/game_data.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace phantomcrash {
namespace {

constexpr const char *dialog_title = "Phantom Crash";
#ifdef _WIN32
constexpr const wchar_t *extractor_name = L"phantomcrash_extract.exe";
// A full install takes seconds; this only stops a hung installer holding the game.
constexpr DWORD install_timeout_ms = 30 * 60 * 1000;
#else
constexpr const char *extractor_name = "phantomcrash_extract";
#endif
constexpr Uint32 dialog_poll_ms = 15;

enum Choice : int { quit = 0, proceed = 1 };

// Shows a question with "Quit" and one other button; true for the other.
bool ask(const std::string &message, const char *proceed_label) {
  const SDL_MessageBoxButtonData buttons[]{
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, quit, "Quit"},
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, proceed, proceed_label},
  };
  const SDL_MessageBoxData data{SDL_MESSAGEBOX_INFORMATION, nullptr, dialog_title, message.c_str(),
                                SDL_arraysize(buttons),     buttons, nullptr};
  int chosen = quit;
  return SDL_ShowMessageBox(&data, &chosen) && chosen == proceed;
}

struct DialogResult {
  std::atomic<bool> done{};
  std::optional<std::filesystem::path> path;
};

// The open-file dialog runs on its own thread on Windows; wait for its answer.
std::optional<std::filesystem::path> choose_disc_image() {
  static constexpr SDL_DialogFileFilter filters[]{{"Xbox disc images", "iso;xiso"},
                                                  {"All files", "*"}};
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    throw std::runtime_error(std::string("Unable to open the file dialog: ") + SDL_GetError());
  }
  DialogResult result;
  SDL_ShowOpenFileDialog(
      [](void *user, const char *const *files, int) {
        auto *answer = static_cast<DialogResult *>(user);
        if (files != nullptr && files[0] != nullptr) {
          answer->path = std::filesystem::path(reinterpret_cast<const char8_t *>(files[0]));
        }
        answer->done = true;
      },
      &result, nullptr, filters, static_cast<int>(SDL_arraysize(filters)), nullptr, false);
  while (!result.done) {
    SDL_PumpEvents();
    SDL_Delay(dialog_poll_ms);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return result.path;
}

#ifdef _WIN32
std::wstring quoted(const std::filesystem::path &path) {
  std::wstring text = path.wstring();
  // A trailing backslash would escape the closing quote.
  if (text.ends_with(L'\\')) {
    text.push_back(L'\\');
  }
  return L"\"" + text + L"\"";
}
#endif

// Runs the installer in a console window of its own; its exit code, or -1.
int run_installer(const std::filesystem::path &root,
                  const std::optional<std::filesystem::path> &disc) {
  const char *base = SDL_GetBasePath();
  const std::filesystem::path folder =
      base != nullptr ? std::filesystem::path(reinterpret_cast<const char8_t *>(base)) : root;
  const std::filesystem::path extractor = folder / extractor_name;
  if (!std::filesystem::is_regular_file(extractor)) {
    throw std::runtime_error("The game data needs installing, but " + extractor.string() +
                             " is missing.");
  }
#ifdef _WIN32
  std::wstring command = quoted(extractor) + L" install --pause-on-error --root " + quoted(root);
  if (disc) {
    command += L" --disc " + quoted(*disc);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(extractor.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process)) {
    throw std::runtime_error("Unable to start " + extractor.string() + " (error " +
                             std::to_string(GetLastError()) + ")");
  }
  CloseHandle(process.hThread);
  DWORD code = static_cast<DWORD>(-1);
  if (WaitForSingleObject(process.hProcess, install_timeout_ms) == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &code);
  } else {
    TerminateProcess(process.hProcess, code);
  }
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
#else
  std::vector<std::string> storage{extractor.string(), "install", "--root", root.string()};
  if (disc) {
    storage.emplace_back("--disc");
    storage.push_back(disc->string());
  }
  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (std::string &arg : storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) {
    throw std::runtime_error("Unable to fork " + extractor.string());
  }
  if (pid == 0) {
    execv(extractor.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

bool prepare_game_data(const std::filesystem::path &root, const bool interactive) {
  GameDataStatus status = game_data_status(root);
  if (status.files && status.assets) {
    return true;
  }
  if (!interactive) {
    SDL_Log("Game data is not current: %s", status.problem.c_str());
    return true;
  }
  std::optional<std::filesystem::path> disc;
  for (;;) {
    if (!status.files) {
      const bool first = !disc;
      if (!ask(first
                   ? "The game data has not been installed yet. Choose your Phantom Crash (Europe or "
                     "USA) disc image (.iso) to install it; this is needed only once."
                   : "That disc image could not be installed. Choose another?",
               "Choose disc image...")) {
        return false;
      }
      disc = choose_disc_image();
      if (!disc) {
        continue;
      }
    }
    const int code = run_installer(root, disc);
    status = game_data_status(root);
    if (status.files && status.assets) {
      return true;
    }
    if (status.files && !ask("Installing the game data did not finish (exit code " +
                                 std::to_string(code) + "): " + status.problem,
                             "Try again")) {
      return false;
    }
  }
}

} // namespace phantomcrash
