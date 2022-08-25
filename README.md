# TIN - TIN Isn't Nano

A text editor written in C and based on [antirez's kilo](https://github.com/antirez/kilo).

## Development

In no particular order:

- [x] Read/create files, make changes, and write to disk
- [x] Store file contents in memory using rows containing appendable string buffers
- [x] Write to temporary file first before overwriting target file
- [x] Line numbers
- [x] Find
- [x] Adjust editor size when window is resized
- [x] Status bar with filename, cursor information, and status messages
- [ ] Copy/cut/paste
- [ ] Unicode support
- [ ] Auto-indent if previous line began with tabs
- [ ] Search highlighting
- [ ] Replace
- [ ] Undo/redo
- [ ] .tinrc configuration file
- [ ] Mouse scroll support
- [ ] Mouse cursor click/select support

## Usage

Clone the repository and run `make all` to build tin. If the `tin` executable is not located somewhere in your `$PATH`, you'll need to call it with `./tin`.

Open a new file by starting the editor with no arguments: `tin`.

Open a file with `tin path/to/file`.

Within the editor, use the following commands:

```
ctrl-x                  exit
ctrl-s                  save
ctrl-f <string>         find
```
