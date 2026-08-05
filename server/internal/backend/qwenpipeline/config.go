package qwenpipeline

import (
	"errors"
	"fmt"
	"log/slog"
	"net/url"
	"strings"
	"time"
)

const RegionChinaBeijing = "china-beijing"

type Config struct {
	APIKey           string
	WorkspaceID      string
	Region           string
	ASRModel         string
	ReasoningModel   string
	ReasoningEffort  string
	TTSModel         string
	TTSVoice         string
	SearchMode       string
	Timeout          time.Duration
	QueueSize        int
	MaxTurns         int
	MaxContextTokens int
	Logger           *slog.Logger
}

func (c Config) validate() error {
	if strings.TrimSpace(c.APIKey) == "" {
		return errors.New("Qwen API key is required")
	}
	if c.Region != RegionChinaBeijing {
		return errors.New("Qwen region must be china-beijing")
	}
	if strings.ContainsAny(c.WorkspaceID, "/?#@") {
		return errors.New("Qwen workspace ID contains invalid characters")
	}
	for name, value := range map[string]string{
		"ASR model": c.ASRModel, "reasoning model": c.ReasoningModel,
		"TTS model": c.TTSModel, "TTS voice": c.TTSVoice,
	} {
		if strings.TrimSpace(value) == "" {
			return fmt.Errorf("%s is required", name)
		}
	}
	if c.ReasoningEffort != "none" && c.ReasoningEffort != "minimal" &&
		c.ReasoningEffort != "low" && c.ReasoningEffort != "medium" &&
		c.ReasoningEffort != "high" {
		return errors.New("reasoning effort must be none, minimal, low, medium, or high")
	}
	if c.SearchMode != "auto" && c.SearchMode != "off" {
		return errors.New("search mode must be auto or off")
	}
	if c.Timeout <= 0 || c.Timeout > 5*time.Minute {
		return errors.New("pipeline timeout must be greater than zero and at most 5m")
	}
	if c.QueueSize < 8 || c.QueueSize > 4096 {
		return errors.New("pipeline queue size must be between 8 and 4096")
	}
	if c.MaxTurns < 1 || c.MaxTurns > 100 {
		return errors.New("pipeline max turns must be between 1 and 100")
	}
	if c.MaxContextTokens < 1024 || c.MaxContextTokens > 1_000_000 {
		return errors.New("pipeline max context tokens must be between 1024 and 1000000")
	}
	return nil
}

func (c Config) compatibleBaseURL() string {
	if strings.TrimSpace(c.WorkspaceID) == "" {
		return "https://dashscope.aliyuncs.com/compatible-mode/v1"
	}
	return fmt.Sprintf("https://%s.cn-beijing.maas.aliyuncs.com/compatible-mode/v1", c.WorkspaceID)
}

func (c Config) ttsURL() string {
	return "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=" + url.QueryEscape(c.TTSModel)
}
