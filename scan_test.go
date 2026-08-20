package main

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"
	"time"
)

func TestDiscoverRepositoriesStopsAtRepositoryBoundary(t *testing.T) {
	root := t.TempDir()
	outer := filepath.Join(root, "alpha")
	nested := filepath.Join(outer, "nested")
	worktree := filepath.Join(root, "beta", "worktree")
	excludedNodeModules := filepath.Join(root, "node_modules", ".git")
	excludedBuild := filepath.Join(root, "build", "nested", ".git")

	for _, directory := range []string{
		filepath.Join(outer, ".git"),
		filepath.Join(nested, ".git"),
		worktree,
		excludedNodeModules,
		excludedBuild,
	} {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(worktree, ".git"), []byte("gitdir: elsewhere"), 0o644); err != nil {
		t.Fatal(err)
	}

	repositories, warnings, err := discoverRepositories(root)
	if err != nil {
		t.Fatalf("discoverRepositories() error = %v", err)
	}
	if len(warnings) != 0 {
		t.Fatalf("discoverRepositories() warnings = %v", warnings)
	}
	want := []string{outer, worktree}
	if !reflect.DeepEqual(repositories, want) {
		t.Fatalf("discoverRepositories() = %v, want %v", repositories, want)
	}
}

func TestDiscoverRepositoriesRejectsFileRoot(t *testing.T) {
	root := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(root, nil, 0o644); err != nil {
		t.Fatal(err)
	}

	if _, _, err := discoverRepositories(root); err == nil {
		t.Fatal("discoverRepositories() error = nil, want an error")
	}
}

func TestDiscoverRepositoriesSkipsExcludedRoot(t *testing.T) {
	root := filepath.Join(t.TempDir(), "build")
	if err := os.MkdirAll(filepath.Join(root, "nested", ".git"), 0o755); err != nil {
		t.Fatal(err)
	}

	repositories, warnings, err := discoverRepositories(root)
	if err != nil {
		t.Fatalf("discoverRepositories() error = %v", err)
	}
	if len(warnings) != 0 {
		t.Fatalf("discoverRepositories() warnings = %v", warnings)
	}
	if len(repositories) != 0 {
		t.Fatalf("discoverRepositories() = %v, want no repositories", repositories)
	}
}

type concurrencyChecker struct {
	mu        sync.Mutex
	active    int
	maxActive int
}

func (checker *concurrencyChecker) Check(_ context.Context, path string) RepositoryStatus {
	checker.mu.Lock()
	checker.active++
	if checker.active > checker.maxActive {
		checker.maxActive = checker.active
	}
	checker.mu.Unlock()

	time.Sleep(20 * time.Millisecond)

	checker.mu.Lock()
	checker.active--
	checker.mu.Unlock()
	return RepositoryStatus{Path: path, HasRemote: true}
}

func TestCheckRepositoriesUsesBoundedWorkerPool(t *testing.T) {
	checker := &concurrencyChecker{}
	paths := []string{"d", "c", "b", "a"}
	statuses := checkRepositories(context.Background(), paths, 2, checker)

	if checker.maxActive != 2 {
		t.Fatalf("maximum concurrent checks = %d, want 2", checker.maxActive)
	}
	for index, want := range []string{"a", "b", "c", "d"} {
		if statuses[index].Path != want {
			t.Fatalf("statuses[%d].Path = %q, want %q", index, statuses[index].Path, want)
		}
	}
}
