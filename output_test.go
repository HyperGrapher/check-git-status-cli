package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"
	"time"
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

func TestWriteTimingReport(t *testing.T) {
	var output bytes.Buffer
	writeTimingReport(
		&output,
		750*time.Microsecond,
		1500*time.Millisecond,
		1501*time.Millisecond,
	)

	want := "Timing: discovery <1ms; checks 1.5s; total 1.501s.\n"
	if output.String() != want {
		t.Fatalf("timing report = %q, want %q", output.String(), want)
	}
}

func TestFormatDuration(t *testing.T) {
	tests := []struct {
		name     string
		duration time.Duration
		want     string
	}{
		{name: "zero", duration: 0, want: "0s"},
		{name: "sub-millisecond", duration: 500 * time.Microsecond, want: "<1ms"},
		{name: "milliseconds", duration: 1499 * time.Microsecond, want: "1ms"},
		{name: "seconds", duration: 2345 * time.Millisecond, want: "2.345s"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := formatDuration(test.duration); got != test.want {
				t.Fatalf("formatDuration(%s) = %q, want %q", test.duration, got, test.want)
			}
		})
	}
}
