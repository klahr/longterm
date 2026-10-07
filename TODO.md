# TODO

## Not possible for now

- **FIDO security keys** (`sk-ed25519`, `sk-ecdsa`). Signing needs the token, over USB or NFC
  with CTAP, and Sailfish OS has no FIDO stack or token access for apps. Revisit if one appears.

## Loose ends

- **Removing a session with a mosh session left running** forgets it on the phone but leaves
  mosh-server on the server, which by default waits for its client for ever. Resuming it just to
  end it would clean up.
- **Typing ahead over mosh** shows plain typing only. mosh's own client also predicts
  backspace and cursor movement within the line.
- **The mosh journal** keeps the screen states and unacknowledged typing in the app's data
  folder in the clear, the session key is in the keychain. The states could be encrypted with a
  key from the keychain too.
- **Scrollback over mosh** only gets the lines that scroll from one screen update to the next,
  as mosh sends screens rather than output. Fast output skips lines, as in mosh itself.

## To check on a phone

The protocol side is covered by `tests/mosh` and `tests/ssh`, which run against real servers.
The Silica pages are not, they were checked against the SDK's QML only.

- The share target: whether Longterm shows in the share menu of Gallery and Files
  (`X-Share-Methods` in the desktop file, `ShareProvider` in `qml/longterm.qml`).
- The reply buttons on agent notifications, which call `reply` on the app's D-Bus interface.
- The app lock on start, after the background delay, and the cover while locked.
- Wake-on-LAN broadcasts from inside the sandbox.
- The arrows key's drag and the double tap that locks Ctrl and Alt.
- Hardware keyboard shortcuts with a Bluetooth keyboard.
- File, folder and multi-file pickers for uploads.
