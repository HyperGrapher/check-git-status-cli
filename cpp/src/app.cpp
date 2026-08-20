#include "check_git_status/app.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace check_git_status {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::string_view ansi_reset = "\x1b[0m";
constexpr std::string_view ansi_bold = "\x1b[1m";
constexpr std::string_view ansi_cyan = "\x1b[36m";
constexpr std::string_view ansi_green = "\x1b[32m";
constexpr std::string_view ansi_yellow = "\x1b[33m";
constexpr std::string_view ansi_red = "\x1b[31m";

struct TableCell {
  std::string text;
  std::string color;
};

std::string path_text(const fs::path &path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

fs::path path_from_utf8(const std::string &value) {
  const std::u8string utf8(reinterpret_cast<const char8_t *>(value.data()),
                           value.size());
  return fs::path(utf8);
}

std::size_t utf8_width(const std::string_view value) {
  return static_cast<std::size_t>(
      std::count_if(value.begin(), value.end(), [](const unsigned char byte) {
        return (byte & 0xc0U) != 0x80U;
      }));
}

#ifdef _WIN32

std::wstring extended_windows_path(const fs::path &path) {
  auto value = path.wstring();
  if (value.starts_with(L"\\\\?\\")) {
    return value;
  }
  if (value.starts_with(L"\\\\")) {
    return L"\\\\?\\UNC\\" + value.substr(2);
  }
  return L"\\\\?\\" + value;
}

std::string windows_error_message(const DWORD code) {
  return std::error_code(static_cast<int>(code), std::system_category())
      .message();
}

bool windows_git_marker_exists(const fs::path &marker,
                               std::vector<std::string> &warnings) {
  const DWORD attributes =
      GetFileAttributesW(extended_windows_path(marker).c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return true;
  }

  const DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    warnings.push_back("inspect \"" + path_text(marker) +
                       "\": " + windows_error_message(error));
  }
  return false;
}

void append_windows_directories(const fs::path &current,
                                std::vector<fs::path> &pending,
                                std::vector<std::string> &warnings) {
  const auto pattern = extended_windows_path(current / "*");
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                                   FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
  if (search == INVALID_HANDLE_VALUE &&
      GetLastError() == ERROR_INVALID_PARAMETER) {
    search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                              FindExSearchNameMatch, nullptr, 0);
  }
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      warnings.push_back("walk \"" + path_text(current) +
                         "\": " + windows_error_message(error));
    }
    return;
  }

  for (;;) {
    const std::wstring_view name(entry.cFileName);
    if (name != L"." && name != L".." &&
        (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      pending.push_back(current / entry.cFileName);
    }
    if (FindNextFileW(search, &entry) == 0) {
      break;
    }
  }

  const DWORD error = GetLastError();
  FindClose(search);
  if (error != ERROR_NO_MORE_FILES && error != ERROR_FILE_NOT_FOUND &&
      error != ERROR_PATH_NOT_FOUND) {
    warnings.push_back("walk \"" + path_text(current) +
                       "\": " + windows_error_message(error));
  }
}

#else

bool should_descend(const fs::directory_entry &entry, std::error_code &error) {
  if (!entry.is_directory(error) || error) {
    return false;
  }
  if (entry.is_symlink(error) || error) {
    return false;
  }
  return true;
}

#endif

std::vector<std::string> words(const std::string &value) {
  std::istringstream input(value);
  std::vector<std::string> result;
  for (std::string word; input >> word;) {
    result.push_back(std::move(word));
  }
  return result;
}

bool parse_worker_count(const std::string &value, unsigned int &workers) {
  unsigned long parsed = 0;
  const auto *begin = value.data();
  const auto *end = begin + value.size();
  const auto conversion = std::from_chars(begin, end, parsed);
  if (conversion.ec != std::errc{} || conversion.ptr != end || parsed == 0 ||
      parsed > std::numeric_limits<unsigned int>::max()) {
    return false;
  }
  workers = static_cast<unsigned int>(parsed);
  return true;
}

