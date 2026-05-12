# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`git-poop` is a tiny C++ PTY/pipe proxy that wraps any child process and censors a hardcoded banned word (`"penis"`, `main.cpp:91`) from the child's stdout in real time. The intended use is as a git wrapper (e.g. `git-poop git log`) so profane strings in commit history are asterisked out before reaching the terminal.

## Build

```bash
cmake -B build
cmake --build build
```

Produces `./build/git-poop` (Linux/macOS) or `build\Debug\git-poop.exe` (Windows). No install step; run directly.

Requires a C++20-capable compiler. Linux/macOS additionally need `libutil`/`libpty` (`pty.h` via glibc on Linux; `util.h` on macOS). No extra libraries are needed on Windows.

## Usage

```bash
./build/git-poop <command> [args...]
```

All arguments after the binary name are passed verbatim to `execvp`. The wrapper detects whether its own stdout/stdin are a tty (`isatty`) and takes one of two code paths:

- **PTY mode** (Unix, interactive terminal): forks with `forkpty`, puts parent stdin into raw mode, forwards `SIGWINCH` for terminal resize, and proxies all I/O through the PTY master fd. This preserves colour output, readline, and other tty-dependent behaviour.
- **Pipe mode** (Unix, non-tty / scripted): forks with three plain `pipe()` pairs for stdin/stdout/stderr. Censoring only applies to stdout in PTY mode (stderr shares the same PTY master), and to both stdout and stderr independently in pipe mode.
- **Windows PTY mode** (interactive console, Windows 10 1809+): uses ConPTY (`CreatePseudoConsole`) so the child sees a real TTY and enables colour output and pager invocation. Two threads handle stdin forwarding and stdout draining (stderr merges into the PTY like Unix PTY mode). Two fixes are required: (1) `PSEUDOCONSOLE_INHERIT_CURSOR` prevents the cursor-home init sequence that made PowerShell scroll previous output away; (2) `ClosePseudoConsole()` is called immediately after `WaitForSingleObject(hProcess)` — without it, `conhost.exe` holds the PTY output pipe write-end open and the drain thread blocks in `ReadFile` forever. ConPTY is loaded at runtime via `GetProcAddress`; if absent the pipe path is used as fallback.
- **Windows pipe mode** (redirected/scripted output, or ConPTY unavailable): `CreateProcess` with three anonymous pipes and three threads (stdout drain, stderr drain, stdin forward). Child sees pipes so `isatty()`-gated features are disabled, same trade-off as Unix pipe mode.

Censoring (`censor_inplace`) is case-insensitive, in-place, and replaces every occurrence with `'*'` characters of the same length. It operates on raw read buffers, so words split across two reads will not be caught.

## Key conventions

- The single source file is `main.cpp`; the CMake target name matches the repo name (`git-poop`).
- The banned word and buffer size are file-scope constants (`BANNED`, `BUFSIZE`) near the top of `main.cpp`; change them there. `strncasecmp` is aliased to `_strnicmp` on Windows via `#define` so `censor_inplace` compiles on both platforms unchanged.
- Unix globals (`g_pty_master`, `g_child_pid`, `g_raw_mode`, `g_saved_termios`) live inside `#else` and are used by signal handlers and `restore_termios()` — keep signal handlers async-signal-safe.
- Windows globals (`g_stop_event`, `pfn_create_pty`, `pfn_close_pty`) live inside `#ifdef _WIN32`. The stop event is a manual-reset event set after `WaitForSingleObject(hProcess)` + `ClosePseudoConsole` (PTY path) or just after process exit (pipe path), unblocking `stdin_forward_thread`; the child stdin write handle is only closed after `WaitForMultipleObjects` confirms the stdin thread has exited to avoid a race on the handle. ConPTY types/constants (`HPCON`, `PSEUDOCONSOLE_INHERIT_CURSOR`, `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`) are defined manually with `#ifndef` guards for older SDK compatibility.
- Platform dispatch happens at the bottom of `main()` via `#ifdef _WIN32`: `run_windows(child_argv)` or `run_unix(argv[1], child_argv)`.
- `CMakeLists.txt` intentionally omits a `cmake_minimum_required` line and has a commented-out `target_link_libraries`; this is the current state of the file, not an oversight to fix unless changing dependencies.
