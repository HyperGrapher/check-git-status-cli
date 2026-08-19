package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os/exec"
	"runtime"
	"time"
)

type options struct {
	dirtyOnly bool
	noColor   bool
	workers   int
	root      string
}

func parseOptions(args []string, stderr io.Writer) (options, error) {
	defaultWorkers := runtime.NumCPU()
	if defaultWorkers < 4 {
		defaultWorkers = 4
	}
	if defaultWorkers > 32 {
		defaultWorkers = 32
	}

	var opts options
	flags := flag.NewFlagSet("check-git-status", flag.ContinueOnError)
	flags.SetOutput(stderr)
	flags.BoolVar(&opts.dirtyOnly, "dirty-only", false, "show only repositories that need attention")
	flags.BoolVar(&opts.dirtyOnly, "d", false, "show only repositories that need attention (shorthand)")
	flags.BoolVar(&opts.noColor, "no-color", false, "disable colored output")
	flags.IntVar(&opts.workers, "workers", defaultWorkers, "number of concurrent repository checks")
	flags.IntVar(&opts.workers, "j", defaultWorkers, "number of concurrent repository checks (shorthand)")
	flags.Usage = func() {
		fmt.Fprintln(stderr, "Usage: check-git-status [options] [directory]")
		fmt.Fprintln(stderr, "\nRecursively find Git repositories and summarize their status.")
		fmt.Fprintln(stderr, "\nOptions:")
		flags.PrintDefaults()
	}

	if err := flags.Parse(args); err != nil {
		return options{}, err
	}
	if flags.NArg() > 1 {
		flags.Usage()
		return options{}, errors.New("only one directory may be specified")
	}
	if opts.workers < 1 {
		return options{}, errors.New("workers must be at least 1")
	}
	opts.root = "."
	if flags.NArg() == 1 {
		opts.root = flags.Arg(0)
	}
	return opts, nil
}

func runCLI(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	opts, err := parseOptions(args, stderr)
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		fmt.Fprintf(stderr, "error: %v\n", err)
		return 2
	}

	gitBinary, err := exec.LookPath("git")
	if err != nil {
		fmt.Fprintln(stderr, "error: Git was not found in PATH; install Git and try again")
		return 1
	}

	repositories, walkWarnings, err := discoverRepositories(opts.root)
	if err != nil {
		fmt.Fprintf(stderr, "error: %v\n", err)
		return 1
	}

	if len(repositories) == 0 {
		fmt.Fprintf(stdout, "No Git repositories found under %s.\n", opts.root)
		writeWalkWarnings(stderr, walkWarnings)
		if len(walkWarnings) > 0 {
			return 1
		}
		return 0
	}

	client := GitClient{Runner: ExecGitRunner{Binary: gitBinary}, Timeout: 15 * time.Second}
	statuses := checkRepositories(ctx, repositories, opts.workers, client)
	if ctx.Err() != nil {
		fmt.Fprintln(stderr, "error: scan interrupted")
		return 130
	}

	displayed := statuses
	if opts.dirtyOnly {
		displayed = filterNeedsAttention(statuses)
	}

	if len(displayed) == 0 {
		fmt.Fprintln(stdout, "No repositories need attention.")
	} else {
		renderTable(stdout, displayed, !opts.noColor && supportsColor(stdout))
	}
	fmt.Fprintf(stdout, "\nScanned %d repositories; %d need attention.\n", len(statuses), countNeedsAttention(statuses))

	writeWalkWarnings(stderr, walkWarnings)
	checkErrors := writeCheckWarnings(stderr, statuses)
	if len(walkWarnings) > 0 || checkErrors > 0 {
		return 1
	}
	return 0
}

func writeWalkWarnings(w io.Writer, warnings []error) {
	for _, warning := range warnings {
		fmt.Fprintf(w, "warning: %v\n", warning)
	}
}

func writeCheckWarnings(w io.Writer, statuses []RepositoryStatus) int {
	count := 0
	for _, status := range statuses {
		for _, issue := range status.Issues() {
			fmt.Fprintf(w, "warning: %s: %s\n", status.Path, issue)
			count++
		}
	}
	return count
}
