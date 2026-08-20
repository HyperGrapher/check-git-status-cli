package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"time"
)

type UpstreamState int

const (
	UpstreamOK UpstreamState = iota
	UpstreamMissing
	UpstreamDetached
	UpstreamError
)

type RepositoryStatus struct {
	Path          string
	RemoteURL     string
	RemoteName    string
	HasRemote     bool
	RemoteError   error
	Dirty         bool
	WorktreeError error
	UpstreamState UpstreamState
	Ahead         int
	UpstreamError error
}

func (status RepositoryStatus) NeedsAttention() bool {
	return status.Dirty || status.Ahead > 0 || !status.HasRemote ||
		status.RemoteError != nil || status.WorktreeError != nil || status.UpstreamError != nil
}

func (status RepositoryStatus) Issues() []string {
	type issueGroup struct {
		message string
		checks  []string
	}

	groups := make([]issueGroup, 0, 3)
	addIssue := func(check string, err error) {
		if err == nil {
			return
		}
		message := err.Error()
		for index := range groups {
			if groups[index].message == message {
				groups[index].checks = append(groups[index].checks, check)
				return
			}
		}
		groups = append(groups, issueGroup{message: message, checks: []string{check}})
	}

	addIssue("remote", status.RemoteError)
	addIssue("worktree", status.WorktreeError)
	addIssue("upstream", status.UpstreamError)

	issues := make([]string, 0, len(groups))
	for _, group := range groups {
		label := group.checks[0] + " check"
		if len(group.checks) > 1 {
			label = strings.Join(group.checks, "/") + " checks"
		}
		issues = append(issues, label+": "+group.message)
	}
	return issues
}

type GitRunner interface {
	Run(context.Context, string, ...string) (string, error)
}

type ExecGitRunner struct {
	Binary string
}

func (runner ExecGitRunner) Run(ctx context.Context, directory string, args ...string) (string, error) {
	commandArgs := append([]string{"-C", directory}, args...)
	command := exec.CommandContext(ctx, runner.Binary, commandArgs...)
	command.Env = append(os.Environ(), "GIT_OPTIONAL_LOCKS=0", "LC_ALL=C")
	output, err := command.CombinedOutput()
	trimmed := strings.TrimSpace(string(output))
	if err != nil {
		if trimmed != "" {
			return "", fmt.Errorf("%s: %w", singleLine(trimmed), err)
		}
		return "", err
	}
	return trimmed, nil
}

type GitClient struct {
	Runner  GitRunner
	Timeout time.Duration
}

func (client GitClient) Check(ctx context.Context, path string) RepositoryStatus {
	status := RepositoryStatus{Path: path}
	if client.Timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, client.Timeout)
		defer cancel()
	}

	client.checkRemote(ctx, path, &status)
	client.checkWorktree(ctx, path, &status)
	client.checkUpstream(ctx, path, &status)
	return status
}

func (client GitClient) checkRemote(ctx context.Context, path string, status *RepositoryStatus) {
	output, err := client.Runner.Run(ctx, path, "remote")
	if err != nil {
		status.RemoteError = err
		return
	}
	remotes := strings.Fields(output)
	if len(remotes) == 0 {
		return
	}

	status.RemoteName = remotes[0]
	for _, remote := range remotes {
		if remote == "origin" {
			status.RemoteName = remote
			break
		}
	}
	status.HasRemote = true
	status.RemoteURL, err = client.Runner.Run(ctx, path, "remote", "get-url", status.RemoteName)
	if err != nil {
		status.RemoteError = err
	}
}

func (client GitClient) checkWorktree(ctx context.Context, path string, status *RepositoryStatus) {
	output, err := client.Runner.Run(ctx, path, "status", "--porcelain")
	if err != nil {
		status.WorktreeError = err
		return
	}
	status.Dirty = output != ""
}

func (client GitClient) checkUpstream(ctx context.Context, path string, status *RepositoryStatus) {
	branchRef, err := client.Runner.Run(ctx, path, "symbolic-ref", "--quiet", "HEAD")
	if err != nil {
		var exitError *exec.ExitError
		if errors.As(err, &exitError) && exitError.ExitCode() == 1 {
			status.UpstreamState = UpstreamDetached
			return
		}
		status.UpstreamState = UpstreamError
		status.UpstreamError = err
		return
	}

	upstream, err := client.Runner.Run(ctx, path, "for-each-ref", "--format=%(upstream)", branchRef)
	if err != nil {
		status.UpstreamState = UpstreamError
		status.UpstreamError = err
		return
	}
	if upstream == "" {
		status.UpstreamState = UpstreamMissing
		return
	}

	output, err := client.Runner.Run(ctx, path, "log", "--format=%H", "@{upstream}..HEAD")
	if err != nil {
		status.UpstreamState = UpstreamError
		status.UpstreamError = err
		return
	}
	status.UpstreamState = UpstreamOK
	status.Ahead = len(strings.Fields(output))
}

func singleLine(value string) string {
	return strings.Join(strings.Fields(value), " ")
}
