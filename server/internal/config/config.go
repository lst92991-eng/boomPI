package config

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"
	"unicode"

	"gopkg.in/yaml.v3"
)

const (
	maxConfigBytes = 64 * 1024

	APIKeyPlaceholder = "replace-with-your-qwen-api-key"

	DefaultDiscoveryPort = 17807
	defaultDeviceToken   = "boompi-teaching-shared-token-v1-2026"
	maxHeartbeatInterval = 10 * time.Second
)

// Credentials 的字段不导出，避免格式化 Config 或日志时泄露密钥。
type Credentials struct {
	apiKey      string
	source      string
	workspaceID string
}

func (c Credentials) APIKey() string      { return c.apiKey }
func (c Credentials) Source() string      { return c.source }
func (c Credentials) WorkspaceID() string { return c.workspaceID }
func (c Credentials) String() string {
	return fmt.Sprintf("{api_key:<redacted> source:%s workspace_id_set:%t}", c.source, c.workspaceID != "")
}
func (c Credentials) GoString() string { return c.String() }

// DeviceToken 是教学客户端和服务端共用的固定课堂口令。
type DeviceToken struct{ value string }

func (t DeviceToken) Value() string    { return t.value }
func (DeviceToken) String() string     { return "<redacted>" }
func (t DeviceToken) GoString() string { return t.String() }

// Config 是校验后的运行参数。配置文件不开放 discovery_port，
// 正式运行固定使用客户端约定的 IPv4 UDP 17807。
type Config struct {
	ListenAddress        string
	WSSPort              int
	DiscoveryPort        int
	LogLevel             string
	ASRModel             string
	ReasoningModel       string
	ReasoningEffort      string
	TTSModel             string
	TTSVoice             string
	SearchMode           string
	SystemPrompt         string
	Persona              string
	HeartbeatInterval    time.Duration
	ConnectionTimeout    time.Duration
	FirstResponseTimeout time.Duration
	SessionIdleTimeout   time.Duration
	MaxTurns             int
	MaxContextTokens     int
	Credentials          Credentials
	DeviceToken          DeviceToken
}

func Defaults() Config {
	return Config{
		ListenAddress:        "0.0.0.0",
		WSSPort:              17806,
		DiscoveryPort:        DefaultDiscoveryPort,
		LogLevel:             "info",
		ASRModel:             "qwen3-asr-flash",
		ReasoningModel:       "qwen3.6-flash",
		ReasoningEffort:      "none",
		TTSModel:             "qwen3-tts-flash-realtime",
		TTSVoice:             "Cherry",
		SearchMode:           "off",
		SystemPrompt:         "You are boomPI, a concise and helpful voice assistant. Reply in Simplified Chinese unless the user asks for another language.",
		Persona:              "Natural, young, friendly, and not overly cute.",
		HeartbeatInterval:    10 * time.Second,
		ConnectionTimeout:    30 * time.Second,
		FirstResponseTimeout: 30 * time.Second,
		SessionIdleTimeout:   30 * time.Minute,
		MaxTurns:             20,
		MaxContextTokens:     24_000,
		DeviceToken:          DeviceToken{value: defaultDeviceToken},
	}
}

// fileConfig 只保留课堂上真正需要调整的参数；provider、区域、链路模式和
// discovery 端口都是本版本的固定事实，不再伪装成可配置项。
type fileConfig struct {
	QwenAPIKey           string       `yaml:"qwen_api_key"`
	ListenAddress        string       `yaml:"listen_address"`
	WSSPort              int          `yaml:"wss_port"`
	LogLevel             string       `yaml:"log_level"`
	ASRModel             string       `yaml:"asr_model"`
	ReasoningModel       string       `yaml:"reasoning_model"`
	ReasoningEffort      string       `yaml:"reasoning_effort"`
	TTSModel             string       `yaml:"tts_model"`
	TTSVoice             string       `yaml:"tts_voice"`
	SearchMode           string       `yaml:"search_mode"`
	SystemPrompt         string       `yaml:"system_prompt"`
	Persona              string       `yaml:"persona"`
	HeartbeatInterval    yamlDuration `yaml:"heartbeat_interval"`
	ConnectionTimeout    yamlDuration `yaml:"connection_timeout"`
	FirstResponseTimeout yamlDuration `yaml:"first_response_timeout"`
	SessionIdleTimeout   yamlDuration `yaml:"session_idle_timeout"`
	MaxTurns             int          `yaml:"max_turns"`
	MaxContextTokens     int          `yaml:"max_context_tokens"`
}

