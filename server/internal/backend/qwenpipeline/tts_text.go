package qwenpipeline

import (
	"regexp"
	"strings"
	"unicode"
)

const (
	ttsTextNormal = iota
	ttsTextLinkLabel
	ttsTextAfterLinkLabel
	ttsTextLinkTarget
)

var ttsLinePrefix = regexp.MustCompile(
	`^(?:(?:>+|#{1,6}|[-*+]|[0-9]+[.)])[ \t]+)+`,
)

// This is deliberately smaller than a Markdown parser. It only carries
// syntax that may be split between model deltas; display/history stay raw.
type ttsTextFilter struct {
	lineStart      bool
	skipLine       bool
	escaped        bool
	stars          int
	starBoundary   bool
	emphasisStars  int
	boundary       bool
	linkState      int
	linkDepth      int
	linkLabelDepth int
	prefix         strings.Builder
	linkLabel      strings.Builder
	linkTarget     strings.Builder
	lastNonSpace   rune
}

func newTTSTextFilter() *ttsTextFilter { return &ttsTextFilter{lineStart: true, boundary: true} }

func (f *ttsTextFilter) Write(text string) string {
	var output strings.Builder
	for _, value := range text {
		f.writeRune(&output, value)
	}
	return output.String()
}

func (f *ttsTextFilter) Finish() string {
	var output strings.Builder
	f.flushPrefix(&output)
	if f.linkState != ttsTextNormal {
		output.WriteRune('[')
		output.WriteString(f.linkLabel.String())
		if f.linkState != ttsTextLinkLabel {
			output.WriteRune(']')
		}
		if f.linkState == ttsTextLinkTarget {
			output.WriteRune('(')
			output.WriteString(f.linkTarget.String())
		}
	}
	if f.escaped {
		f.emitRune(&output, '\\')
	}
	f.flushStars(&output, 0, true)
	return output.String()
}

func (f *ttsTextFilter) writeRune(output *strings.Builder, value rune) {
	if f.skipLine {
		if value == '\n' || value == '\r' {
			f.skipLine, f.lineStart = false, true
		}
		return
	}
	if f.lineStart && f.linkState == ttsTextNormal && !f.escaped {
		if isTTSLinePrefixRune(value) {
			f.prefix.WriteRune(value)
			if strings.HasPrefix(strings.TrimLeft(f.prefix.String(), " \t"), "```") {
				f.prefix.Reset()
				f.skipLine, f.lineStart = true, false
			}
			return
		}
		f.flushPrefix(output)
	}
	f.writeInlineRune(output, value)
	if (value == '\n' || value == '\r') && f.linkState == ttsTextNormal {
		f.lineStart = true
	}
}

func (f *ttsTextFilter) flushPrefix(output *strings.Builder) {
	if f.prefix.Len() != 0 {
		text := strings.TrimLeft(f.prefix.String(), " \t")
		f.writeInlineText(output, ttsLinePrefix.ReplaceAllString(text, ""))
		f.prefix.Reset()
	}
	f.lineStart = false
}

func (f *ttsTextFilter) writeInlineText(output *strings.Builder, text string) {
	for _, value := range text {
		f.writeInlineRune(output, value)
	}
}

func (f *ttsTextFilter) writeInlineRune(output *strings.Builder, value rune) {
	switch f.linkState {
	case ttsTextLinkLabel:
		if value == '[' {
			f.linkLabelDepth++
			f.linkLabel.WriteRune(value)
		} else if value != ']' {
			f.linkLabel.WriteRune(value)
		} else if f.linkLabelDepth > 1 {
			f.linkLabelDepth--
			f.linkLabel.WriteRune(value)
		} else {
			f.linkState = ttsTextAfterLinkLabel
		}
		return
	case ttsTextAfterLinkLabel:
		if value == '(' {
			f.linkState, f.linkDepth = ttsTextLinkTarget, 1
			return
		}
		label := f.linkLabel.String()
		f.linkLabel.Reset()
		f.linkState = ttsTextNormal
		f.emitRune(output, '[')
		output.WriteString(label)
		f.emitRune(output, ']')
	case ttsTextLinkTarget:
		if value == '(' {
			f.linkDepth++
			f.linkTarget.WriteRune(value)
		} else if value == ')' {
			f.linkDepth--
			if f.linkDepth == 0 {
				label := f.linkLabel.String()
				f.linkLabel.Reset()
				f.linkTarget.Reset()
				f.linkState = ttsTextNormal
				label = strings.ReplaceAll(strings.ReplaceAll(label, "[", ""), "]", "")
				f.writeInlineText(output, label)
			} else {
				f.linkTarget.WriteRune(value)
			}
		} else {
			f.linkTarget.WriteRune(value)
		}
		return
	}
	if f.escaped {
		f.flushStars(output, value, false)
		if !isMarkdownEscape(value) {
			f.emitRune(output, '\\')
		}
		f.emitRune(output, value)
		f.escaped = false
		return
	}
	switch value {
	case '\\':
		f.flushStars(output, value, false)
		f.escaped = true
	case '[':
		f.flushStars(output, value, false)
		f.linkState, f.linkLabelDepth = ttsTextLinkLabel, 1
	case '`':
		f.flushStars(output, value, false)
	case '*':
		if f.stars == 0 {
			f.starBoundary = f.boundary
		}
		f.stars++
	default:
		f.flushStars(output, value, false)
		f.emitRune(output, value)
	}
}

func (f *ttsTextFilter) flushStars(output *strings.Builder, next rune, end bool) {
	if f.stars == 0 {
		return
	}
	if f.emphasisStars != 0 && f.stars == f.emphasisStars {
		f.emphasisStars = 0
	} else if f.emphasisStars == 0 && f.starBoundary && !end &&
		!unicode.IsSpace(next) && f.stars <= 3 &&
		!(unicode.IsDigit(f.lastNonSpace) && unicode.IsDigit(next)) {
		f.emphasisStars = f.stars
	} else {
		for range f.stars {
			f.emitRune(output, '*')
		}
	}
	f.stars = 0
}

func (f *ttsTextFilter) emitRune(output *strings.Builder, value rune) {
	output.WriteRune(value)
	f.boundary = unicode.IsSpace(value) || unicode.IsPunct(value)
	if !unicode.IsSpace(value) {
		f.lastNonSpace = value
	}
}

func isMarkdownEscape(value rune) bool {
	return strings.ContainsRune("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", value)
}

func isTTSLinePrefixRune(value rune) bool {
	return value >= '0' && value <= '9' || strings.ContainsRune(" \t#>-+*`.)", value)
}
