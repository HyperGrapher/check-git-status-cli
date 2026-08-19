# check-git-status

`check-git-status` is a fast, cross-platform CLI that recursively finds Git repositories and summarizes the state of each worktree. Repository checks run concurrently through a bounded worker pool, while traversal stops at every repository boundary so nested content is not scanned unnecessarily.

The tool reports:

- the repository path;
- the preferred remote URL (`origin`, or the first configured remote);
- whether the worktree has uncommitted changes; and
- whether the current branch has commits that are not in its upstream branch.

Linked worktrees and submodules that use a `.git` file are recognized in addition to ordinary `.git` directories.

## Requirements

- Go 1.22 or newer to build the tool
- Git available on `PATH` when running it

## Build

```sh
go build -o check-git-status .
```

On Windows, Go automatically creates `check-git-status.exe` when appropriate:

```powershell
go build -o check-git-status.exe .
```

You can also install it into your configured Go binary directory:

```sh
go install .
```

## Usage

Scan the current directory:

```sh
check-git-status
```

Scan another directory:

```sh
check-git-status /path/to/projects
```

Show only repositories with uncommitted changes, unpushed commits, a missing remote, or a check error:

```sh
check-git-status --dirty-only /path/to/projects
check-git-status -d /path/to/projects
```

Control concurrency or disable ANSI colors:

```sh
check-git-status --workers 8 --no-color /path/to/projects
```

All options:

```text
Usage: check-git-status [options] [directory]

Options:
  -d, --dirty-only       show only repositories that need attention
  -j, --workers number   number of concurrent repository checks
      --no-color         disable colored output
```

Color is enabled only for interactive terminal output and can also be disabled by setting the [`NO_COLOR`](https://no-color.org/) environment variable.

## Status semantics

- **No Remote** means no Git remotes are configured.
- **Dirty** means `git status --porcelain` returned at least one entry, including untracked files.
- **N commits ahead** is calculated against the current branch's configured upstream.
- **No Upstream** means the current branch has no tracking branch. It is reported but, by itself, is not included by `--dirty-only` unless the repository also has another problem such as no remote.
- **Detached HEAD** is reported explicitly and is not treated as an unpushed change by the filter.
- **Error** means a Git check could not be completed. Details are written as warnings to standard error and the process exits nonzero.

Traversal permission failures are reported as warnings without preventing accessible repositories from being checked. Operational failures result in exit code `1`; invalid command-line usage results in exit code `2`; and an interrupted scan exits with code `130`.

## Development

```sh
go fmt ./...
go test ./...
go vet ./...
```
