#include "check_git_status/app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace check_git_status {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string trim(std::string value) {
  const auto is_space = [](const unsigned char character) {
    return std::isspace(character) != 0;
  };
  const auto first = std::find_if_not(value.begin(), value.end(), is_space);
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (first >= last) {
    return {};
  }
  return {first, last};
}

std::string single_line(const std::string &value) {
  std::istringstream input(value);
  std::string result;
  for (std::string word; input >> word;) {
    if (!result.empty()) {
      result += ' ';
    }
    result += word;
  }
  return result;
}

CommandResult completed_result(std::string output, const int exit_code) {
  output = trim(std::move(output));
  if (exit_code == 0) {
    return {
        .output = std::move(output),
        .error = {},
        .exit_code = 0,
        .timed_out = false,
    };
  }

  std::string error;
  if (!output.empty()) {
    error = single_line(output) + ": ";
  }
  error += "exit status " + std::to_string(exit_code);
  return {
      .output = {},
      .error = std::move(error),
      .exit_code = exit_code,
      .timed_out = false,
  };
}

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string &value) {
  if (value.empty()) {
    return {};
  }
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::wstring quote_windows_argument(const std::wstring &argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }

  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted += character;
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted += character;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted += L'\"';
  return quoted;
}

std::string windows_error(const DWORD code) {
  return std::error_code(static_cast<int>(code), std::system_category())
      .message();
}

std::optional<fs::path> find_windows_git() {
  const DWORD path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (path_size == 0) {
    return std::nullopt;
  }
  std::wstring path_value(static_cast<std::size_t>(path_size), L'\0');
  GetEnvironmentVariableW(L"PATH", path_value.data(), path_size);
  if (!path_value.empty() && path_value.back() == L'\0') {
    path_value.pop_back();
  }

  std::size_t start = 0;
  while (start <= path_value.size()) {
    const auto separator = path_value.find(L';', start);
    auto directory = path_value.substr(start, separator - start);
    if (directory.size() >= 2 && directory.front() == L'\"' &&
        directory.back() == L'\"') {
      directory = directory.substr(1, directory.size() - 2);
    }
    fs::path candidate = directory.empty() ? fs::path(L"git.exe")
                                           : fs::path(directory) / L"git.exe";
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) {
      return fs::absolute(candidate, error).lexically_normal();
    }
    if (separator == std::wstring::npos) {
      break;
    }
    start = separator + 1;
  }
  return std::nullopt;
}

#else

std::optional<fs::path> find_posix_git() {
  const char *path_environment = std::getenv("PATH");
  if (path_environment == nullptr) {
    return std::nullopt;
  }

  const std::string path_value(path_environment);
  std::size_t start = 0;
  while (start <= path_value.size()) {
    const auto separator = path_value.find(':', start);
    const auto directory = path_value.substr(start, separator - start);
    const fs::path candidate =
        directory.empty() ? fs::path("git") : fs::path(directory) / "git";
    if (::access(candidate.c_str(), X_OK) == 0) {
      std::error_code error;
      return fs::absolute(candidate, error).lexically_normal();
    }
    if (separator == std::string::npos) {
      break;
    }
    start = separator + 1;
  }
  return std::nullopt;
}

#endif

} // namespace

ExecGitRunner::ExecGitRunner(fs::path binary) : binary_(std::move(binary)) {}

std::optional<fs::path> find_git_executable() {
#ifdef _WIN32
  return find_windows_git();
#else
  return find_posix_git();
#endif
}

