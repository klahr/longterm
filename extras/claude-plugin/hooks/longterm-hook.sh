#!/bin/sh
# Tells Longterm on the phone what Claude is doing: the status dot colour and notifications.
# Usage: longterm-hook.sh <working|tool|done|clear|notify>, the hook's JSON on stdin
[ -w "$SSH_TTY" ] || exit 0

# Control characters would end the escape sequence early
clean() { printf '%s' "$1" | tr '\n\t' '  ' | tr -d '\000-\037\177' | cut -c1-200; }
status() { printf '\033]777;longterm-status;%s;%s\007' "$1" "$(clean "$2")" >> "$SSH_TTY"; }
notify() { printf '\033]777;notify;Claude;%s\007' "$(clean "$1")" >> "$SSH_TTY"; }

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
    working|done) status "$1" ;;
    clear) status none ;;
    tool)
        input=$(cat)
        case "$(printf '%s' "$input" | jq -r '.tool_name // empty')" in
            # These wait for an answer, so go red now instead of when the notification comes
            AskUserQuestion|ExitPlanMode) status waiting ;;
            *) status working "$(printf '%s' "$input" | describe_tool)" ;;
        esac
        ;;
    notify)
        input=$(cat)
        # Only a question needs an answer, the idle reminder after a finished turn does not
        case "$(printf '%s' "$input" | jq -r '.notification_type // empty')" in
            permission_prompt|elicitation_dialog) status waiting ;;
        esac
        notify "$(printf '%s' "$input" | jq -r '.message // empty')"
        ;;
esac
[ "$1" = done ] && notify "Done"
exit 0