std::string checks_label(const std::vector<std::string> &checks) {
  if (checks.empty()) {
    return "repository check";
  }
  if (checks.size() == 1) {
    return checks.front() + " check";
  }

  std::string label;
  for (std::size_t index = 0; index < checks.size(); ++index) {
    if (index > 0) {
      label += '/';
    }
    label += checks[index];
  }
  return label + " checks";
}

void add_issue(std::vector<Issue> &issues, const std::string &check,
               const std::optional<std::string> &error) {
  if (!error) {
    return;
  }
  const auto existing =
      std::find_if(issues.begin(), issues.end(),
                   [&](const Issue &issue) { return issue.message == *error; });
  if (existing != issues.end()) {
    existing->checks.push_back(check);
    return;
  }
  issues.push_back(Issue{{check}, *error});
}

std::string command_timeout_message(const std::chrono::milliseconds timeout) {
  return "timed out after " +
         format_duration(
             std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
}

CommandResult
run_before_deadline(const GitRunner &runner, const fs::path &path,
                    const std::vector<std::string> &arguments,
                    const Clock::time_point deadline,
                    const std::chrono::milliseconds original_timeout,
                    const std::atomic_bool &cancelled) {
  if (cancelled.load(std::memory_order_relaxed)) {
    return {
        .output = {},
        .error = "interrupted",
        .exit_code = -1,
        .timed_out = false,
    };
  }

  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - Clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return {
        .output = {},
        .error = command_timeout_message(original_timeout),
        .exit_code = -1,
        .timed_out = true,
    };
  }
  return runner.run(path, arguments, remaining, cancelled);
}

std::vector<TableCell> status_row(const RepositoryStatus &status) {
  TableCell remote{status.remote_url, {}};
  if (status.remote_error) {
    remote = {"Error", std::string(ansi_red)};
  } else if (!status.has_remote) {
    remote = {"No Remote", std::string(ansi_red)};
  } else if (status.remote_url.empty()) {
    remote = {"Configured", std::string(ansi_green)};
  }

  TableCell worktree{"Clean", std::string(ansi_green)};
  if (status.worktree_error) {
    worktree = {"Error", std::string(ansi_red)};
  } else if (status.dirty) {
    worktree = {"Dirty", std::string(ansi_red)};
  }

  TableCell unpushed{"OK", std::string(ansi_green)};
  if (status.upstream_error || status.upstream_state == UpstreamState::error) {
    unpushed = {"Error", std::string(ansi_red)};
  } else if (status.upstream_state == UpstreamState::detached) {
    unpushed = {"Detached HEAD", std::string(ansi_yellow)};
  } else if (status.upstream_state == UpstreamState::missing) {
    unpushed = {"No Upstream", std::string(ansi_yellow)};
  } else if (status.ahead == 1) {
    unpushed = {"1 commit ahead", std::string(ansi_red)};
  } else if (status.ahead > 1) {
    unpushed = {std::to_string(status.ahead) + " commits ahead",
                std::string(ansi_red)};
  }

  return {{path_text(status.path), {}}, remote, worktree, unpushed};
}

bool supports_color(std::ostream &output) {
  const char *no_color = std::getenv("NO_COLOR");
  const char *term = std::getenv("TERM");
  if ((no_color != nullptr && *no_color != '\0') ||
      (term != nullptr && std::string_view(term) == "dumb") ||
      &output != &std::cout) {
    return false;
  }

#ifdef _WIN32
  if (_isatty(_fileno(stdout)) == 0) {
    return false;
  }
  const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  return console != INVALID_HANDLE_VALUE &&
         GetConsoleMode(console, &mode) != 0 &&
         SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) !=
             0;
#else
  return isatty(STDOUT_FILENO) != 0;
#endif
}

void write_wrapped(std::ostream &output, const std::string &message,
                   const std::size_t indent, const std::size_t max_width) {
  const std::string prefix(indent, ' ');
  std::string line = prefix;
  std::size_t line_width = indent;

  for (const auto &word : words(message)) {
    const auto word_width = utf8_width(word);
    const std::size_t separator_width = line_width > indent ? 1 : 0;
    if (line_width > indent &&
        line_width + separator_width + word_width > max_width) {
      output << line << '\n';
      line = prefix + word;
      line_width = indent + word_width;
      continue;
    }
    if (separator_width > 0) {
      line += ' ';
      ++line_width;
    }
    line += word;
    line_width += word_width;
  }

  if (line_width > indent) {
    output << line << '\n';
  }
}

