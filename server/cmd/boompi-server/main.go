package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/lst92991-eng/boomPI/server/internal/app"
	"github.com/lst92991-eng/boomPI/server/internal/config"
	"github.com/lst92991-eng/boomPI/server/internal/logging"
)

type options struct {
	configPath  string
	checkConfig bool
}

func main() {
	os.Exit(realMain(os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}

func realMain(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return run(ctx, args, stdin, stdout, stderr)
}

func run(ctx context.Context, args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet("boompi-server", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var opts options
	flags.StringVar(&opts.configPath, "config", defaultConfigPath(), "path to config.yaml")
	flags.BoolVar(&opts.checkConfig, "check-config", false, "validate configuration and exit")
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

	if err := ensureConfiguration(opts.configPath, stdin, stdout); err != nil {
		fmt.Fprintf(stderr, "configuration setup error: %v\n", err)
		return 1
	}

	cfg, err := config.Load(opts.configPath)
	if err != nil {
		fmt.Fprintf(stderr, "configuration error: %v\n", err)
		return 1
	}
	if opts.checkConfig {
		fmt.Fprintf(stdout, "configuration valid (wss=%s:%d discovery_udp=%d asr=%s llm=%s tts=%s credential_source=%s)\n",
			cfg.ListenAddress, cfg.WSSPort, cfg.DiscoveryPort, cfg.ASRModel,
			cfg.ReasoningModel, cfg.TTSModel, cfg.Credentials.Source())
		return 0
	}

	logger, err := logging.NewJSON(stderr, cfg.LogLevel)
	if err != nil {
		fmt.Fprintf(stderr, "logging configuration error: %v\n", err)
		return 1
	}
	absoluteConfigPath, err := filepath.Abs(opts.configPath)
	if err != nil {
		logger.Error("resolve configuration path", "error", err)
		return 1
	}
	identityDirectory := filepath.Join(filepath.Dir(absoluteConfigPath), "state")
	application, err := app.New(cfg, logger, identityDirectory)
	if err != nil {
		logger.Error("application initialization failed", "error", err)
		return 1
	}
	if err := application.Run(ctx); err != nil {
		logger.Error("application stopped with an error", "error", err)
		return 1
	}
	return 0
}

func defaultConfigPath() string {
	executable, err := os.Executable()
	if err != nil {
		return "config.yaml"
	}
	return filepath.Join(filepath.Dir(executable), "config.yaml")
}

func ensureConfiguration(path string, stdin io.Reader, stdout io.Writer) error {
	if _, err := os.Stat(path); err == nil {
		return nil
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("inspect config: %w", err)
	}

	apiKey := strings.TrimSpace(os.Getenv("DASHSCOPE_API_KEY"))
	if apiKey == "" {
		fmt.Fprintln(stdout, "首次运行：请粘贴中国内地 DashScope API Key。")
		fmt.Fprintln(stdout, "Key 只会保存到 EXE 同目录的 config.yaml，不会写入日志。")
		fmt.Fprint(stdout, "Qwen API Key: ")
		reader := bufio.NewReader(io.LimitReader(stdin, 4097))
		line, err := reader.ReadString('\n')
		if err != nil && !errors.Is(err, io.EOF) {
			return fmt.Errorf("read Qwen API key: %w", err)
		}
		apiKey = strings.TrimSpace(line)
		if len(apiKey) > 4096 {
			return errors.New("Qwen API key is too long")
		}
	}
	created, err := config.CreateStarter(path, apiKey)
	if err != nil {
		return err
	}
	if created {
		absolutePath, pathErr := filepath.Abs(path)
		if pathErr != nil {
			absolutePath = path
		}
		fmt.Fprintf(stdout, "配置已保存：%s\n", absolutePath)
		fmt.Fprintln(stdout, "正在启动 boomPI 服务端……")
	}
	return nil
}
