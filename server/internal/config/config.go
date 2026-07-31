package config

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode"
)

const maxConfigBytes = 64 * 1024

var configKeys = []string{
	"listen_address",
	"wss_port",
	"discovery_port",
	"log_level",
	"provider",
	"region",
	"conversation_mode",
	"model",
	"asr_model",
	"reasoning_model",
	"reasoning_effort",
	"tts_model",
	"tts_voice",
	"voice",
	"search_mode",
	"system_prompt",
	"persona",
	"heartbeat_interval",
	"connection_timeout",
	"first_response_warning",
	"first_response_timeout",
	"session_idle_timeout",
	"max_turns",
	"max_context_tokens",
	"pairing_code_ttl",
	"pairing_max_attempts",
}

type Overrides map[string]string

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

type DeviceToken struct {
	value string
}

func (t DeviceToken) Value() string { return t.value }

func (DeviceToken) String() string { return "<redacted>" }

func (t DeviceToken) GoString() string { return t.String() }

type Config struct {
	ListenAddress        string
	WSSPort              int
	DiscoveryPort        int
	LogLevel             string
	Provider             string
	Region               string
	ConversationMode     string
	Model                string
	ASRModel             string
	ReasoningModel       string
	ReasoningEffort      string
	TTSModel             string
	TTSVoice             string
	Voice                string
	SearchMode           string
	SystemPrompt         string
	Persona              string
	HeartbeatInterval    time.Duration
	ConnectionTimeout    time.Duration
	FirstResponseWarning time.Duration
	FirstResponseTimeout time.Duration
	SessionIdleTimeout   time.Duration
	MaxTurns             int
	MaxContextTokens     int
	PairingCodeTTL       time.Duration
	PairingMaxAttempts   int
	Credentials          Credentials
	DeviceToken          DeviceToken
}

func Defaults() Config {
	return Config{
		ListenAddress:        "0.0.0.0",
		WSSPort:              17806,
		DiscoveryPort:        17807,
		LogLevel:             "info",
		Provider:             "qwen",
		Region:               "singapore",
		ConversationMode:     "realtime",
		Model:                "qwen3.5-omni-plus-realtime",
		ASRModel:             "qwen3-asr-flash",
		ReasoningModel:       "qwen3.6-flash",
		ReasoningEffort:      "none",
		TTSModel:             "qwen3-tts-flash-realtime",
		TTSVoice:             "Ethan",
		Voice:                "Ethan",
		SearchMode:           "off",
		SystemPrompt:         "You are boomPI, a concise and helpful voice assistant. Reply in Simplified Chinese unless the user asks for another language.",
		Persona:              "Natural, young, friendly, and not overly cute.",
		HeartbeatInterval:    10 * time.Second,
		ConnectionTimeout:    30 * time.Second,
		FirstResponseWarning: 15 * time.Second,
		FirstResponseTimeout: 30 * time.Second,
		SessionIdleTimeout:   30 * time.Minute,
		MaxTurns:             20,
		MaxContextTokens:     24_000,
		PairingCodeTTL:       2 * time.Minute,
		PairingMaxAttempts:   5,
	}
}

func Load(path string, cli Overrides) (Config, error) {
	cfg := Defaults()
	if path != "" {
		file, err := os.Open(path)
		if err != nil {
			return Config{}, fmt.Errorf("open config %q: %w", path, err)
		}
		defer file.Close()

		if err := parseYAMLSubset(io.LimitReader(file, maxConfigBytes+1), &cfg); err != nil {
			return Config{}, fmt.Errorf("parse config %q: %w", path, err)
		}
	}
	if err := applyEnvironment(&cfg, os.LookupEnv); err != nil {
		return Config{}, err
	}
	if err := applyOverrides(&cfg, cli); err != nil {
		return Config{}, err
	}

	apiKey, source := lookupAPIKey(os.LookupEnv)
	if apiKey == "" {
		return Config{}, errors.New("missing API credential: set DASHSCOPE_API_KEY")
	}
	workspaceID, _ := os.LookupEnv("DASHSCOPE_WORKSPACE_ID")
	cfg.Credentials = Credentials{
		apiKey:      apiKey,
		source:      source,
		workspaceID: strings.TrimSpace(workspaceID),
	}
	deviceToken, ok := os.LookupEnv("BOOMPI_DEVICE_TOKEN")
	if !ok || strings.TrimSpace(deviceToken) == "" {
		return Config{}, errors.New("missing device credential: set BOOMPI_DEVICE_TOKEN")
	}
	cfg.DeviceToken = DeviceToken{value: deviceToken}

	if err := cfg.Validate(); err != nil {
		return Config{}, err
	}
	return cfg, nil
}

