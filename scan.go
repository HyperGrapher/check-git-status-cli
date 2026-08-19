package main

import (
	"context"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"sync"
)

// discoverRepositories finds non-bare Git repositories. A .git directory is
// standard, while a .git file also identifies linked worktrees and submodules.
func discoverRepositories(root string) ([]string, []error, error) {
	absRoot, err := filepath.Abs(root)
	if err != nil {
		return nil, nil, fmt.Errorf("resolve scan root %q: %w", root, err)
	}
	info, err := os.Stat(absRoot)
	if err != nil {
		return nil, nil, fmt.Errorf("open scan root %q: %w", absRoot, err)
	}
	if !info.IsDir() {
		return nil, nil, fmt.Errorf("scan root %q is not a directory", absRoot)
	}

	var repositories []string
	var warnings []error
	err = filepath.WalkDir(absRoot, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			warnings = append(warnings, fmt.Errorf("walk %q: %w", path, walkErr))
			if entry != nil && entry.IsDir() {
				return fs.SkipDir
			}
			return nil
		}
		if !entry.IsDir() {
			return nil
		}

		gitMarker := filepath.Join(path, ".git")
		markerInfo, statErr := os.Stat(gitMarker)
		switch {
		case statErr == nil && (markerInfo.IsDir() || markerInfo.Mode().IsRegular()):
			repositories = append(repositories, path)
			return fs.SkipDir
		case statErr != nil && !os.IsNotExist(statErr):
			warnings = append(warnings, fmt.Errorf("inspect %q: %w", gitMarker, statErr))
		}
		return nil
	})
	if err != nil {
		return nil, warnings, fmt.Errorf("walk scan root %q: %w", absRoot, err)
	}

	sort.Strings(repositories)
	return repositories, warnings, nil
}

type repositoryChecker interface {
	Check(context.Context, string) RepositoryStatus
}

func checkRepositories(ctx context.Context, paths []string, workers int, checker repositoryChecker) []RepositoryStatus {
	if len(paths) == 0 {
		return nil
	}
	if workers > len(paths) {
		workers = len(paths)
	}

	jobs := make(chan string)
	results := make(chan RepositoryStatus, len(paths))
	var workerGroup sync.WaitGroup
	workerGroup.Add(workers)

	for range workers {
		go func() {
			defer workerGroup.Done()
			for path := range jobs {
				results <- checker.Check(ctx, path)
			}
		}()
	}

	go func() {
		for _, path := range paths {
			jobs <- path
		}
		close(jobs)
		workerGroup.Wait()
		close(results)
	}()

	statuses := make([]RepositoryStatus, 0, len(paths))
	for status := range results {
		statuses = append(statuses, status)
	}
	sort.Slice(statuses, func(i, j int) bool { return statuses[i].Path < statuses[j].Path })
	return statuses
}
