package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strconv"
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
	client.checkStatus(ctx, path, &status)
	return status
}

func (client GitClient) checkRemote(ctx context.Context, path string, status *RepositoryStatus) {
	output, err := client.Runner.Run(ctx, path, "remote", "-v")
	if err != nil {
		status.RemoteError = err
		return
	}

	remoteURLs := make(map[string]string)
	for _, line := range strings.Split(output, "\n") {
		line = strings.TrimSuffix(line, "\r")
		const fetchSuffix = " (fetch)"
		if !strings.HasSuffix(line, fetchSuffix) {
			continue
		}
		line = strings.TrimSuffix(line, fetchSuffix)
		nameEnd := strings.IndexAny(line, " \t")
		if nameEnd < 0 {
			continue
		}
		name := line[:nameEnd]
		url := strings.TrimLeft(line[nameEnd:], " \t")
		if name == "" || url == "" {
			continue
		}
		if _, exists := remoteURLs[name]; !exists {
			remoteURLs[name] = url
		}
	}
	if len(remoteURLs) == 0 {
		return
	}

	names := make([]string, 0, len(remoteURLs))
	for name := range remoteURLs {
		names = append(names, name)
	}
	sort.Strings(names)
	status.RemoteName = names[0]
	if _, exists := remoteURLs["origin"]; exists {
		status.RemoteName = "origin"
	}

	status.HasRemote = true
	status.RemoteURL = remoteURLs[status.RemoteName]
}

func (client GitClient) checkStatus(ctx context.Context, path string, status *RepositoryStatus) {
	output, err := client.Runner.Run(ctx, path, "status", "--porcelain=v2", "--branch")
	if err != nil {
		status.WorktreeError = err
		status.UpstreamState = UpstreamError
		status.UpstreamError = err
		return
	}

	detached := false
	hasUpstream := false
	for _, line := range strings.Split(output, "\n") {
		line = strings.TrimSuffix(line, "\r")
		if line == "" {
			continue
		}
		if !strings.HasPrefix(line, "#") {
			status.Dirty = true
			continue
		}

		fields := strings.Fields(line)
		if len(fields) < 3 {
			continue
		}
		switch fields[1] {
		case "branch.head":
			detached = fields[2] == "(detached)"
		case "branch.upstream":
			hasUpstream = true
		case "branch.ab":
			ahead, parseErr := strconv.Atoi(strings.TrimPrefix(fields[2], "+"))
			if parseErr != nil || ahead < 0 {
				status.UpstreamState = UpstreamError
				status.UpstreamError = fmt.Errorf("parse ahead count %q", fields[2])
				return
			}
			status.Ahead = ahead
		}
	}

	switch {
	case detached:
		status.UpstreamState = UpstreamDetached
	case hasUpstream:
		status.UpstreamState = UpstreamOK
	default:
		status.UpstreamState = UpstreamMissing
	}
}

func singleLine(value string) string {
	return strings.Join(strings.Fields(value), " ")
}
