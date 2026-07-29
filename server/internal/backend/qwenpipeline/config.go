package qwenpipeline

import (
	"errors"
	"fmt"
	"log/slog"
	"net/url"
	"strings"
	"time"
)

const (
	RegionChinaBeijing = "china-beijing"
	RegionSingapore    = "singapore"
)

type Config struct {
	APIKey          string
	WorkspaceID     string
	Region          string
	ASRModel        string
	ReasoningModel  string
	ReasoningEffort string
	TTSModel        string
	TTSVoice        string
	SearchMode      string
	Timeout         time.Duration
	QueueSize       int
	Logger          *slog.Logger
}

func (c Config) validate() error {
	if strings.TrimSpace(c.APIKey) == "" || strings.TrimSpace(c.WorkspaceID) == "" {
		return errors.New("Qwen API key and workspace ID are required")
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
	if c.ReasoningEffort != "low" && c.ReasoningEffort != "medium" && c.ReasoningEffort != "high" {
		return errors.New("reasoning effort must be low, medium, or high")
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
	return nil
}

func (c Config) compatibleBaseURL() string {
	domain := "cn-beijing.maas.aliyuncs.com"
	if c.Region == RegionSingapore {
		domain = "ap-southeast-1.maas.aliyuncs.com"
	}
	return fmt.Sprintf("https://%s.%s/compatible-mode/v1", c.WorkspaceID, domain)
}

func (c Config) ttsURL() string {
	domain := "dashscope.aliyuncs.com"
	if c.Region == RegionSingapore {
		domain = "dashscope-intl.aliyuncs.com"
	}
	return "wss://" + domain + "/api-ws/v1/realtime?model=" + url.QueryEscape(c.TTSModel)
}
