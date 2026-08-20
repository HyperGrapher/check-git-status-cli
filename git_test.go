package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

type scriptedGitRunner struct {
	remoteOutput string
	statusOutput string
	calls        [][]string
}

func (runner *scriptedGitRunner) Run(_ context.Context, _ string, args ...string) (string, error) {
	runner.calls = append(runner.calls, append([]string(nil), args...))
	switch {
	case slices.Equal(args, []string{"remote", "-v"}):
		return runner.remoteOutput, nil
	case slices.Equal(args, []string{"status", "--porcelain=v2", "--branch"}):
		return runner.statusOutput, nil
	default:
		return "", fmt.Errorf("unexpected Git command: %v", args)
	}
}

func TestGitClientUsesTwoCommands(t *testing.T) {
	runner := &scriptedGitRunner{
		remoteOutput: "backup\thttps://example.com/backup.git (fetch)\n" +
			"backup\thttps://example.com/backup.git (push)\n" +
			"origin\thttps://example.com/project.git (fetch)\n" +
			"origin\thttps://example.com/project.git (push)",
		statusOutput: "# branch.oid abcdef\n" +
			"# branch.head main\n" +
			"# branch.upstream origin/main\n" +
			"# branch.ab +2 -0\n" +
			"1 .M N... 100644 100644 100644 abcdef abcdef tracked.txt",
	}
	client := GitClient{Runner: runner, Timeout: 15 * time.Second}
	status := client.Check(context.Background(), "project")

	if len(runner.calls) != 2 {
		t.Fatalf("Git command count = %d, want 2: %v", len(runner.calls), runner.calls)
	}
	if !status.HasRemote || status.RemoteName != "origin" ||
		status.RemoteURL != "https://example.com/project.git" {
		t.Fatalf("remote status = %+v, want origin URL", status)
	}
	if !status.Dirty || status.UpstreamState != UpstreamOK || status.Ahead != 2 {
		t.Fatalf("repository status = %+v, want dirty and 2 commits ahead", status)
	}
}

func TestGitClientParsesDetachedStatus(t *testing.T) {
	runner := &scriptedGitRunner{
		statusOutput: "# branch.oid abcdef\n# branch.head (detached)",
	}
	client := GitClient{Runner: runner, Timeout: 15 * time.Second}
	status := client.Check(context.Background(), "project")

	if len(runner.calls) != 2 {
		t.Fatalf("Git command count = %d, want 2: %v", len(runner.calls), runner.calls)
	}
	if status.HasRemote || status.Dirty || status.UpstreamState != UpstreamDetached {
		t.Fatalf("repository status = %+v, want clean detached repository without remote", status)
	}
}

func TestRepositoryStatusIssuesConsolidatesDuplicateErrors(t *testing.T) {
	sharedError := errors.New("repository is inaccessible")
	status := RepositoryStatus{
		RemoteError:   sharedError,
		WorktreeError: sharedError,
		UpstreamError: sharedError,
	}

	issues := status.Issues()
	if len(issues) != 1 {
		t.Fatalf("Issues() returned %d issues, want 1: %v", len(issues), issues)
	}
	want := "remote/worktree/upstream checks: repository is inaccessible"
	if issues[0] != want {
		t.Fatalf("Issues()[0] = %q, want %q", issues[0], want)
	}
}

func TestRepositoryStatusIssuesPreservesDistinctErrors(t *testing.T) {
	sharedError := errors.New("repository is inaccessible")
	status := RepositoryStatus{
		RemoteError:   sharedError,
		WorktreeError: sharedError,
		UpstreamError: errors.New("upstream failed separately"),
	}

	issues := status.Issues()
	want := []string{
		"remote/worktree checks: repository is inaccessible",
		"upstream check: upstream failed separately",
	}
	if len(issues) != len(want) {
		t.Fatalf("Issues() returned %d issues, want %d: %v", len(issues), len(want), issues)
	}
	for index := range want {
		if issues[index] != want[index] {
			t.Fatalf("Issues()[%d] = %q, want %q", index, issues[index], want[index])
		}
	}
}

func TestGitClientRepositoryStates(t *testing.T) {
	gitBinary, err := exec.LookPath("git")
	if err != nil {
		t.Skip("Git is not installed")
	}

	root := t.TempDir()
	repository := filepath.Join(root, "repository")
	remote := filepath.Join(root, "remote.git")
	runGitTest(t, gitBinary, "init", "-q", repository)
	tracked := filepath.Join(repository, "tracked.txt")
	if err := os.WriteFile(tracked, []byte("initial\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGitTest(t, gitBinary, "-C", repository, "add", "tracked.txt")
	commitTestFile(t, gitBinary, repository, "initial")

	client := GitClient{Runner: ExecGitRunner{Binary: gitBinary}, Timeout: 10 * time.Second}
	status := client.Check(context.Background(), repository)
	if status.HasRemote || status.Dirty || status.UpstreamState != UpstreamMissing {
		t.Fatalf("initial status = %+v, want clean with no remote and no upstream", status)
	}

	untracked := filepath.Join(repository, "untracked.txt")
	if err := os.WriteFile(untracked, []byte("dirty\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	status = client.Check(context.Background(), repository)
	if !status.Dirty {
		t.Fatalf("status.Dirty = false, want true: %+v", status)
	}
	if err := os.Remove(untracked); err != nil {
		t.Fatal(err)
	}

	runGitTest(t, gitBinary, "init", "--bare", "-q", remote)
	runGitTest(t, gitBinary, "-C", repository, "remote", "add", "origin", remote)
	runGitTest(t, gitBinary, "-C", repository, "push", "-q", "-u", "origin", "HEAD")
	status = client.Check(context.Background(), repository)
	if !status.HasRemote || status.RemoteURL == "" || status.Ahead != 0 || status.UpstreamState != UpstreamOK {
		t.Fatalf("tracked status = %+v, want a configured remote with no commits ahead", status)
	}

	if err := os.WriteFile(tracked, []byte("second\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGitTest(t, gitBinary, "-C", repository, "add", "tracked.txt")
	commitTestFile(t, gitBinary, repository, "second")
	status = client.Check(context.Background(), repository)
	if status.Ahead != 1 || status.UpstreamState != UpstreamOK {
		t.Fatalf("ahead status = %+v, want one commit ahead", status)
	}

	runGitTest(t, gitBinary, "-C", repository, "checkout", "--detach", "-q")
	status = client.Check(context.Background(), repository)
	if status.UpstreamState != UpstreamDetached || status.UpstreamError != nil {
		t.Fatalf("detached status = %+v, want detached HEAD", status)
	}
}

func commitTestFile(t *testing.T, gitBinary, repository, message string) {
	t.Helper()
	runGitTest(t, gitBinary,
		"-C", repository,
		"-c", "user.name=Test User",
		"-c", "user.email=test@example.com",
		"commit", "-q", "-m", message,
	)
}

func runGitTest(t *testing.T, gitBinary string, args ...string) {
	t.Helper()
	command := exec.Command(gitBinary, args...)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("git %v failed: %v\n%s", args, err, output)
	}
}
