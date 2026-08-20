#include "check_git_status/app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using check_git_status::CommandResult;
using check_git_status::GitRunner;
using check_git_status::RepositoryStatus;
using check_git_status::UpstreamState;

int failures = 0;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string path_text(const fs::path &path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

CommandResult successful_command(std::string output) {
  return {
      .output = std::move(output),
      .error = {},
      .exit_code = 0,
      .timed_out = false,
  };
}

CommandResult failed_command(std::string error, const int exit_code) {
  return {
      .output = {},
      .error = std::move(error),
      .exit_code = exit_code,
      .timed_out = false,
  };
}

struct TemporaryDirectory {
  fs::path path;

  TemporaryDirectory() {
    path = fs::temp_directory_path() /
           ("check-git-status-cpp-tests-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

class FakeGitRunner final : public GitRunner {
public:
  CommandResult run(const fs::path &, const std::vector<std::string> &arguments,
                    std::chrono::milliseconds,
                    const std::atomic_bool &) const override {
    if (arguments == std::vector<std::string>{"remote"}) {
      return successful_command("backup\norigin");
    }
    if (arguments == std::vector<std::string>{"remote", "get-url", "origin"}) {
      return successful_command("https://example.com/project.git");
    }
    if (arguments == std::vector<std::string>{"status", "--porcelain"}) {
      return successful_command(" M tracked.txt");
    }
    if (arguments ==
        std::vector<std::string>{"symbolic-ref", "--quiet", "HEAD"}) {
      return successful_command("refs/heads/main");
    }
    if (arguments == std::vector<std::string>{"for-each-ref",
                                              "--format=%(upstream)",
                                              "refs/heads/main"}) {
      return successful_command("refs/remotes/origin/main");
    }
    if (arguments ==
        std::vector<std::string>{"log", "--format=%H", "@{upstream}..HEAD"}) {
      return successful_command("first\nsecond");
    }
    return failed_command("unexpected Git command", 2);
  }
};

void test_parse_options() {
  const auto parsed = check_git_status::parse_options(
      {"-d", "--no-color", "-j", "3", "projects"});
  expect(parsed.error.empty(), "valid options should parse");
  expect(parsed.options.dirty_only, "-d should enable dirty-only output");
  expect(parsed.options.no_color, "--no-color should disable color");
  expect(parsed.options.workers == 3, "-j should set the worker count");
  expect(parsed.options.root == "projects",
         "the positional directory should be retained");

  const auto invalid_workers =
      check_git_status::parse_options({"--workers", "0"});
  expect(!invalid_workers.error.empty(), "zero workers should be rejected");

  const auto extra_root = check_git_status::parse_options({".", "projects"});
  expect(!extra_root.error.empty(),
         "multiple positional directories should be rejected");
}

void test_git_client_status() {
  const FakeGitRunner runner;
  const check_git_status::GitClient client(runner, 15s);
  const std::atomic_bool cancelled = false;
  const auto status = client.check("project", cancelled);

  expect(status.has_remote, "Git client should find a remote");
  expect(status.remote_name == "origin", "origin should be preferred");
  expect(status.remote_url == "https://example.com/project.git",
         "Git client should read the remote URL");
  expect(status.dirty, "porcelain output should mark the worktree dirty");
  expect(status.upstream_state == UpstreamState::ok,
         "upstream should be tracked");
  expect(status.ahead == 2, "Git client should count unpushed commits");
}

void test_issue_consolidation() {
  RepositoryStatus status;
  status.remote_error = "repository is inaccessible";
  status.worktree_error = "repository is inaccessible";
  status.upstream_error = "upstream failed separately";

  const auto issues = status.issues();
  expect(issues.size() == 2, "duplicate errors should be consolidated");
  expect(issues[0].label() == "remote/worktree checks",
         "consolidated issue should list its checks");
  expect(issues[1].label() == "upstream check",
         "a distinct error should remain separate");
}

void test_duration_formatting() {
  expect(check_git_status::format_duration(0ns) == "0s",
         "zero duration formatting");
  expect(check_git_status::format_duration(500us) == "<1ms",
         "sub-ms formatting");
  expect(check_git_status::format_duration(1499us) == "1ms",
         "millisecond rounding");
  expect(check_git_status::format_duration(2345ms) == "2.345s",
         "second formatting");
  expect(check_git_status::format_duration(62345ms) == "1m2.345s",
         "minute formatting");
}

void test_warning_layout() {
  RepositoryStatus status;
  status.path = "long-repository-path";
  std::string long_error;
  for (int index = 0; index < 12; ++index) {
    long_error += "repository access failed while checking ownership ";
  }
  status.remote_error = long_error;
  status.worktree_error = status.remote_error;

  std::ostringstream output;
  const auto count = check_git_status::write_check_warnings(output, {status});
  expect(count == 1, "a consolidated warning should count once");
  expect(output.str().starts_with(
             "warning: long-repository-path\n  remote/worktree checks:\n    "),
         "warning should use a structured block");

  std::istringstream lines(output.str());
  for (std::string line; std::getline(lines, line);) {
    expect(line.size() <= 100,
           "wrapped warning lines should not exceed 100 columns");
  }
}

void test_repository_discovery() {
  TemporaryDirectory temporary;
  const auto outer = temporary.path / "alpha";
  const auto nested = outer / "nested";
  const auto worktree = temporary.path / "beta" / "worktree";
  fs::create_directories(outer / ".git");
  fs::create_directories(nested / ".git");
  fs::create_directories(worktree);
  std::ofstream(worktree / ".git") << "gitdir: elsewhere";

  const auto discovery =
      check_git_status::discover_repositories(temporary.path);
  expect(discovery.error.empty(), "repository discovery should succeed");
  expect(discovery.warnings.empty(), "repository discovery should not warn");
  expect(discovery.repositories.size() == 2,
         "discovery should stop at repository boundaries");
  if (discovery.repositories.size() == 2) {
    expect(path_text(discovery.repositories[0]) == path_text(outer),
           "ordinary repository should be discovered");
    expect(path_text(discovery.repositories[1]) == path_text(worktree),
           "worktree with a .git file should be discovered");
  }
}

void test_bounded_concurrency() {
  const std::vector<fs::path> paths{"d", "c", "b", "a"};
  std::atomic_int active = 0;
  std::atomic_int maximum = 0;
  const std::atomic_bool cancelled = false;

  const auto statuses = check_git_status::check_repositories(
      paths, 2,
      [&](const fs::path &path) {
        const int current = active.fetch_add(1) + 1;
        int observed = maximum.load();
        while (observed < current &&
               !maximum.compare_exchange_weak(observed, current)) {
        }
        std::this_thread::sleep_for(20ms);
        active.fetch_sub(1);
        RepositoryStatus status;
        status.path = path;
        status.has_remote = true;
        return status;
      },
      cancelled);

  expect(maximum.load() == 2,
         "worker pool should enforce its concurrency bound");
  expect(statuses.size() == 4, "all repositories should be checked");
  if (statuses.size() == 4) {
    expect(path_text(statuses[0].path) == "a", "results should be sorted");
    expect(path_text(statuses[3].path) == "d", "results should be sorted");
  }
}

} // namespace

int main() {
  test_parse_options();
  test_git_client_status();
  test_issue_consolidation();
  test_duration_formatting();
  test_warning_layout();
  test_repository_discovery();
  test_bounded_concurrency();

  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "All C++ tests passed\n";
  return 0;
}
