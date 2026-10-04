#!/bin/sh
# Tells Longterm on the phone what Claude is doing: the status dot colour and notifications.
# Usage: longterm-hook.sh <working|done|clear|notify>, the hook's JSON on stdin
[ -w "$SSH_TTY" ] || exit 0

status() { printf '\033]777;longterm-status;%s\007' "$1" > "$SSH_TTY"; }
notify() { printf '\033]777;notify;Claude;%s\007' "$(printf '%s' "$1" | tr -d '\n\007\033')" > "$SSH_TTY"; }

case "$1" in
    working|done) status "$1" ;;
    clear) status none ;;
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
