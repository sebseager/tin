# Tin Isn't Nano

Tin is a portable text editor written in C and inspired by [antirez's kilo](https://github.com/antirez/kilo). It does not depend on any non-standard libraries and uses VT100 escape codes for terminal control rather than `curses`. It should run on many UNIX-based OSes, including macOS.

## Development

In no particular order:

- [x] Read/create files, make changes, and write to disk
- [x] Store file contents in memory using rows containing appendable string buffers
- [x] Write to temporary file first before overwriting target file
- [x] Mash exit command several times to throw out unsaved buffer
- [x] Line numbers
- [x] Find
- [x] Adjust editor size when window is resized
- [x] Status bar with filename, cursor information, and status messages
- [x] Take nonexistent filename argument as new file
- [x] Unicode (UTF8) support
- [ ] Keep track of changes per-line to avoid unnecessary updates
- [ ] Copy/cut/paste
- [x] Auto-indent if previous line began with tabs
- [ ] Search highlighting
- [ ] Replace
- [ ] Undo/redo
- [ ] .tinrc configuration file
- [ ] Mouse scroll support
- [ ] Mouse cursor click/select support
- [x] Help screen

## Usage

Clone the repository and run `make all` to build tin. If the `tin` executable is not located somewhere in your `$PATH`, you'll need to call it with `./tin`.

Open a new file by starting the editor with no arguments: `tin`.

Open a file with `tin path/to/file`.

Within the editor, use the following commands:

```
ctrl-x    exit
ctrl-o    save
ctrl-?    help
```

## Caveats

- For now, Tin does not run on Windows because it relies on the `ioctl` system call. It should work fine in Windows Subsystem for Linux (WSL).