type yamlDuration time.Duration

func (d *yamlDuration) UnmarshalYAML(node *yaml.Node) error {
	var value string
	if err := node.Decode(&value); err != nil {
		return errors.New("must be a duration such as 10s or 30m")
	}
	parsed, err := time.ParseDuration(value)
	if err != nil {
		return errors.New("must be a duration such as 10s or 30m")
	}
	*d = yamlDuration(parsed)
	return nil
}

func Load(path string) (Config, error) {
	cfg := Defaults()
	if path != "" {
		loaded, err := loadFile(path, cfg)
		if err != nil {
			return Config{}, err
		}
		cfg = loaded
	}

	if key := strings.TrimSpace(os.Getenv("DASHSCOPE_API_KEY")); key != "" {
		cfg.Credentials.apiKey = key
		cfg.Credentials.source = "DASHSCOPE_API_KEY"
	}
	cfg.Credentials.workspaceID = strings.TrimSpace(os.Getenv("DASHSCOPE_WORKSPACE_ID"))
	if err := cfg.Validate(); err != nil {
		return Config{}, err
	}
	return cfg, nil
}

func loadFile(path string, defaults Config) (Config, error) {
	file, err := os.Open(path)
	if err != nil {
		return Config{}, fmt.Errorf("open config %q: %w", path, err)
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, maxConfigBytes+1))
	if err != nil {
		return Config{}, fmt.Errorf("read config %q: %w", path, err)
	}
	if len(data) > maxConfigBytes {
		return Config{}, fmt.Errorf("config %q exceeds %d bytes", path, maxConfigBytes)
	}

	raw := fileConfig{
		ListenAddress:        defaults.ListenAddress,
		WSSPort:              defaults.WSSPort,
		LogLevel:             defaults.LogLevel,
		ASRModel:             defaults.ASRModel,
		ReasoningModel:       defaults.ReasoningModel,
		ReasoningEffort:      defaults.ReasoningEffort,
		TTSModel:             defaults.TTSModel,
		TTSVoice:             defaults.TTSVoice,
		SearchMode:           defaults.SearchMode,
		SystemPrompt:         defaults.SystemPrompt,
		Persona:              defaults.Persona,
		HeartbeatInterval:    yamlDuration(defaults.HeartbeatInterval),
		ConnectionTimeout:    yamlDuration(defaults.ConnectionTimeout),
		FirstResponseTimeout: yamlDuration(defaults.FirstResponseTimeout),
		SessionIdleTimeout:   yamlDuration(defaults.SessionIdleTimeout),
		MaxTurns:             defaults.MaxTurns,
		MaxContextTokens:     defaults.MaxContextTokens,
	}
	decoder := yaml.NewDecoder(bytes.NewReader(data))
	decoder.KnownFields(true)
	if err := decoder.Decode(&raw); err != nil {
		return Config{}, fmt.Errorf("parse config %q: %w", path, err)
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		if err == nil {
			return Config{}, fmt.Errorf("parse config %q: multiple YAML documents are not supported", path)
		}
		return Config{}, fmt.Errorf("parse config %q: %w", path, err)
	}

	defaults.ListenAddress = strings.TrimSpace(raw.ListenAddress)
	defaults.WSSPort = raw.WSSPort
	defaults.LogLevel = strings.ToLower(strings.TrimSpace(raw.LogLevel))
	defaults.ASRModel = strings.TrimSpace(raw.ASRModel)
	defaults.ReasoningModel = strings.TrimSpace(raw.ReasoningModel)
	defaults.ReasoningEffort = strings.ToLower(strings.TrimSpace(raw.ReasoningEffort))
	defaults.TTSModel = strings.TrimSpace(raw.TTSModel)
	defaults.TTSVoice = strings.TrimSpace(raw.TTSVoice)
	defaults.SearchMode = strings.ToLower(strings.TrimSpace(raw.SearchMode))
	defaults.SystemPrompt = strings.TrimSpace(raw.SystemPrompt)
	defaults.Persona = strings.TrimSpace(raw.Persona)
	defaults.HeartbeatInterval = time.Duration(raw.HeartbeatInterval)
	defaults.ConnectionTimeout = time.Duration(raw.ConnectionTimeout)
	defaults.FirstResponseTimeout = time.Duration(raw.FirstResponseTimeout)
	defaults.SessionIdleTimeout = time.Duration(raw.SessionIdleTimeout)
	defaults.MaxTurns = raw.MaxTurns
	defaults.MaxContextTokens = raw.MaxContextTokens
	defaults.Credentials = Credentials{apiKey: strings.TrimSpace(raw.QwenAPIKey), source: "config.yaml"}
	return defaults, nil
}

