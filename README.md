# Longterm

An SSH terminal for Sailfish OS.

- Multiple connections at once, switchable from the app and visible on the cover
- xterm-256color terminal (vim, top, tmux), pinch to zoom, copy and paste, search
  in the scrollback, links open in the browser, OSC 52 clipboard from the server
- Configurable key toolbar with Ctrl, Alt, arrows, Home/End, PgUp/PgDn and F1-F12
- Swipe the keyboard down for the whole screen, flick through the scrollback
- Password, key and keyboard-interactive (one-time code) login
- mosh, which keeps the session through network changes, sleep and dead spots
- Files on the server: browse, download into Downloads, upload, rename and delete,
  or download and upload a single file straight from the session menu
- Saved hosts with jump hosts, local and remote port forwards, a SOCKS proxy, agent
  forwarding, environment variables, keepalive and connect timeouts, and attaching to
  a tmux session, importable and exportable as ssh_config
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
and asks for the Internet, Secrets and user directories permissions on first start, the last for
downloading and uploading files.

## mosh

Turn on "Use mosh" for a saved host to run the terminal over [mosh](https://mosh.org). Longterm
logs in over SSH, starts `mosh-server` there and talks to it over UDP, so the host needs mosh
installed and UDP ports 60000 to 61000 reachable. Where mosh-server does not start, the session
says so and goes on as a plain SSH session. The session survives changing networks and the
phone sleeping, and shows how long the server has been silent when packets stop getting through.

The SSH connection stays open next to it for files, port forwards and agent forwarding for as
long as it lasts. If it drops, the terminal goes on over mosh and those stop until the next
connect. Through a jump host, mosh-server is started over the jump but the UDP packets go to the
host directly. mosh-server keeps escape sequences it does not know to itself, so programs cannot
notify or report their status through it, see below. Typing is not predicted locally the way
mosh's own client does.

## Coding agent status

Longterm can show what a coding agent such as [Claude Code](https://claude.com/claude-code) or
[Codex](https://developers.openai.com/codex) is doing in a session. The dot and the line under the
connection turn blue while it works, with what it is doing such as "Reading terminal.cpp...", red for
"Waiting for input..." when it asks for permission or asks a question, and green for "Done".
Questions and finished turns also notify, and tapping the notification opens the session.

Both use the same plugin from this repository, installed on the server. It needs `jq`.

Claude Code, inside Claude:

```
/plugin marketplace add klahr/longterm
/plugin install longterm@longterm
```

Codex, which also needs `hooks = true` under `[features]` in `~/.codex/config.toml`:

```
codex plugin marketplace add klahr/longterm
```

then install Longterm from `/plugins` in Codex and trust its hooks when Codex asks to review them.

The plugin takes effect in sessions started afterwards. Its hooks write to `$SSH_TTY`, the terminal
of the SSH login, and do nothing outside SSH. Inside tmux they write to the terminal of the client
attached at the moment, when that came in over SSH, so reattaching from a new connection works.
Over mosh the status cannot get through, mosh-server drops the escape sequences.

Other tools can report the same way with `extras/agent-plugin/bin/longterm-status`, a plain shell
script:

```
longterm-status working "Running the tests"
longterm-status waiting
longterm-status done
longterm-status clear
longterm-status notify "Build" "Finished in 3 minutes"
```

It writes escape sequences that any program can send directly:
`ESC ] 777 ; longterm-status ; working|waiting|done [; detail] BEL` sets the status, any other
value clears it, and `ESC ] 777 ; notify ; title ; body BEL` or `ESC ] 9 ; body BEL` notifies.

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

The mosh test runs the client against a local `mosh-server` through a relay that drops and reorders
packets, the argument is the share dropped in percent:

```
qmake ../tests/mosh/mosh.pro && make && ./moshtest 30
```

The SSH test logs in to a real server on 127.0.0.1 and goes through the shell, tmux, forwards,
files and mosh, see the top of `tests/ssh/main.cpp` for what it expects. It changes the test
account, so run it in a container. It needs a build of the bundled libssh with server support:

```
cmake -S ../3rdparty/libssh -B libssh-build -DWITH_SERVER=ON -DWITH_GSSAPI=OFF -DUNIT_TESTING=OFF
cmake --build libssh-build
qmake ../tests/ssh/ssh.pro LIBSSH_BUILD=$PWD/libssh-build && make && ./sshtest
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
