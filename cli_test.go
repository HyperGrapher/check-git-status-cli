package main

import (
	"bytes"
	"context"
	"io"
	"os/exec"
	"strings"
	"testing"
)

func TestParseOptions(t *testing.T) {
	opts, err := parseOptions([]string{"-d", "-j", "3", "projects"}, io.Discard)
	if err != nil {
		t.Fatalf("parseOptions() error = %v", err)
	}
	if !opts.dirtyOnly || opts.workers != 3 || opts.root != "projects" {
		t.Fatalf("parseOptions() = %+v", opts)
	}
}

func TestParseOptionsDefaultsToCurrentDirectory(t *testing.T) {
	opts, err := parseOptions(nil, io.Discard)
	if err != nil {
		t.Fatalf("parseOptions() error = %v", err)
	}
	if opts.root != "." {
		t.Fatalf("opts.root = %q, want current directory", opts.root)
	}
}

func TestRunCLIEndsWithTimingReport(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("Git is not installed")
	}

	var stdout bytes.Buffer
	var stderr bytes.Buffer
	exitCode := runCLI(
		context.Background(),
		[]string{"--no-color", t.TempDir()},
		&stdout,
		&stderr,
	)
	if exitCode != 0 {
		t.Fatalf("runCLI() exit code = %d, stderr = %q", exitCode, stderr.String())
	}

	lines := strings.Split(strings.TrimSpace(stdout.String()), "\n")
	lastLine := lines[len(lines)-1]
	for _, expected := range []string{"Timing: discovery ", "; checks ", "; total "} {
		if !strings.Contains(lastLine, expected) {
			t.Fatalf("last output line does not contain %q: %q", expected, lastLine)
		}
	}
}