std::string seconds_component(const long long milliseconds) {
  const auto seconds = milliseconds / 1000;
  const auto fraction = milliseconds % 1000;
  std::string result = std::to_string(seconds);
  if (fraction != 0) {
    std::ostringstream digits;
    digits << std::setw(3) << std::setfill('0') << fraction;
    auto fraction_text = digits.str();
    while (!fraction_text.empty() && fraction_text.back() == '0') {
      fraction_text.pop_back();
    }
    result += '.' + fraction_text;
  }
  return result + 's';
}

} // namespace

ParseResult parse_options(const std::vector<std::string> &arguments) {
  ParseResult result;
  const auto hardware = std::thread::hardware_concurrency();
  result.options.workers = std::clamp(hardware == 0 ? 4U : hardware, 4U, 32U);

  bool options_ended = false;
  bool root_set = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &argument = arguments[index];
    if (!options_ended && argument == "--") {
      options_ended = true;
      continue;
    }
    if (!options_ended && (argument == "-h" || argument == "--help")) {
      result.help_requested = true;
      return result;
    }
    if (!options_ended && (argument == "-d" || argument == "--dirty-only")) {
      result.options.dirty_only = true;
      continue;
    }
    if (!options_ended && argument == "--no-color") {
      result.options.no_color = true;
      continue;
    }

    std::optional<std::string> worker_value;
    if (!options_ended && (argument == "-j" || argument == "--workers")) {
      if (++index >= arguments.size()) {
        result.error = argument + " requires a value";
        return result;
      }
      worker_value = arguments[index];
    } else if (!options_ended && argument.starts_with("--workers=")) {
      worker_value = argument.substr(std::string("--workers=").size());
    } else if (!options_ended && argument.starts_with("-j=")) {
      worker_value = argument.substr(3);
    }
    if (worker_value) {
      if (!parse_worker_count(*worker_value, result.options.workers)) {
        result.error = "workers must be at least 1";
        return result;
      }
      continue;
    }

    if (!options_ended && argument.starts_with('-')) {
      result.error = "unknown option: " + argument;
      return result;
    }
    if (root_set) {
      result.error = "only one directory may be specified";
      return result;
    }
    result.options.root = argument;
    root_set = true;
  }
  return result;
}

void write_usage(std::ostream &output) {
  output
      << "Usage: check-git-status-cpp [options] [directory]\n\n"
      << "Recursively find Git repositories and summarize their status.\n\n"
      << "Options:\n"
      << "  -d, --dirty-only       show only repositories that need attention\n"
      << "  -j, --workers number   number of concurrent repository checks\n"
      << "      --no-color         disable colored output\n"
      << "  -h, --help             show this help\n";
}

std::string Issue::label() const { return checks_label(checks); }

bool RepositoryStatus::needs_attention() const {
  return dirty || ahead > 0 || !has_remote || remote_error || worktree_error ||
         upstream_error;
}

std::vector<Issue> RepositoryStatus::issues() const {
  std::vector<Issue> result;
  result.reserve(3);
  add_issue(result, "remote", remote_error);
  add_issue(result, "worktree", worktree_error);
  add_issue(result, "upstream", upstream_error);
  return result;
}

bool CommandResult::ok() const { return error.empty() && exit_code == 0; }

GitClient::GitClient(const GitRunner &runner,
                     const std::chrono::milliseconds timeout)
    : runner_(runner), timeout_(timeout) {}