func (c Config) Validate() error {
	if ip := net.ParseIP(c.ListenAddress); ip == nil || ip.To4() == nil {
		return errors.New("listen_address must be an IPv4 address")
	}
	if err := validatePort("wss_port", c.WSSPort); err != nil {
		return err
	}
	if err := validatePort("discovery_port", c.DiscoveryPort); err != nil {
		return err
	}
	if c.WSSPort == c.DiscoveryPort {
		return errors.New("wss_port and discovery_port must differ")
	}
	if !oneOf(c.LogLevel, "debug", "info", "warn", "error") {
		return errors.New("log_level must be debug, info, warn, or error")
	}
	for name, value := range map[string]string{
		"asr_model": c.ASRModel, "reasoning_model": c.ReasoningModel,
		"tts_model": c.TTSModel, "tts_voice": c.TTSVoice,
	} {
		if strings.TrimSpace(value) == "" || len(value) > 128 || strings.IndexFunc(value, unicode.IsControl) >= 0 {
			return fmt.Errorf("%s must contain 1..128 characters without control characters", name)
		}
	}
	if !oneOf(c.ReasoningEffort, "none", "minimal", "low", "medium", "high") {
		return errors.New("reasoning_effort must be none, minimal, low, medium, or high")
	}
	if !oneOf(c.SearchMode, "auto", "off") {
		return errors.New("search_mode must be auto or off")
	}
	if strings.TrimSpace(c.SystemPrompt) == "" || len(c.SystemPrompt) > 8192 {
		return errors.New("system_prompt must contain 1..8192 characters")
	}
	if strings.TrimSpace(c.Persona) == "" || len(c.Persona) > 2048 {
		return errors.New("persona must contain 1..2048 characters")
	}
	if c.HeartbeatInterval < time.Second || c.HeartbeatInterval > maxHeartbeatInterval {
		return errors.New("heartbeat_interval must be between 1s and 10s")
	}
	if c.ConnectionTimeout < 3*c.HeartbeatInterval || c.ConnectionTimeout > 30*time.Second {
		return errors.New("connection_timeout must cover three heartbeats and be at most 30s")
	}
	if c.FirstResponseTimeout < time.Second || c.FirstResponseTimeout > 5*time.Minute {
		return errors.New("first_response_timeout must be between 1s and 5m")
	}
	if c.SessionIdleTimeout < time.Minute || c.SessionIdleTimeout > 24*time.Hour {
		return errors.New("session_idle_timeout must be between 1m and 24h")
	}
	if c.MaxTurns < 1 || c.MaxTurns > 100 {
		return errors.New("max_turns must be between 1 and 100")
	}
	if c.MaxContextTokens < 1024 || c.MaxContextTokens > 1_000_000 {
		return errors.New("max_context_tokens must be between 1024 and 1000000")
	}
	if key := strings.TrimSpace(c.Credentials.apiKey); key == "" || key == APIKeyPlaceholder {
		return errors.New("Qwen API key is missing: edit config.yaml or set DASHSCOPE_API_KEY")
	}
	if strings.ContainsAny(c.Credentials.workspaceID, "/?#@") {
		return errors.New("DASHSCOPE_WORKSPACE_ID contains invalid characters")
	}
	token := c.DeviceToken.value
	if len([]byte(token)) < 32 || len([]byte(token)) > 256 || token != strings.TrimSpace(token) ||
		strings.IndexFunc(token, func(r rune) bool { return unicode.IsSpace(r) || unicode.IsControl(r) }) >= 0 {
		return errors.New("device_token must contain 32..256 bytes without whitespace")
	}
	return nil
}

func validatePort(name string, value int) error {
	if value < 1 || value > 65535 {
		return fmt.Errorf("%s must be between 1 and 65535", name)
	}
	return nil
}

func oneOf(value string, allowed ...string) bool {
	for _, candidate := range allowed {
		if value == candidate {
			return true
		}
	}
	return false
}
