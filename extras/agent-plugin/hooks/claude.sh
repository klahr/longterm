#!/bin/sh
# Claude Code hooks for Longterm, maps hook events to longterm-status.
# Usage: claude.sh <prompt|tool|tool-done|notification|stop|end>, the hook's JSON on stdin
status() { sh "$(dirname "$0")/../bin/longterm-status" "$@"; }
# longterm-status finds the terminal, inside tmux as well
[ -w "$SSH_TTY" ] || [ -n "$TMUX" ] || [ -n "$LONGTERM_STATUS_FILE" ] || exit 0

# What a tool call is about to do, in a few words
describe_tool() {
    jq -r '
        def base: split("/") | last;
        .tool_name as $tool | (.tool_input // {}) as $in |
        if $tool == "Bash" then ($in.description // $in.command // "Running a command")
        elif $tool == "Read" then "Reading \($in.file_path // "" | base)"
        elif $tool == "Edit" or $tool == "MultiEdit" then "Editing \($in.file_path // "" | base)"
        elif $tool == "Write" then "Writing \($in.file_path // "" | base)"
        elif $tool == "NotebookEdit" then "Editing \($in.notebook_path // "" | base)"
        elif $tool == "Grep" then "Searching for \($in.pattern // "")"
        elif $tool == "Glob" then "Finding \($in.pattern // "")"
        elif $tool == "WebFetch" then "Fetching \($in.url // "")"
        elif $tool == "WebSearch" then "Searching the web for \($in.query // "")"
        elif $tool == "Agent" or $tool == "Task" then ($in.description // "Running an agent")
        elif $tool == "TodoWrite" then "Planning"
        else "Using \($tool | sub("^mcp__"; "") | gsub("__"; " "))"
        end' 2>/dev/null
}

case "$1" in
    prompt|tool-done) status working ;;
    tool)
        input=$(cat)
        case "$(printf '%s' "$input" | jq -r '.tool_name // empty')" in
            # These wait for an answer, so go red now instead of when the notification comes
            AskUserQuestion|ExitPlanMode) status waiting ;;
            *) status working "$(printf '%s' "$input" | describe_tool)" ;;
        esac
        ;;
    notification)
        input=$(cat)
        # Only a question needs an answer, the idle reminder after a finished turn does not
        message=$(printf '%s' "$input" | jq -r '.message // empty')
        case "$(printf '%s' "$input" | jq -r '.notification_type // empty')" in
            # The first choice of a permission prompt allows it, Esc refuses
            permission_prompt) status waiting; status ask Claude "$message" 'Yes=1' 'No=\e' ;;
            elicitation_dialog) status waiting; status notify Claude "$message" ;;
            *) status notify Claude "$message" ;;
        esac
        ;;
    stop) status done; status notify Claude Done ;;
    end) status clear ;;
esac
exit 0
