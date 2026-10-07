#!/bin/sh
# Codex hooks for Longterm, maps hook events to longterm-status.
# Usage: codex.sh <prompt|tool|tool-done|permission|stop|interrupt|end>, the hook's JSON on stdin
status() { sh "$(dirname "$0")/../bin/longterm-status" "$@"; }
# longterm-status finds the terminal, inside tmux as well
[ -w "$SSH_TTY" ] || [ -n "$TMUX" ] || [ -n "$LONGTERM_STATUS_FILE" ] || exit 0

# What a tool call is about to do, in a few words. apply_patch carries the whole patch as its
# command, the files are on its "*** Update File: <path>" lines
describe_tool() {
    jq -r '
        def base: split("/") | last;
        .tool_name as $tool | (.tool_input // {}) as $in |
        if $tool == "Bash" then (($in.command // "") | if type == "array" then join(" ") else . end
                                 | sub("^(bash|sh|zsh) -l?c "; "") | sub("^\u0027(?<c>.*)\u0027$"; "\(.c)"))
        elif $tool == "apply_patch" then
            ([($in.command // "") | scan("\\*\\*\\* (?:Update|Add|Delete) File: ([^\\n]+)") | .[0] | base]
             | if length == 0 then "Editing files" else "Editing \(join(", "))" end)
        else "Using \($tool | sub("^mcp__"; "") | gsub("__"; " "))"
        end' 2>/dev/null
}

case "$1" in
    prompt|tool-done) status working ;;
    tool) status working "$(describe_tool)" ;;
    permission)
        input=$(cat)
        status waiting
        reason=$(printf '%s' "$input" | jq -r '.tool_input.description // empty')
        [ -n "$reason" ] || reason="Wants to run: $(printf '%s' "$input" | describe_tool)"
        # y approves in Codex's approval prompt, Esc refuses
        status ask Codex "$reason" 'Yes=y' 'No=\e'
        ;;
    stop) status done; status notify Codex Done ;;
    # Cancelled, Codex waits for the next prompt without a Stop
    interrupt) status done ;;
    end) status clear ;;
esac
exit 0