RepositoryStatus GitClient::check(const fs::path &path,
                                  const std::atomic_bool &cancelled) const {
  RepositoryStatus status;
  status.path = path;
  const auto deadline = Clock::now() + timeout_;
  const auto run = [&](const std::vector<std::string> &arguments) {
    return run_before_deadline(runner_, path, arguments, deadline, timeout_,
                               cancelled);
  };

  const auto remotes_result = run({"remote"});
  if (!remotes_result.ok()) {
    status.remote_error = remotes_result.error;
  } else {
    const auto remotes = words(remotes_result.output);
    if (!remotes.empty()) {
      status.remote_name = remotes.front();
      if (std::find(remotes.begin(), remotes.end(), "origin") !=
          remotes.end()) {
        status.remote_name = "origin";
      }
      status.has_remote = true;
      const auto remote_url = run({"remote", "get-url", status.remote_name});
      if (remote_url.ok()) {
        status.remote_url = remote_url.output;
      } else {
        status.remote_error = remote_url.error;
      }
    }
  }

  const auto worktree = run({"status", "--porcelain"});
  if (worktree.ok()) {
    status.dirty = !worktree.output.empty();
  } else {
    status.worktree_error = worktree.error;
  }

  const auto branch = run({"symbolic-ref", "--quiet", "HEAD"});
  if (!branch.ok()) {
    if (branch.exit_code == 1) {
      status.upstream_state = UpstreamState::detached;
    } else {
      status.upstream_state = UpstreamState::error;
      status.upstream_error = branch.error;
    }
    return status;
  }

  const auto upstream =
      run({"for-each-ref", "--format=%(upstream)", branch.output});
  if (!upstream.ok()) {
    status.upstream_state = UpstreamState::error;
    status.upstream_error = upstream.error;
    return status;
  }
  if (upstream.output.empty()) {
    status.upstream_state = UpstreamState::missing;
    return status;
  }

  const auto ahead = run({"log", "--format=%H", "@{upstream}..HEAD"});
  if (!ahead.ok()) {
    status.upstream_state = UpstreamState::error;
    status.upstream_error = ahead.error;
    return status;
  }
  status.upstream_state = UpstreamState::ok;
  status.ahead = static_cast<int>(words(ahead.output).size());
  return status;
}

DiscoveryResult discover_repositories(const fs::path &root) {
  DiscoveryResult result;
  std::error_code error;
  const auto absolute_root = fs::absolute(root, error).lexically_normal();
  if (error) {
    result.error =
        "resolve scan root \"" + path_text(root) + "\": " + error.message();
    return result;
  }

  const auto root_status = fs::status(absolute_root, error);
  if (error) {
    result.error = "open scan root \"" + path_text(absolute_root) +
                   "\": " + error.message();
    return result;
  }
  if (!fs::is_directory(root_status)) {
    result.error =
        "scan root \"" + path_text(absolute_root) + "\" is not a directory";
    return result;
  }

  std::vector<fs::path> pending{absolute_root};
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();

    const auto marker = current / ".git";
#ifdef _WIN32
    if (windows_git_marker_exists(marker, result.warnings)) {
      result.repositories.push_back(std::move(current));
      continue;
    }
    append_windows_directories(current, pending, result.warnings);
#else
    error.clear();
    const auto marker_status = fs::status(marker, error);
    if (!error && (fs::is_directory(marker_status) ||
                   fs::is_regular_file(marker_status))) {
      result.repositories.push_back(std::move(current));
      continue;
    }
    if (error && error != std::errc::no_such_file_or_directory) {
      result.warnings.push_back("inspect \"" + path_text(marker) +
                                "\": " + error.message());
    }

    error.clear();
    fs::directory_iterator iterator(current, fs::directory_options::none,
                                    error);
    if (error) {
      result.warnings.push_back("walk \"" + path_text(current) +
                                "\": " + error.message());
      continue;
    }
    const fs::directory_iterator end;
    while (iterator != end) {
      const auto entry = *iterator;
      std::error_code status_error;
      if (should_descend(entry, status_error)) {
        pending.push_back(entry.path());
      } else if (status_error &&
                 status_error != std::errc::no_such_file_or_directory) {
        result.warnings.push_back("inspect \"" + path_text(entry.path()) +
                                  "\": " + status_error.message());
      }

      error.clear();
      iterator.increment(error);
      if (error) {
        result.warnings.push_back("walk \"" + path_text(current) +
                                  "\": " + error.message());
        break;
      }
    }
#endif
  }

  std::sort(result.repositories.begin(), result.repositories.end(),
            [](const fs::path &left, const fs::path &right) {
              return path_text(left) < path_text(right);
            });
  return result;
}

