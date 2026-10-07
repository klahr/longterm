# Longterm

An SSH terminal for Sailfish OS.

- Multiple connections at once, switchable from the app and visible on the cover
- xterm-256color terminal (vim, top, tmux), pinch to zoom, copy and paste, search
  in the scrollback, links open in the browser, OSC 52 clipboard from the server
- Configurable key toolbar with Ctrl, Alt, arrows, Home/End, PgUp/PgDn and F1-F12, above
  the keyboard or at the top. Tap Ctrl or Alt twice to lock it, drag the arrows key to
  move the cursor
- Snippets of commands typed with one tap, per host or for all, and the history of
  what was typed in the session to type again
- Hardware keyboards: Alt and Meta, Ctrl with symbols, Ctrl+Shift+C/V to copy and paste,
  Ctrl+Tab and Ctrl+PgUp/PgDn between connections, Ctrl+Shift+T for a new one
- Swipe the keyboard down for the whole screen, flick through the scrollback, save or
  share the scrollback, log sessions to Documents/Longterm
- Any monospace font on the device, with Nerd Fonts symbols for prompts whatever the font,
  and color schemes imported from iTerm2, Alacritty, Windows Terminal, Xresources or base16
- Password, key, certificate and keyboard-interactive (one-time code) login
- mosh, which keeps the session through network changes, sleep and dead spots, and
  even through a restart of the app, with typing shown before the server echoes it
- Files on the server: browse, download files and folders into Downloads, open them in
  their app, upload files and folders, rename and delete, or download and upload a single
  file straight from the session menu. Files shared from other apps upload to a server
- Saved hosts in groups and favorites, most recent first, with search. Jump hosts, local
  and remote port forwards, a SOCKS proxy, agent forwarding, environment variables,
  keepalive and connect timeouts, Wake-on-LAN, and attaching to a tmux session,
  importable and exportable as ssh_config
- Keys installed on a host from the app, like ssh-copy-id
- Everything backed up into one file encrypted with a passphrase, and restored from it
- The app can lock with a code, and host key fingerprints show as QR codes
- Shows the operating system of each host as an icon, detected when connecting
- Ed25519, ECDSA and RSA keys and remembered passwords kept in the Sailfish
  Secrets keychain, private keys exportable with a passphrase
- Reconnects by itself when the network drops or changes, recovers from changed
  host keys, known hosts can be reviewed and removed
- Terminal bell vibrates, or notifies when the session is not on screen, and programs
  can send their own notifications with OSC 9 or OSC 777
- Color schemes: Default, Catppuccin Mocha, Dracula, Gruvbox Dark, Nord,
  Solarized Dark and Light

Not supported: FIDO security keys (`sk-ed25519`), as Sailfish OS gives apps no way to talk to them.

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
connect, and a new SSH connection is made in the background for them. Through a jump host,
mosh-server is started over the jump but the UDP packets go to the host directly.

When the app quits, a mosh session stays on the server and the app picks it up on its next
start, screen and all, without logging in again. What it needs for that is in its data folder,
the session key in the keychain. Disconnecting ends the session on the server for good.

On slow links, typing shows underlined before the server echoes it, like mosh's own client
does. Each new line starts out unsure: only once the screen confirmed what was typed on it do
further keys show early, so nothing typed at a password prompt does. It can be turned off in
the settings.

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
mosh-server drops the escape sequences, so under mosh Longterm gives the shell a
`LONGTERM_STATUS_FILE` to write to instead and follows it over SSH, which the plugin does by
itself.

When Claude Code or Codex asks for permission, the notification has buttons to allow or refuse
it without opening the app.

Other tools can report the same way with `extras/agent-plugin/bin/longterm-status`, a plain shell
script:

```
longterm-status working "Running the tests"
longterm-status waiting
longterm-status done
longterm-status clear
longterm-status notify "Build" "Finished in 3 minutes"
longterm-status ask "Deploy" "Push to production?" 'Yes=y\r' 'No=n\r'
```

It writes escape sequences that any program can send directly:
`ESC ] 777 ; longterm-status ; working|waiting|done [; detail] BEL` sets the status, any other
value clears it, and `ESC ] 777 ; notify ; title ; body BEL` or `ESC ] 9 ; body BEL` notifies.
`ESC ] 777 ; longterm-ask ; title ; label=keys|label=keys ; body BEL` notifies with up to four
buttons that type their keys into the session, where `\e` is Esc and `\r` Enter.

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

The SSH test logs in to a real server on 127.0.0.1 and goes through the shell, tmux, agent and
port forwarding, files and folders, mosh with resuming after a restart, the agent status,
installing keys, certificates and backups. It changes the test account, so it runs in a container
with the server, mosh and tmux set up, from the repository's top folder:

```
docker build -t longterm-testenv tests/ssh
docker run --rm -v $PWD:/src:ro -v /tmp/longterm-build:/build -w /build longterm-testenv sh /src/tests/ssh/run.sh
```

Names of tests after `run.sh`, such as `mosh files`, run only those.

## License

GPLv3, see [LICENSE](LICENSE).

Bundled third-party code:

- [libssh](https://www.libssh.org/) 0.12.2, LGPL-2.1 (`3rdparty/libssh`, submodule)
- [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3, MIT (`3rdparty/libvterm`, submodule of the
  [Neovim mirror](https://github.com/neovim/libvterm)), with memory safety and resize fixes from
  `3rdparty/patches/libvterm` applied at build time
- [Source Code Pro](https://github.com/adobe-fonts/source-code-pro), SIL Open Font License 1.1 (`fonts`)
- [Nerd Fonts](https://www.nerdfonts.com/) symbols, MIT (`fonts`), with the icon sets under their
  own licenses as listed in `fonts/NerdFonts-README.md`
- [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library), MIT (`3rdparty/qrcodegen`)
- [Devicon](https://devicon.dev/) operating system logos, MIT (`qml/images/systems`). The logos are
  trademarks of their respective owners.
