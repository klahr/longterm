# Longterm

An SSH terminal for Sailfish OS.

- Multiple connections at once, switchable from the app and visible on the cover
- xterm-256color terminal (vim, top, tmux), pinch to zoom, copy and paste, search
  in the scrollback, links open in the browser, OSC 52 clipboard from the server
- Configurable key toolbar with Ctrl, Alt, arrows, Home/End, PgUp/PgDn and F1-F12
- Swipe the keyboard down for the whole screen, flick through the scrollback
- Password, key and keyboard-interactive (one-time code) login
- Saved hosts with jump hosts, local port forwards and agent forwarding,
  importable and exportable as ssh_config
- Shows the operating system of each host as an icon, detected when connecting
- Ed25519, ECDSA and RSA keys and remembered passwords kept in the Sailfish
  Secrets keychain, private keys exportable with a passphrase
- Reconnects by itself when the network drops or changes, recovers from changed
  host keys, known hosts can be reviewed and removed
- Terminal bell vibrates, or notifies when the session is not on screen, and programs
  can send their own notifications with OSC 9 or OSC 777
- Color schemes: Default, Catppuccin Mocha, Dracula, Gruvbox Dark, Nord,
  Solarized Dark and Light

Needs Sailfish OS 5.1 or newer, on aarch64 or armv7hl. The app is sandboxed
and asks for the Internet and Secrets permissions on first start.

## Claude Code status

Longterm can show what [Claude Code](https://claude.com/claude-code) is doing in a session. The dot
and the line under the connection turn blue for "Working...", red for "Waiting for input..." when
Claude asks for permission or asks a question, and green for "Done". Questions and finished turns
also notify, and tapping the notification opens the session.

On the server, install the plugin from this repository in Claude Code. It needs `jq`:

```
/plugin marketplace add klahr/longterm
/plugin install longterm@longterm
```

The plugin takes effect in Claude sessions started afterwards. Its hooks write to `$SSH_TTY`, the
terminal of the SSH login, and do nothing outside SSH. Inside tmux, `$SSH_TTY` keeps naming the
login that started tmux, so after reattaching from a new connection the status goes nowhere.

Any program can do the same with escape sequences: `ESC ] 777 ; longterm-status ; working|waiting|done BEL`
sets the status, any other value clears it, and `ESC ] 777 ; notify ; title ; body BEL` or
`ESC ] 9 ; body BEL` notifies.

## Building

libssh and libvterm are git submodules, so clone with them:

```
git clone --recursive git@github.com:klahr/longterm.git
```

Building needs the Sailfish SDK, with the project inside the SDK workspace:

```
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build
sfdk -c target=SailfishOS-5.1.0.11-armv7hl build
```

The RPM ends up in `RPMS/`. Clean the build files between targets.

The terminal tests build on the desktop with Qt 5, outside the source tree. The arguments are the
number of fuzz and random-use runs:

```
mkdir build-tests && cd build-tests
qmake ../tests/terminal/terminal.pro CONFIG+=sanitizer CONFIG+=sanitize_address CONFIG+=sanitize_undefined
make && ./terminaltest 200 200
```

## License

GPLv3, see [LICENSE](LICENSE).

Bundled third-party code:

- [libssh](https://www.libssh.org/) 0.12.2, LGPL-2.1 (`3rdparty/libssh`, submodule)
- [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3, MIT (`3rdparty/libvterm`, submodule of the
  [Neovim mirror](https://github.com/neovim/libvterm)), with memory safety and resize fixes from
  `3rdparty/patches/libvterm` applied at build time
- [Source Code Pro](https://github.com/adobe-fonts/source-code-pro), SIL Open Font License 1.1 (`fonts`)
- [Devicon](https://devicon.dev/) operating system logos, MIT (`qml/images/systems`). The logos are
  trademarks of their respective owners.