CommandResult ExecGitRunner::run(const fs::path &directory,
                                 const std::vector<std::string> &arguments,
                                 const std::chrono::milliseconds timeout,
                                 const std::atomic_bool &cancelled) const {
  if (cancelled.load(std::memory_order_relaxed)) {
    return {
        .output = {},
        .error = "interrupted",
        .exit_code = -1,
        .timed_out = false,
    };
  }

#ifdef _WIN32
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (CreatePipe(&read_pipe, &write_pipe, &security, 0) == 0) {
    return {
        .output = {},
        .error = "create Git output pipe: " + windows_error(GetLastError()),
        .exit_code = -1,
        .timed_out = false,
    };
  }
  if (SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0) == 0) {
    const auto error = windows_error(GetLastError());
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return {
        .output = {},
        .error = "configure Git output pipe: " + error,
        .exit_code = -1,
        .timed_out = false,
    };
  }

  std::vector<std::wstring> command_arguments{
      binary_.wstring(),
      L"--no-optional-locks",
      L"-C",
      directory.wstring(),
  };
  command_arguments.reserve(command_arguments.size() + arguments.size());
  for (const auto &argument : arguments) {
    command_arguments.push_back(utf8_to_wide(argument));
  }

  std::wstring command_line;
  for (const auto &argument : command_arguments) {
    if (!command_line.empty()) {
      command_line += L' ';
    }
    command_line += quote_windows_argument(argument);
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  PROCESS_INFORMATION process{};

  if (CreateProcessW(binary_.c_str(), command_line.data(), nullptr, nullptr,
                     TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                     &process) == 0) {
    const auto error = windows_error(GetLastError());
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return {
        .output = {},
        .error = "start Git: " + error,
        .exit_code = -1,
        .timed_out = false,
    };
  }
  CloseHandle(write_pipe);

  std::string output;
  std::thread reader([&] {
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) !=
               0 &&
           bytes_read > 0) {
      output.append(buffer, static_cast<std::size_t>(bytes_read));
    }
  });

  const auto deadline = Clock::now() + timeout;
  bool was_cancelled = false;
  bool timed_out = false;
  std::string wait_error;
  for (;;) {
    if (cancelled.load(std::memory_order_relaxed)) {
      was_cancelled = true;
      TerminateProcess(process.hProcess, 130);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
    if (Clock::now() >= deadline) {
      timed_out = true;
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 25);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result == WAIT_FAILED) {
      wait_error = windows_error(GetLastError());
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
  }

  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  reader.join();
  CloseHandle(read_pipe);

  if (was_cancelled) {
    return {
        .output = {},
        .error = "interrupted",
        .exit_code = static_cast<int>(exit_code),
        .timed_out = false,
    };
  }
  if (timed_out) {
    return {
        .output = {},
        .error =
            "timed out after " +
            format_duration(
                std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)),
        .exit_code = static_cast<int>(exit_code),
        .timed_out = true,
    };
  }
  if (!wait_error.empty()) {
    return {
        .output = {},
        .error = "wait for Git: " + wait_error,
        .exit_code = static_cast<int>(exit_code),
        .timed_out = false,
    };
  }
  return completed_result(std::move(output), static_cast<int>(exit_code));

#else
  std::vector<std::string> command_arguments{
      binary_.string(),
      "--no-optional-locks",
      "-C",
      directory.string(),
  };
  command_arguments.insert(command_arguments.end(), arguments.begin(),
                           arguments.end());

  int output_pipe[2];
  if (::pipe(output_pipe) != 0) {
    return {
        .output = {},
        .error = "create Git output pipe: " + std::string(std::strerror(errno)),
        .exit_code = -1,
        .timed_out = false,
    };
  }

  const pid_t process = ::fork();
  if (process < 0) {
    const auto error = std::string(std::strerror(errno));
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    return {
        .output = {},
        .error = "start Git: " + error,
        .exit_code = -1,
        .timed_out = false,
    };
  }
  if (process == 0) {
    ::close(output_pipe[0]);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(output_pipe[1]);
    ::setenv("GIT_OPTIONAL_LOCKS", "0", 1);
    ::setenv("LC_ALL", "C", 1);

    std::vector<char *> argv;
    argv.reserve(command_arguments.size() + 1);
    for (auto &argument : command_arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    ::execv(binary_.c_str(), argv.data());
    const auto message = std::string("start Git: ") + std::strerror(errno);
    ::write(STDERR_FILENO, message.data(), message.size());
    ::_exit(127);
  }

  ::close(output_pipe[1]);
  std::string output;
  std::thread reader([&] {
    char buffer[4096];
    for (;;) {
      const auto bytes_read = ::read(output_pipe[0], buffer, sizeof(buffer));
      if (bytes_read <= 0) {
        return;
      }
      output.append(buffer, static_cast<std::size_t>(bytes_read));
    }
  });

  const auto deadline = Clock::now() + timeout;
  bool was_cancelled = false;
  bool timed_out = false;
  int process_status = 0;
  std::string wait_error;
  for (;;) {
    const auto wait_result = ::waitpid(process, &process_status, WNOHANG);
    if (wait_result == process) {
      break;
    }
    if (wait_result < 0) {
      wait_error = std::strerror(errno);
      ::kill(process, SIGKILL);
      ::waitpid(process, &process_status, 0);
      break;
    }
    if (cancelled.load(std::memory_order_relaxed)) {
      was_cancelled = true;
      ::kill(process, SIGKILL);
      ::waitpid(process, &process_status, 0);
      break;
    }
    if (Clock::now() >= deadline) {
      timed_out = true;
      ::kill(process, SIGKILL);
      ::waitpid(process, &process_status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  reader.join();
  ::close(output_pipe[0]);
  const int exit_code =
      WIFEXITED(process_status)
          ? WEXITSTATUS(process_status)
          : 128 + (WIFSIGNALED(process_status) ? WTERMSIG(process_status) : 0);

  if (was_cancelled) {
    return {
        .output = {},
        .error = "interrupted",
        .exit_code = exit_code,
        .timed_out = false,
    };
  }
  if (timed_out) {
    return {
        .output = {},
        .error =
            "timed out after " +
            format_duration(
                std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)),
        .exit_code = exit_code,
        .timed_out = true,
    };
  }
  if (!wait_error.empty()) {
    return {
        .output = {},
        .error = "wait for Git: " + wait_error,
        .exit_code = exit_code,
        .timed_out = false,
    };
  }
  return completed_result(std::move(output), exit_code);
#endif
}

} // namespace check_git_status