std::vector<RepositoryStatus>
check_repositories(const std::vector<fs::path> &paths, unsigned int workers,
                   const RepositoryChecker &checker,
                   const std::atomic_bool &cancelled) {
  if (paths.empty()) {
    return {};
  }

  workers =
      std::max(1U, std::min(workers, static_cast<unsigned int>(paths.size())));
  std::vector<RepositoryStatus> statuses(paths.size());
  std::atomic_size_t next_index = 0;
  std::vector<std::thread> threads;
  threads.reserve(workers);

  for (unsigned int worker = 0; worker < workers; ++worker) {
    threads.emplace_back([&] {
      while (!cancelled.load(std::memory_order_relaxed)) {
        const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
        if (index >= paths.size()) {
          return;
        }
        statuses[index] = checker(paths[index]);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  std::sort(statuses.begin(), statuses.end(),
            [](const RepositoryStatus &left, const RepositoryStatus &right) {
              return path_text(left.path) < path_text(right.path);
            });
  return statuses;
}

std::vector<RepositoryStatus>
filter_needs_attention(const std::vector<RepositoryStatus> &statuses) {
  std::vector<RepositoryStatus> filtered;
  std::copy_if(
      statuses.begin(), statuses.end(), std::back_inserter(filtered),
      [](const RepositoryStatus &status) { return status.needs_attention(); });
  return filtered;
}

std::size_t
count_needs_attention(const std::vector<RepositoryStatus> &statuses) {
  return static_cast<std::size_t>(std::count_if(
      statuses.begin(), statuses.end(),
      [](const RepositoryStatus &status) { return status.needs_attention(); }));
}

void render_table(std::ostream &output,
                  const std::vector<RepositoryStatus> &statuses,
                  const bool color) {
  std::vector<std::vector<TableCell>> rows;
  rows.reserve(statuses.size() + 1);
  rows.push_back({
      {"REPOSITORY", std::string(ansi_bold) + std::string(ansi_cyan)},
      {"REMOTE", std::string(ansi_bold) + std::string(ansi_cyan)},
      {"WORKTREE", std::string(ansi_bold) + std::string(ansi_cyan)},
      {"UNPUSHED", std::string(ansi_bold) + std::string(ansi_cyan)},
  });
  for (const auto &status : statuses) {
    rows.push_back(status_row(status));
  }

  std::vector<std::size_t> widths(4, 0);
  for (const auto &row : rows) {
    for (std::size_t column = 0; column < row.size(); ++column) {
      widths[column] = std::max(widths[column], utf8_width(row[column].text));
    }
  }

  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    const auto &row = rows[row_index];
    for (std::size_t column = 0; column < row.size(); ++column) {
      std::string value = row[column].text;
      if (column + 1 < row.size()) {
        value.append(widths[column] - utf8_width(row[column].text) + 2, ' ');
      }
      if (color && !row[column].color.empty()) {
        output << row[column].color << value << ansi_reset;
      } else {
        output << value;
      }
    }
    output << '\n';
    if (row_index == 0) {
      const auto total_width = widths[0] + widths[1] + widths[2] + widths[3] +
                               2 * (widths.size() - 1);
      output << std::string(total_width, '-') << '\n';
    }
  }
}

std::string format_duration(const std::chrono::nanoseconds duration) {
  if (duration <= std::chrono::nanoseconds::zero()) {
    return "0s";
  }
  if (duration < std::chrono::milliseconds(1)) {
    return "<1ms";
  }

  const auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(
      duration + std::chrono::microseconds(500));
  auto milliseconds = rounded.count();
  if (milliseconds < 1000) {
    return std::to_string(milliseconds) + "ms";
  }
  if (milliseconds < 60'000) {
    return seconds_component(milliseconds);
  }

  const auto hours = milliseconds / 3'600'000;
  milliseconds %= 3'600'000;
  const auto minutes = milliseconds / 60'000;
  milliseconds %= 60'000;

  std::string result;
  if (hours > 0) {
    result += std::to_string(hours) + 'h';
  }
  result += std::to_string(minutes) + 'm';
  result += seconds_component(milliseconds);
  return result;
}

void write_timing_report(std::ostream &output,
                         const std::chrono::nanoseconds discovery,
                         const std::chrono::nanoseconds checks,
                         const std::chrono::nanoseconds total) {
  output << "Timing: discovery " << format_duration(discovery) << "; checks "
         << format_duration(checks) << "; total " << format_duration(total)
         << ".\n";
}

void write_walk_warnings(std::ostream &output,
                         const std::vector<std::string> &warnings) {
  for (const auto &warning : warnings) {
    output << "warning: " << warning << '\n';
  }
}

std::size_t
write_check_warnings(std::ostream &output,
                     const std::vector<RepositoryStatus> &statuses) {
  std::size_t count = 0;
  bool wrote_status = false;
  for (const auto &status : statuses) {
    const auto issues = status.issues();
    if (issues.empty()) {
      continue;
    }
    if (wrote_status) {
      output << '\n';
    }
    output << "warning: " << path_text(status.path) << '\n';
    for (const auto &issue : issues) {
      output << "  " << issue.label() << ":\n";
      write_wrapped(output, issue.message, 4, 100);
      ++count;
    }
    wrote_status = true;
  }
  return count;
}

int run_cli(const std::vector<std::string> &arguments,
            std::ostream &standard_output, std::ostream &error_output,
            const std::atomic_bool &cancelled) {
  const auto run_started = Clock::now();
  const auto parsed = parse_options(arguments);
  if (parsed.help_requested) {
    write_usage(standard_output);
    return 0;
  }
  if (!parsed.error.empty()) {
    write_usage(error_output);
    error_output << "error: " << parsed.error << '\n';
    return 2;
  }

  const auto git_binary = find_git_executable();
  if (!git_binary) {
    error_output
        << "error: Git was not found in PATH; install Git and try again\n";
    return 1;
  }

  const auto discovery_started = Clock::now();
  const auto discovery =
      discover_repositories(path_from_utf8(parsed.options.root));
  const auto discovery_duration = Clock::now() - discovery_started;
  if (!discovery.error.empty()) {
    error_output << "error: " << discovery.error << '\n';
    return 1;
  }

  if (discovery.repositories.empty()) {
    standard_output << "No Git repositories found under " << parsed.options.root
                    << ".\n";
    write_walk_warnings(error_output, discovery.warnings);
    write_timing_report(standard_output, discovery_duration,
                        std::chrono::nanoseconds::zero(),
                        Clock::now() - run_started);
    return discovery.warnings.empty() ? 0 : 1;
  }

  const ExecGitRunner runner(*git_binary);
  const GitClient client(runner, std::chrono::seconds(15));
  const auto checks_started = Clock::now();
  const auto statuses = check_repositories(
      discovery.repositories, parsed.options.workers,
      [&](const fs::path &path) { return client.check(path, cancelled); },
      cancelled);
  const auto checks_duration = Clock::now() - checks_started;

  if (cancelled.load(std::memory_order_relaxed)) {
    error_output << "error: scan interrupted\n";
    return 130;
  }

  const auto displayed =
      parsed.options.dirty_only ? filter_needs_attention(statuses) : statuses;
  if (displayed.empty()) {
    standard_output << "No repositories need attention.\n";
  } else {
    render_table(standard_output, displayed,
                 !parsed.options.no_color && supports_color(standard_output));
  }
  standard_output << "\nScanned " << statuses.size() << " repositories; "
                  << count_needs_attention(statuses) << " need attention.\n";

  write_walk_warnings(error_output, discovery.warnings);
  const auto check_errors = write_check_warnings(error_output, statuses);
  write_timing_report(standard_output, discovery_duration, checks_duration,
                      Clock::now() - run_started);
  return discovery.warnings.empty() && check_errors == 0 ? 0 : 1;
}

} // namespace check_git_status
