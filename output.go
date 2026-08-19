package main

import (
	"fmt"
	"io"
	"os"
	"strings"
	"unicode/utf8"
)

const (
	ansiReset  = "\x1b[0m"
	ansiBold   = "\x1b[1m"
	ansiCyan   = "\x1b[36m"
	ansiGreen  = "\x1b[32m"
	ansiYellow = "\x1b[33m"
	ansiRed    = "\x1b[31m"
)

type tableCell struct {
	text  string
	color string
}

func filterNeedsAttention(statuses []RepositoryStatus) []RepositoryStatus {
	filtered := make([]RepositoryStatus, 0, len(statuses))
	for _, status := range statuses {
		if status.NeedsAttention() {
			filtered = append(filtered, status)
		}
	}
	return filtered
}

func countNeedsAttention(statuses []RepositoryStatus) int {
	return len(filterNeedsAttention(statuses))
}

func renderTable(w io.Writer, statuses []RepositoryStatus, color bool) {
	headers := []tableCell{
		{text: "REPOSITORY", color: ansiBold + ansiCyan},
		{text: "REMOTE", color: ansiBold + ansiCyan},
		{text: "WORKTREE", color: ansiBold + ansiCyan},
		{text: "UNPUSHED", color: ansiBold + ansiCyan},
	}
	rows := make([][]tableCell, 0, len(statuses)+1)
	rows = append(rows, headers)
	for _, status := range statuses {
		rows = append(rows, statusRow(status))
	}

	widths := make([]int, len(headers))
	for _, row := range rows {
		for column, cell := range row {
			if width := utf8.RuneCountInString(cell.text); width > widths[column] {
				widths[column] = width
			}
		}
	}

	for rowIndex, row := range rows {
		for column, cell := range row {
			value := cell.text
			if column < len(row)-1 {
				value += strings.Repeat(" ", widths[column]-utf8.RuneCountInString(cell.text)+2)
			}
			if color && cell.color != "" {
				value = cell.color + value + ansiReset
			}
			fmt.Fprint(w, value)
		}
		fmt.Fprintln(w)
		if rowIndex == 0 {
			totalWidth := 2 * (len(headers) - 1)
			for _, width := range widths {
				totalWidth += width
			}
			fmt.Fprintln(w, strings.Repeat("-", totalWidth))
		}
	}
}

func statusRow(status RepositoryStatus) []tableCell {
	remote := tableCell{text: status.RemoteURL}
	switch {
	case status.RemoteError != nil:
		remote = tableCell{text: "Error", color: ansiRed}
	case !status.HasRemote:
		remote = tableCell{text: "No Remote", color: ansiRed}
	case status.RemoteURL == "":
		remote = tableCell{text: "Configured", color: ansiGreen}
	}

	worktree := tableCell{text: "Clean", color: ansiGreen}
	if status.WorktreeError != nil {
		worktree = tableCell{text: "Error", color: ansiRed}
	} else if status.Dirty {
		worktree = tableCell{text: "Dirty", color: ansiRed}
	}

	unpushed := tableCell{text: "OK", color: ansiGreen}
	switch {
	case status.UpstreamError != nil || status.UpstreamState == UpstreamError:
		unpushed = tableCell{text: "Error", color: ansiRed}
	case status.UpstreamState == UpstreamDetached:
		unpushed = tableCell{text: "Detached HEAD", color: ansiYellow}
	case status.UpstreamState == UpstreamMissing:
		unpushed = tableCell{text: "No Upstream", color: ansiYellow}
	case status.Ahead == 1:
		unpushed = tableCell{text: "1 commit ahead", color: ansiRed}
	case status.Ahead > 1:
		unpushed = tableCell{text: fmt.Sprintf("%d commits ahead", status.Ahead), color: ansiRed}
	}

	return []tableCell{{text: status.Path}, remote, worktree, unpushed}
}

func supportsColor(w io.Writer) bool {
	if os.Getenv("NO_COLOR") != "" || os.Getenv("TERM") == "dumb" {
		return false
	}
	file, ok := w.(*os.File)
	if !ok {
		return false
	}
	info, err := file.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}
