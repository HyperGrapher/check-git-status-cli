#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace check_git_status {

struct Options {
  bool dirty_only = false;
  bool no_color = false;
  unsigned int workers = 4;
  std::string root = ".";
};

struct ParseResult {
  Options options;
  bool help_requested = false;
  std::string error;
};

ParseResult parse_options(const std::vector<std::string> &arguments);
void write_usage(std::ostream &output);

enum class UpstreamState {
  ok,
  missing,
  detached,
  error,
};

struct Issue {
  std::vector<std::string> checks;
  std::string message;

  [[nodiscard]] std::string label() const;
};

struct RepositoryStatus {
  std::filesystem::path path;
  std::string remote_url;
  std::string remote_name;
  bool has_remote = false;
  std::optional<std::string> remote_error;
  bool dirty = false;
  std::optional<std::string> worktree_error;
  UpstreamState upstream_state = UpstreamState::ok;
  int ahead = 0;
  std::optional<std::string> upstream_error;

  [[nodiscard]] bool needs_attention() const;
  [[nodiscard]] std::vector<Issue> issues() const;
};

struct CommandResult {
  std::string output;
  std::string error;
  int exit_code = -1;
  bool timed_out = false;

  [[nodiscard]] bool ok() const;
};

class GitRunner {
public:
  virtual ~GitRunner() = default;

  virtual CommandResult run(const std::filesystem::path &directory,
                            const std::vector<std::string> &arguments,
                            std::chrono::milliseconds timeout,
                            const std::atomic_bool &cancelled) const = 0;
};

class ExecGitRunner final : public GitRunner {
public:
  explicit ExecGitRunner(std::filesystem::path binary);

  CommandResult run(const std::filesystem::path &directory,
                    const std::vector<std::string> &arguments,
                    std::chrono::milliseconds timeout,
                    const std::atomic_bool &cancelled) const override;

private:
  std::filesystem::path binary_;
};

std::optional<std::filesystem::path> find_git_executable();

class GitClient {
public:
  GitClient(const GitRunner &runner, std::chrono::milliseconds timeout);

  [[nodiscard]] RepositoryStatus check(const std::filesystem::path &path,
                                       const std::atomic_bool &cancelled) const;

private:
  const GitRunner &runner_;
  std::chrono::milliseconds timeout_;
};

struct DiscoveryResult {
  std::vector<std::filesystem::path> repositories;
  std::vector<std::string> warnings;
  std::string error;
};

DiscoveryResult discover_repositories(const std::filesystem::path &root);

using RepositoryChecker =
    std::function<RepositoryStatus(const std::filesystem::path &)>;

std::vector<RepositoryStatus>
check_repositories(const std::vector<std::filesystem::path> &paths,
                   unsigned int workers, const RepositoryChecker &checker,
                   const std::atomic_bool &cancelled);

std::vector<RepositoryStatus>
filter_needs_attention(const std::vector<RepositoryStatus> &statuses);
std::size_t
count_needs_attention(const std::vector<RepositoryStatus> &statuses);

void render_table(std::ostream &output,
                  const std::vector<RepositoryStatus> &statuses, bool color);

std::string format_duration(std::chrono::nanoseconds duration);
void write_timing_report(std::ostream &output,
                         std::chrono::nanoseconds discovery,
                         std::chrono::nanoseconds checks,
                         std::chrono::nanoseconds total);

void write_walk_warnings(std::ostream &output,
                         const std::vector<std::string> &warnings);
std::size_t write_check_warnings(std::ostream &output,
                                 const std::vector<RepositoryStatus> &statuses);

int run_cli(const std::vector<std::string> &arguments,
            std::ostream &standard_output, std::ostream &error_output,
            const std::atomic_bool &cancelled);

} // namespace check_git_status
