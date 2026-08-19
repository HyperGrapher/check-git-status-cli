package main

import (
	"io"
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