func applyEnvironment(cfg *Config, lookup func(string) (string, bool)) error {
	for _, key := range configKeys {
		environmentKey := "BOOMPI_" + strings.ToUpper(key)
		if value, ok := lookup(environmentKey); ok {
			if err := applyValue(cfg, key, value); err != nil {
				return fmt.Errorf("environment variable %s: %w", environmentKey, err)
			}
		}
	}
	return nil
}

func applyOverrides(cfg *Config, overrides Overrides) error {
	for key, value := range overrides {
		if !isKnownConfigKey(key) {
			return fmt.Errorf("CLI override %q is not supported", key)
		}
		if err := applyValue(cfg, key, value); err != nil {
			return fmt.Errorf("CLI override %q: %w", key, err)
		}
	}
	return nil
}

func lookupAPIKey(lookup func(string) (string, bool)) (string, string) {
	if value, ok := lookup("DASHSCOPE_API_KEY"); ok && strings.TrimSpace(value) != "" {
		return strings.TrimSpace(value), "DASHSCOPE_API_KEY"
	}
	return "", ""
}

func (c Config) Validate() error {
	if !validListenAddress(c.ListenAddress) {
		return errors.New("listen_address must be an IPv4 or IPv6 address")
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
	if c.Provider != "qwen" {
		return errors.New("provider must be qwen in P1")
	}
	if !oneOf(c.Region, "china-beijing", "singapore") {
		return errors.New("region must be china-beijing or singapore")
	}
	if !oneOf(c.ConversationMode, "realtime", "intelligence") {
		return errors.New("conversation_mode must be realtime or intelligence")
	}
	if strings.TrimSpace(c.Model) == "" || len(c.Model) > 128 {
		return errors.New("model must contain 1..128 characters")
	}
	if strings.IndexFunc(c.Model, unicode.IsControl) >= 0 {
		return errors.New("model must not contain control characters")
	}
	for name, value := range map[string]string{
		"asr_model":       c.ASRModel,
		"reasoning_model": c.ReasoningModel,
		"tts_model":       c.TTSModel,
		"tts_voice":       c.TTSVoice,
	} {
		if strings.TrimSpace(value) == "" || len(value) > 128 ||
			strings.IndexFunc(value, unicode.IsControl) >= 0 {
			return fmt.Errorf("%s must contain 1..128 characters without control characters", name)
		}
	}
	if !oneOf(c.ReasoningEffort, "none", "minimal", "low", "medium", "high") {
		return errors.New("reasoning_effort must be none, minimal, low, medium, or high")
	}
	if strings.TrimSpace(c.Voice) == "" || len(c.Voice) > 64 || strings.IndexFunc(c.Voice, unicode.IsControl) >= 0 {
		return errors.New("voice must contain 1..64 characters without control characters")
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
	if c.HeartbeatInterval < time.Second || c.HeartbeatInterval > 5*time.Minute {
		return errors.New("heartbeat_interval must be between 1s and 5m")
	}
	if c.ConnectionTimeout <= c.HeartbeatInterval || c.ConnectionTimeout > 15*time.Minute {
		return errors.New("connection_timeout must exceed heartbeat_interval and be at most 15m")
	}
	if c.FirstResponseWarning < time.Second || c.FirstResponseWarning >= c.FirstResponseTimeout {
		return errors.New("first_response_warning must be at least 1s and less than first_response_timeout")
	}
	if c.FirstResponseTimeout > 5*time.Minute {
		return errors.New("first_response_timeout must be at most 5m")
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
	if c.PairingCodeTTL < 30*time.Second || c.PairingCodeTTL > 10*time.Minute {
		return errors.New("pairing_code_ttl must be between 30s and 10m")
	}
	if c.PairingMaxAttempts < 1 || c.PairingMaxAttempts > 10 {
		return errors.New("pairing_max_attempts must be between 1 and 10")
	}
	if strings.TrimSpace(c.Credentials.apiKey) == "" {
		return errors.New("API credential is required")
	}
	if strings.TrimSpace(c.Credentials.workspaceID) == "" {
		return errors.New("DASHSCOPE_WORKSPACE_ID is required for the selected Qwen region")
	}
	trimmedDeviceToken := strings.TrimSpace(c.DeviceToken.value)
	if len([]byte(trimmedDeviceToken)) < 32 || len([]byte(trimmedDeviceToken)) > 256 {
		return errors.New("BOOMPI_DEVICE_TOKEN must contain 32..256 bytes")
	}
	if trimmedDeviceToken != c.DeviceToken.value || strings.IndexFunc(c.DeviceToken.value, func(current rune) bool {
		return unicode.IsSpace(current) || unicode.IsControl(current)
	}) >= 0 {
		return errors.New("BOOMPI_DEVICE_TOKEN must not contain whitespace or control characters")
	}
	return nil
}

func parseYAMLSubset(reader io.Reader, cfg *Config) error {
	data, err := io.ReadAll(reader)
	if err != nil {
		return fmt.Errorf("read: %w", err)
	}
	if len(data) > maxConfigBytes {
		return fmt.Errorf("file exceeds %d bytes", maxConfigBytes)
	}

	seen := make(map[string]struct{})
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for lineNumber := 1; scanner.Scan(); lineNumber++ {
		raw := strings.TrimSuffix(scanner.Text(), "\r")
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		if raw[0] == ' ' || raw[0] == '\t' {
			return fmt.Errorf("line %d: nested or indented YAML is not supported", lineNumber)
		}
		key, rawValue, ok := strings.Cut(raw, ":")
		if !ok {
			return fmt.Errorf("line %d: expected key: value", lineNumber)
		}
		key = strings.TrimSpace(key)
		if !validConfigKey(key) {
			return fmt.Errorf("line %d: invalid key", lineNumber)
		}
		if _, exists := seen[key]; exists {
			return fmt.Errorf("line %d: duplicate key %q", lineNumber, key)
		}
		seen[key] = struct{}{}

		value, err := parseScalar(rawValue)
		if err != nil {
			return fmt.Errorf("line %d key %q: %w", lineNumber, key, err)
		}
		if err := applyValue(cfg, key, value); err != nil {
			return fmt.Errorf("line %d key %q: %w", lineNumber, key, err)
		}
	}
	if err := scanner.Err(); err != nil {
		return fmt.Errorf("scan: %w", err)
	}
	return nil
}

func parseScalar(raw string) (string, error) {
	value, err := stripInlineComment(strings.TrimSpace(raw))
	if err != nil {
		return "", err
	}
	value = strings.TrimSpace(value)
	if value == "" {
		return "", errors.New("value is empty")
	}
	if strings.HasPrefix(value, "\"") {
		parsed, err := strconv.Unquote(value)
		if err != nil {
			return "", errors.New("invalid double-quoted scalar")
		}
		return parsed, nil
	}
	if strings.HasPrefix(value, "'") {
		if len(value) < 2 || !strings.HasSuffix(value, "'") {
			return "", errors.New("invalid single-quoted scalar")
		}
		return strings.ReplaceAll(value[1:len(value)-1], "''", "'"), nil
	}
	if strings.ContainsAny(value, "[]{}&*!|>") {
		return "", errors.New("only plain or quoted scalar values are supported")
	}
	return value, nil
}

func stripInlineComment(value string) (string, error) {
	var quote rune
	var previous rune
	hasPrevious := false
	escaped := false
	for index, current := range value {
		if quote == '"' {
			if escaped {
				escaped = false
				previous = current
				hasPrevious = true
				continue
			}
			if current == '\\' {
				escaped = true
				previous = current
				hasPrevious = true
				continue
			}
			if current == quote {
				quote = 0
			}
			previous = current
			hasPrevious = true
			continue
		}
		if quote == '\'' {
			if current == quote {
				quote = 0
			}
			previous = current
			hasPrevious = true
			continue
		}
		switch current {
		case '\'', '"':
			quote = current
		case '#':
			if !hasPrevious || unicode.IsSpace(previous) {
				return value[:index], nil
			}
		}
		previous = current
		hasPrevious = true
	}
	if quote != 0 {
		return "", errors.New("unterminated quoted scalar")
	}
	return value, nil
}

func applyValue(cfg *Config, key, value string) error {
	parseInt := func() (int, error) {
		parsed, err := strconv.Atoi(value)
		if err != nil {
			return 0, errors.New("must be an integer")
		}
		return parsed, nil
	}
	parseDuration := func() (time.Duration, error) {
		parsed, err := time.ParseDuration(value)
		if err != nil {
			return 0, errors.New("must be a Go duration such as 10s or 30m")
		}
		return parsed, nil
	}

	switch key {
	case "listen_address":
		cfg.ListenAddress = value
	case "wss_port":
		parsed, err := parseInt()
		if err != nil {
			return err
		}
		cfg.WSSPort = parsed
	case "discovery_port":
		parsed, err := parseInt()
		if err != nil {
			return err
		}
		cfg.DiscoveryPort = parsed
	case "log_level":
		cfg.LogLevel = strings.ToLower(value)
	case "provider":
		cfg.Provider = strings.ToLower(value)
	case "region":
		cfg.Region = strings.ToLower(value)
	case "conversation_mode":
		cfg.ConversationMode = strings.ToLower(value)
	case "model":
		cfg.Model = value
	case "asr_model":
		cfg.ASRModel = value
	case "reasoning_model":
		cfg.ReasoningModel = value
	case "reasoning_effort":
		cfg.ReasoningEffort = strings.ToLower(value)
	case "tts_model":
		cfg.TTSModel = value
	case "tts_voice":
		cfg.TTSVoice = value
	case "voice":
		cfg.Voice = value
	case "search_mode":
		cfg.SearchMode = strings.ToLower(value)
	case "system_prompt":
		cfg.SystemPrompt = value
	case "persona":
		cfg.Persona = value
	case "heartbeat_interval":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.HeartbeatInterval = parsed
	case "connection_timeout":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.ConnectionTimeout = parsed
	case "first_response_warning":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.FirstResponseWarning = parsed
	case "first_response_timeout":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.FirstResponseTimeout = parsed
	case "session_idle_timeout":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.SessionIdleTimeout = parsed
	case "max_turns":
		parsed, err := parseInt()
		if err != nil {
			return err
		}
		cfg.MaxTurns = parsed
	case "max_context_tokens":
		parsed, err := parseInt()
		if err != nil {
			return err
		}
		cfg.MaxContextTokens = parsed
	case "pairing_code_ttl":
		parsed, err := parseDuration()
		if err != nil {
			return err
		}
		cfg.PairingCodeTTL = parsed
	case "pairing_max_attempts":
		parsed, err := parseInt()
		if err != nil {
			return err
		}
		cfg.PairingMaxAttempts = parsed
	case "api_key", "dashscope_api_key", "workspace_id", "device_token":
		return errors.New("secrets and workspace identifiers must come from environment variables")
	default:
		return errors.New("unknown configuration key")
	}
	return nil
}

func validatePort(name string, value int) error {
	if value < 1 || value > 65535 {
		return fmt.Errorf("%s must be between 1 and 65535", name)
	}
	return nil
}

func validListenAddress(value string) bool {
	return net.ParseIP(value) != nil
}

func validConfigKey(value string) bool {
	if value == "" {
		return false
	}
	for _, current := range value {
		if !((current >= 'a' && current <= 'z') || (current >= '0' && current <= '9') || current == '_') {
			return false
		}
	}
	return true
}

func isKnownConfigKey(value string) bool {
	for _, key := range configKeys {
		if value == key {
			return true
		}
	}
	return false
}

func oneOf(value string, allowed ...string) bool {
	for _, candidate := range allowed {
		if value == candidate {
			return true
		}
	}
	return false
}
