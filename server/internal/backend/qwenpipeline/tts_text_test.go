package qwenpipeline

import (
	"strings"
	"testing"
)

func TestTTSTextFilterMarkdownAcrossDeltas(t *testing.T) {
	tests := []struct {
		name   string
		chunks []string
		want   string
	}{
		{name: "bold", chunks: []string{"这是 **重", "要** 内容。"}, want: "这是 重要 内容。"},
		{name: "italic and combined", chunks: []string{"这是 *斜", "体* 和 ***重", "点***。"}, want: "这是 斜体 和 重点。"},
		{name: "lists", chunks: []string{"- 第一项\n1.", " 第二项\n* 第三项"}, want: "第一项\n第二项\n第三项"},
		{name: "heading", chunks: []string{"##", " 标题\n正文。"}, want: "标题\n正文。"},
		{name: "blockquote", chunks: []string{"> 引用\n> -", " 嵌套列表"}, want: "引用\n嵌套列表"},
		{name: "inline code", chunks: []string{"运行 `go test", " ./...`。"}, want: "运行 go test ./...。"},
		{name: "code fence", chunks: []string{"```g", "o\nfmt.Println(`你好`)\n`", "``\n结束。"}, want: "fmt.Println(你好)\n结束。"},
		{name: "link", chunks: []string{"请看[外层 [内", "层]](https://example.com/a_(b))，谢谢。"}, want: "请看外层 内层，谢谢。"},
		{name: "escapes", chunks: []string{`保留 \`, `*星号\*，路径 C:\\tmp。`}, want: `保留 *星号*，路径 C:\tmp。`},
		{name: "plain backslash", chunks: []string{`路径 C:\tmp 和 unknown\q。`}, want: `路径 C:\tmp 和 unknown\q。`},
		{name: "Chinese punctuation", chunks: []string{"你好，世界！下一句？", "好的；结束。"}, want: "你好，世界！下一句？好的；结束。"},
		{name: "ordinary symbols", chunks: []string{"公式 2*3=6、2**3、2***3、2 *3、2 **3，星 * 号，变量 A_B，", "标签 #1 和[注意]。"}, want: "公式 2*3=6、2**3、2***3、2 *3、2 **3，星 * 号，变量 A_B，标签 #1 和[注意]。"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			filter := newTTSTextFilter()
			var got strings.Builder
			for _, chunk := range test.chunks {
				got.WriteString(filter.Write(chunk))
			}
			got.WriteString(filter.Finish())
			if got.String() != test.want {
				t.Fatalf("filtered text = %q, want %q", got.String(), test.want)
			}
		})
	}
}

func TestTTSTextFilterKeepsIncompleteMarkdownContent(t *testing.T) {
	filter := newTTSTextFilter()
	got := filter.Write(`普通[内容](unfinished`) + filter.Finish()
	if want := `普通[内容](unfinished`; got != want {
		t.Fatalf("filtered incomplete Markdown = %q, want %q", got, want)
	}
}
