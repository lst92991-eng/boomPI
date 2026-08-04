package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/lst92991-eng/boomPI/server/internal/app"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/logging"
)

type options struct {
	configPath  string
	checkConfig bool
	overrides   config.Overrides
}

func main() {
	os.Exit(realMain(os.Args[1:], os.Stdout, os.Stderr))
}

func realMain(args []string, stdout, stderr io.Writer) int {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return run(ctx, args, stdout, stderr)
}

func run(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet("boompi-server", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var opts options
	opts.overrides = make(config.Overrides)
	flags.StringVar(&opts.configPath, "config", "config.yaml", "path to the YAML configuration")
	flags.BoolVar(&opts.checkConfig, "check-config", false, "validate configuration and exit")
	addOverrideFlag(flags, opts.overrides, "listen-address", "listen_address", "override the server listen address")
	addOverrideFlag(flags, opts.overrides, "wss-port", "wss_port", "override the WSS port")
	addOverrideFlag(flags, opts.overrides, "discovery-port", "discovery_port", "override the UDP discovery port")
	addOverrideFlag(flags, opts.overrides, "log-level", "log_level", "override the log level")
	addOverrideFlag(flags, opts.overrides, "provider", "provider", "override the provider name")
	addOverrideFlag(flags, opts.overrides, "region", "region", "override the provider region")
	addOverrideFlag(flags, opts.overrides, "model", "model", "override the provider model")
	addOverrideFlag(flags, opts.overrides, "voice", "voice", "override the provider voice")
	addOverrideFlag(flags, opts.overrides, "search-mode", "search_mode", "override search mode (auto or off)")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if flags.NArg() != 0 {
		fmt.Fprintln(stderr, "unexpected positional arguments")
		return 2
	}

	created, err := config.CreateStarter(opts.configPath)
	if err != nil {
		fmt.Fprintf(stderr, "configuration setup error: %v\n", err)
		return 1
	}
	if created {
		absolutePath, pathErr := filepath.Abs(opts.configPath)
		if pathErr != nil {
			absolutePath = opts.configPath
		}
		fmt.Fprintf(stdout, "Created starter configuration: %s\n", absolutePath)
		fmt.Fprintln(stdout, "Edit qwen_api_key, save the file, then run boompi-server again.")
		return 0
	}

	cfg, err := config.Load(opts.configPath, opts.overrides)
	if err != nil {
		fmt.Fprintf(stderr, "configuration error: %v\n", err)
		return 1
	}
	if opts.checkConfig {
		fmt.Fprintf(stdout, "configuration valid (listen_address=%s wss_port=%d discovery_port=%d provider=%s region=%s model=%s credential_source=%s)\n",
			cfg.ListenAddress, cfg.WSSPort, cfg.DiscoveryPort, cfg.Provider, cfg.Region, cfg.Model, cfg.Credentials.Source())
		return 0
	}

	logger, err := logging.NewJSON(stderr, cfg.LogLevel)
	if err != nil {
		fmt.Fprintf(stderr, "logging configuration error: %v\n", err)
		return 1
	}
	absoluteConfigPath, err := filepath.Abs(opts.configPath)
	if err != nil {
		// Startup errors are local (path/TLS/bind) and already scrubbed upstream;
		// log them as strings because the redactor drops KindAny values wholesale.
		logger.Error("resolve configuration path", "error", err.Error())
		return 1
	}
	identityDirectory := filepath.Join(filepath.Dir(absoluteConfigPath), "state")
	application, err := app.New(cfg, logger, identityDirectory)
	if err != nil {
		logger.Error("application initialization failed", "error", err.Error())
		return 1
	}
	if err := application.Run(ctx); err != nil {
		logger.Error("application stopped with an error", "error", err.Error())
		return 1
	}
	return 0
}

type overrideValue struct {
	key       string
	overrides config.Overrides
}

func (v overrideValue) String() string { return "" }

func (v overrideValue) Set(value string) error {
	v.overrides[v.key] = value
	return nil
}

func addOverrideFlag(flags *flag.FlagSet, overrides config.Overrides, flagName, configKey, usage string) {
	flags.Var(overrideValue{key: configKey, overrides: overrides}, flagName, usage)
}
