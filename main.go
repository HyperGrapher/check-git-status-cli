package main

import (
	"context"
	"os"
	"os/signal"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	os.Exit(runCLI(ctx, os.Args[1:], os.Stdout, os.Stderr))
}
