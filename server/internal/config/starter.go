package config

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const starterTemplate = `# boomPI 教学版服务端配置
#
# 只需把下一行替换成新加坡区 Model Studio 的 Qwen API Key，然后重新运行。
# 也可以不修改本行，改用 DASHSCOPE_API_KEY 环境变量覆盖。
qwen_api_key: %q

# 教学版默认令牌与板端默认值一致；可信局域网之外请用 BOOMPI_DEVICE_TOKEN 覆盖。
# 不要把本文件、API Key 或 device token 提交到 Git。
device_token: "boompi-teaching-shared-token-v1-2026"

# 以下均有可用默认值；需要时取消注释再调整。
# listen_address: "0.0.0.0"
# wss_port: 17806
# region: "singapore"
# conversation_mode: "intelligence"
# log_level: "info"
`

// CreateStarter creates a private, self-contained teaching configuration.
// Existing files are never overwritten.
func CreateStarter(path string) (bool, error) {
	if strings.TrimSpace(path) == "" {
		return false, errors.New("configuration path is required")
	}
	directory := filepath.Dir(path)
	if directory != "." {
		if err := os.MkdirAll(directory, 0o700); err != nil {
			return false, fmt.Errorf("create configuration directory: %w", err)
		}
	}

	contents := fmt.Sprintf(starterTemplate, APIKeyPlaceholder)

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
