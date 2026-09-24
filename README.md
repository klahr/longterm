# Longterm

An SSH terminal for Sailfish OS.

- Multiple connections at once, switchable from the app and visible on the cover
- xterm-256color terminal (vim, top, tmux), pinch to zoom, copy and paste
- Saved hosts, Ed25519 keys and remembered passwords kept in the Sailfish Secrets keychain
- Reconnect dropped connections, recover from changed host keys
- Color schemes: Default, Catppuccin Mocha, Dracula

Needs Sailfish OS 5.1 or newer, on aarch64 or armv7hl. The app is sandboxed
and asks for the Internet and Secrets permissions on first start.

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

## License

GPLv3, see [LICENSE](LICENSE).

Bundled third-party code:

- [libssh](https://www.libssh.org/) 0.12.2, LGPL-2.1 (`3rdparty/libssh`, submodule)
- [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3, MIT (`3rdparty/libvterm`, submodule of the
  [Neovim mirror](https://github.com/neovim/libvterm))
- [Source Code Pro](https://github.com/adobe-fonts/source-code-pro), SIL Open Font License 1.1 (`fonts`)
