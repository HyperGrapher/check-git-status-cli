package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"
)

func TestFilterNeedsAttention(t *testing.T) {
	statuses := []RepositoryStatus{
		{Path: "clean", HasRemote: true, UpstreamState: UpstreamOK},
		{Path: "dirty", HasRemote: true, Dirty: true},
		{Path: "ahead", HasRemote: true, Ahead: 2},
		{Path: "no-remote", UpstreamState: UpstreamMissing},
		{Path: "no-upstream", HasRemote: true, UpstreamState: UpstreamMissing},
		{Path: "detached", HasRemote: true, UpstreamState: UpstreamDetached},
		{Path: "error", HasRemote: true, WorktreeError: errors.New("failed")},
	}

	filtered := filterNeedsAttention(statuses)
	want := []string{"dirty", "ahead", "no-remote", "error"}
	if len(filtered) != len(want) {
		t.Fatalf("filtered length = %d, want %d: %+v", len(filtered), len(want), filtered)
	}
	for index, path := range want {
		if filtered[index].Path != path {
			t.Fatalf("filtered[%d].Path = %q, want %q", index, filtered[index].Path, path)
		}
	}
}

func TestRenderTable(t *testing.T) {
	statuses := []RepositoryStatus{
		{Path: "repo-a", HasRemote: true, RemoteURL: "https://example.com/repo-a", Dirty: true, Ahead: 1},
		{Path: "repo-b", UpstreamState: UpstreamDetached},
	}
	var output bytes.Buffer
	renderTable(&output, statuses, false)

	for _, expected := range []string{
		"REPOSITORY", "REMOTE", "WORKTREE", "UNPUSHED",
		"repo-a", "https://example.com/repo-a", "Dirty", "1 commit ahead",
		"repo-b", "No Remote", "Detached HEAD",
	} {
		if !strings.Contains(output.String(), expected) {
			t.Errorf("table does not contain %q:\n%s", expected, output.String())
		}
	}
	if strings.Contains(output.String(), "\x1b[") {
		t.Fatalf("uncolored table contains ANSI escapes:\n%q", output.String())
	}
}
