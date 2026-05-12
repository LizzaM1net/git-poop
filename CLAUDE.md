# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`git-poop` is a tiny C++ PTY/pipe proxy that wraps any child process and censors a hardcoded banned word (`"penis"`, `main.cpp:91`) from the child's stdout in real time. The intended use is as a git wrapper (e.g. `git-poop git log`) so profane strings in commit history are asterisked out before reaching the terminal.

## Build

```bash
cmake -B build
cmake --build build
```

Produces `./build/git-poop`. No install step; run directly.

Requires a C++20-capable compiler and `libutil`/`libpty` (Linux: `pty.h` via glibc; macOS: `util.h`).

## Usage

```bash
./build/git-poop <command> [args...]
```

All arguments after the binary name are passed verbatim to `execvp`. The wrapper detects whether its own stdout/stdin are a tty (`isatty`) and takes one of two code paths:

- **PTY mode** (interactive terminal): forks with `forkpty`, puts parent stdin into raw mode, forwards `SIGWINCH` for terminal resize, and proxies all I/O through the PTY master fd. This preserves colour output, readline, and other tty-dependent behaviour.
- **Pipe mode** (non-tty / scripted): forks with three plain `pipe()` pairs for stdin/stdout/stderr. Censoring only applies to stdout in PTY mode (stderr shares the same PTY master), and to both stdout and stderr independently in pipe mode.

Censoring (`censor_inplace`) is case-insensitive, in-place, and replaces every occurrence with `'*'` characters of the same length. It operates on raw read buffers, so words split across two reads will not be caught.

## Key conventions

- The single source file is `main.cpp`; the CMake target name matches the repo name (`git-poop`).
- The banned word and buffer size are file-scope constants (`BANNED`, `BUFSIZE`) near the top of `main.cpp`; change them there.
- Global state (`g_pty_master`, `g_child_pid`, `g_raw_mode`, `g_saved_termios`) is used by signal handlers and the `atexit`-style `restore_termios()` — keep signal handlers async-signal-safe.
- `CMakeLists.txt` intentionally omits a `cmake_minimum_required` line and has a commented-out `target_link_libraries`; this is the current state of the file, not an oversight to fix unless changing dependencies.
