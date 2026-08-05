package config

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

const starterHeader = `# boomPI 教学版服务端配置
# 首次运行已写入中国内地 DashScope API Key，其他参数使用代码默认值。
# 不要把本文件提交到 Git，也不要把它发送给他人。
`

// CreateStarter 写入只有 Key 的教学配置，已有文件绝不覆盖。
func CreateStarter(path, apiKey string) (bool, error) {
	if strings.TrimSpace(path) == "" {
		return false, errors.New("configuration path is required")
	}
	apiKey = strings.TrimSpace(apiKey)
	if apiKey == "" || apiKey == APIKeyPlaceholder || strings.IndexFunc(apiKey, func(r rune) bool {
		return r == '\n' || r == '\r' || r == 0
	}) >= 0 {
		return false, errors.New("Qwen API key is empty or invalid")
	}
	directory := filepath.Dir(path)
	if directory != "." {
		if err := os.MkdirAll(directory, 0o700); err != nil {
			return false, fmt.Errorf("create configuration directory: %w", err)
		}
	}

	encoded, err := yaml.Marshal(struct {
		QwenAPIKey string `yaml:"qwen_api_key"`
	}{QwenAPIKey: apiKey})
	if err != nil {
		return false, fmt.Errorf("encode starter configuration: %w", err)
	}
	contents := starterHeader + string(encoded)

	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if errors.Is(err, fs.ErrExist) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("create configuration file: %w", err)
	}
	removeOnError := true
	defer func() {
		if removeOnError {
			_ = os.Remove(path)
		}
	}()
	if _, err := file.WriteString(contents); err != nil {
		_ = file.Close()
		return false, fmt.Errorf("write configuration file: %w", err)
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		return false, fmt.Errorf("sync configuration file: %w", err)
	}
	if err := file.Close(); err != nil {
		return false, fmt.Errorf("close configuration file: %w", err)
	}
	removeOnError = false
	return true, nil
}
