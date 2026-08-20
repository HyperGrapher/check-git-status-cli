package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

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
