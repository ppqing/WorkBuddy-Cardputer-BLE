package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"

	"github.com/mhrsntrk/ask-master/internal/truncate"
)

// notifyCardputerSent sends a log notification to the MCP client so that
// CodeBuddy displays a message in its chat while waiting for the Cardputer
// response. This gives the user a visible prompt on both screens.
func notifyCardputerSent(ctx context.Context, s *server.MCPServer, msg string) {
	if s == nil {
		return
	}
	notif := mcp.NewLoggingMessageNotification(mcp.LoggingLevelNotice, "ask-master", msg)
	_ = s.SendLogMessageToClient(ctx, notif)
}

// Payload length budget, in runes. The device wraps and scrolls text, so these
// limits only exist to bound one BLE message: the firmware's receive buffer is
// 4 KB and CJK characters cost 3 bytes each in UTF-8, which keeps the worst
// case (question + context + 6 options) comfortably inside it.
//
// Text over the limit is cut with an ellipsis so the human can see that
// something was dropped rather than reading a sentence that stops mid-word.
const (
	maxQuestionRunes = 200
	maxContextRunes  = 120
	maxOptionRunes   = 80
)

func RegisterTools(s *server.MCPServer, bridge Bridger, logger *slog.Logger) {
	if logger == nil {
		logger = slog.Default()
	}

	s.AddTool(
		mcp.NewTool(
			"ask-human",
			mcp.WithDescription("Ask a human a question. Use this tool when you need human input and the user has not responded in chat for 2+ minutes, or when you want to guarantee attention via the Cardputer device."),
			mcp.WithString("question", mcp.Required(), mcp.Description("Question to ask.")),
			mcp.WithString("context", mcp.Description("Additional context.")),
			mcp.WithInteger("timeout", mcp.Description("Timeout in milliseconds.")),
		),
		func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			question, err := req.RequireString("question")
			if err != nil {
				return mcp.NewToolResultError(err.Error()), nil
			}

			payload, err := jsonPayload(map[string]any{
				"type":     "ask",
				"question": truncate.StringWithEllipsis(question, maxQuestionRunes),
				"context":  truncate.StringWithEllipsis(req.GetString("context", ""), maxContextRunes),
			})
			if err != nil {
				logger.Error("marshal ask-human payload failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			notifyCardputerSent(ctx, s, fmt.Sprintf("等待 Cardputer 输入: %s", question))
			reply, err := bridge.SendAndWait(payload, "ask", nil, toolTimeout(req))
			if err != nil {
				if errors.Is(err, context.DeadlineExceeded) {
					logger.Warn("ask-human timed out", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf("[CARDPUTER TIMEOUT] Nobody answered in time. Please answer manually: %s", question)), nil
				}
				if isBridgeOffline(bridge, err) {
					logger.Warn("ask-human offline fallback", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf("[CARDPUTER OFFLINE] Please answer manually: %s", question)), nil
				}
				logger.Error("ask-human failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			return mcp.NewToolResultText(reply), nil
		},
	)

	s.AddTool(
		mcp.NewTool(
			"confirm",
			mcp.WithDescription("Ask a human to confirm something. Use this tool when you need explicit confirmation and the user has not responded in chat for 2+ minutes, or when the decision is critical."),
			mcp.WithString("statement", mcp.Required(), mcp.Description("Statement to confirm.")),
			mcp.WithString("consequence", mcp.Description("Consequence of confirming.")),
			mcp.WithInteger("timeout", mcp.Description("Timeout in milliseconds.")),
		),
		func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			statement, err := req.RequireString("statement")
			if err != nil {
				return mcp.NewToolResultError(err.Error()), nil
			}

			payload, err := jsonPayload(map[string]any{
				"type":     "confirm",
				"question": truncate.StringWithEllipsis(statement, maxQuestionRunes),
				"context":  truncate.StringWithEllipsis(req.GetString("consequence", ""), maxContextRunes),
			})
			if err != nil {
				logger.Error("marshal confirm payload failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			notifyCardputerSent(ctx, s, fmt.Sprintf("等待 Cardputer 确认: %s", statement))
			reply, err := bridge.SendAndWait(payload, "confirm", nil, toolTimeout(req))
			if err != nil {
				// A timeout and an offline device are NOT a human "no": keep them
				// distinguishable from a real rejection.
				if errors.Is(err, context.DeadlineExceeded) {
					logger.Warn("confirm timed out", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf(
						"[CARDPUTER TIMEOUT] Nobody answered in time; treat as NOT confirmed (this is not a human \"no\"): %s",
						statement)), nil
				}
				if isBridgeOffline(bridge, err) {
					logger.Warn("confirm offline fallback", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf(
						"[CARDPUTER OFFLINE] Device unreachable; treat as NOT confirmed (this is not a human \"no\"): %s",
						statement)), nil
				}
				logger.Error("confirm failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			switch strings.ToLower(strings.TrimSpace(reply)) {
			case "y", "true":
				reply = "true"
			case "n", "false":
				reply = "false"
			}

			return mcp.NewToolResultText(reply), nil
		},
	)

	s.AddTool(
		mcp.NewTool(
			"choose",
			mcp.WithDescription("Ask a human to choose from options. Use this tool when you need a choice and the user has not responded in chat for 2+ minutes, or when you want to present options via the Cardputer device."),
			mcp.WithString("question", mcp.Required(), mcp.Description("Question to ask.")),
			mcp.WithArray("options", mcp.Required(), mcp.Description("Available options."), mcp.WithStringItems()),
			mcp.WithString("context", mcp.Description("Additional context.")),
			mcp.WithInteger("timeout", mcp.Description("Timeout in milliseconds.")),
		),
		func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			_ = ctx

			question, err := req.RequireString("question")
			if err != nil {
				return mcp.NewToolResultError(err.Error()), nil
			}

			options := req.GetStringSlice("options", nil)
			if len(options) < 2 || len(options) > 6 {
				return mcp.NewToolResultError("options must have 2-6 items"), nil
			}

			truncatedOptions := make([]string, 0, len(options))
			for _, option := range options {
				truncatedOptions = append(truncatedOptions, truncate.StringWithEllipsis(option, maxOptionRunes))
			}

			payload, err := jsonPayload(map[string]any{
				"type":     "choose",
				"question": truncate.StringWithEllipsis(question, maxQuestionRunes),
				"context":  truncate.StringWithEllipsis(req.GetString("context", ""), maxContextRunes),
				"options":  truncatedOptions,
			})
			if err != nil {
				logger.Error("marshal choose payload failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			notifyCardputerSent(ctx, s, fmt.Sprintf("等待 Cardputer 选择: %s", question))
			reply, err := bridge.SendAndWait(payload, "choose", truncatedOptions, toolTimeout(req))
			if err != nil {
				// Never silently pass off the first option as a human choice.
				if errors.Is(err, context.DeadlineExceeded) {
					logger.Warn("choose timed out", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf(
						"[CARDPUTER TIMEOUT] Nobody chose in time; no option was selected: %s",
						question)), nil
				}
				if isBridgeOffline(bridge, err) {
					logger.Warn("choose offline fallback", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf(
						"[CARDPUTER OFFLINE] Device unreachable; no option was selected: %s",
						question)), nil
				}
				logger.Error("choose failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			return mcp.NewToolResultText(reply), nil
		},
	)

	s.AddTool(
		mcp.NewTool(
			"escalate-to-human",
			mcp.WithDescription("Escalate a question to the human via the Cardputer device. Use ONLY when: (1) You already asked in chat and got no response after 2+ minutes, OR (2) The question is urgent and needs immediate attention. This tool is a 'louder' version of ask-human designed to grab attention."),
			mcp.WithString("question", mcp.Required(), mcp.Description("Question to ask.")),
			mcp.WithString("context", mcp.Description("Additional context.")),
			mcp.WithInteger("chat_wait_time_seconds", mcp.Description("How many seconds you waited in chat before escalating")),
			mcp.WithInteger("timeout", mcp.Description("Timeout in milliseconds.")),
		),
		func(ctx context.Context, req mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			_ = ctx

			question, err := req.RequireString("question")
			if err != nil {
				return mcp.NewToolResultError(err.Error()), nil
			}

			payload, err := jsonPayload(map[string]any{
				"type":      "escalate",
				"question":  truncate.StringWithEllipsis(question, maxQuestionRunes),
				"context":   truncate.StringWithEllipsis(req.GetString("context", ""), maxContextRunes),
				"escalated": true,
			})
			if err != nil {
				logger.Error("marshal escalate-to-human payload failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			notifyCardputerSent(ctx, s, fmt.Sprintf("等待 Cardputer 紧急回复: %s", question))
			reply, err := bridge.SendAndWait(payload, "escalate", nil, toolTimeout(req))
			if err != nil {
				if errors.Is(err, context.DeadlineExceeded) {
					logger.Warn("escalate-to-human timed out", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf("[CARDPUTER TIMEOUT] Nobody answered in time. Please answer manually: %s", question)), nil
				}
				if isBridgeOffline(bridge, err) {
					logger.Warn("escalate-to-human offline fallback", "error", err)
					return mcp.NewToolResultText(fmt.Sprintf("[CARDPUTER OFFLINE] Please answer manually: %s", question)), nil
				}
				logger.Error("escalate-to-human failed", "error", err)
				return mcp.NewToolResultError(err.Error()), nil
			}

			return mcp.NewToolResultText(reply), nil
		},
	)
}

func toolTimeout(req mcp.CallToolRequest) time.Duration {
	timeoutMS := req.GetInt("timeout", 30000)
	if timeoutMS <= 0 {
		timeoutMS = 30000
	}
	return time.Duration(timeoutMS) * time.Millisecond
}

func jsonPayload(payload map[string]any) (string, error) {
	b, err := json.Marshal(payload)
	if err != nil {
		return "", fmt.Errorf("marshal tool payload: %w", err)
	}
	return string(b), nil
}

func isBridgeOffline(bridge Bridger, err error) bool {
	if bridge == nil {
		return true
	}
	if !bridge.DeviceOnline() {
		return true
	}
	if errors.Is(err, errDeviceOffline) {
		return true
	}
	if errors.Is(err, errBridgeDisconnected) {
		return true
	}
	errText := err.Error()
	return strings.Contains(errText, "not connected") || strings.Contains(errText, "disconnected") || strings.Contains(errText, "offline")
}
