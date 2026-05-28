# FROXY — FTP Caching Proxy

A lightweight FTP proxy written in C that sits between FTP clients and remote servers, adding transparent file caching, access control policies, and detailed logging.

## Features

- **Transparent proxying** — supports the FTP passive mode (PASV) protocol, relaying LIST and RETR commands to any upstream FTP server
- **File caching** — downloaded files are cached locally; subsequent requests for the same file are served instantly without hitting the remote server
- **TTL-based cache expiry** — a background garbage collector thread automatically evicts stale cache entries
- **Access control policies** — block specific FTP domains for specific client hosts during configurable time intervals
- **Extension filtering** — blocks dangerous file types (`.exe`, `.sh`, `.bat`, `.mp4`, `.iso`)
- **Path traversal protection** — rejects filenames containing `..` or `/`
- **Live config reload** — send `RELOAD_CONFIG` to apply changes to `froxy.conf` without restarting the server
- **Per-session threading** — each client connection is handled on its own thread
- **Structured logging** — timestamped log entries written to `froxy.log`, including cache hit/miss events and periodic traffic statistics

## Architecture

```
FTP Client
    │
    │  connects to port 2121
    ▼
┌─────────────────────────────┐
│         FROXY SERVER        │
│                             │
│  ┌─────────────────────┐   │
│  │   client_handler    │   │  ← one thread per client
│  │   (per thread)      │   │
│  └────────┬────────────┘   │
│           │                 │
│  ┌────────▼────────────┐   │
│  │    Cache Layer      │   │  ← ./cache/ directory
│  │  (hit → serve       │   │
│  │   miss → fetch+save)│   │
│  └────────┬────────────┘   │
│           │                 │
│  ┌────────▼────────────┐   │
│  │  Garbage Collector  │   │  ← background thread, runs every 15s
│  └─────────────────────┘   │
└─────────────────────────────┘
    │
    │  connects to port 21
    ▼
Remote FTP Server
```

## Requirements

- Linux (tested on Ubuntu)
- GCC
- POSIX threads (`pthread`)


## Build

```bash
make
```

This produces two executables: `server` and `client`.

To clean build artifacts:

```bash
make clean
```

## Usage

**Start the proxy server:**
```bash
./server
```
The server starts on port `2121` by default and logs activity to `froxy.log`.

**Connect with the built-in client:**
```bash
./client
```

**Or connect with any standard FTP client** (e.g. `ftp`, `lftp`, `FileZilla`) pointed at `127.0.0.1:2121`.

**Login format** — the username must include the target server using `@`:
```
USER anonymous@ftp.gnu.org
PASS your@email.com
```

**Supported commands:**
| Command | Description |
|---|---|
| `USER user@host` | Connect to upstream FTP server |
| `PASS password` | Authenticate with upstream server |
| `LIST` | List directory contents |
| `RETR filename` | Download a file (cached after first download) |
| `QUIT` | Close the connection |
| `RELOAD_CONFIG` | Reload `froxy.conf` without restarting |


## Configuration

Edit `froxy.conf` before starting the server:

```ini
PORT=2121
MAX_CACHE=5000000      # max cache file size in bytes (default: ~5 MB)
TTL=300                # cache entry lifetime in seconds (default: 5 min)

# Block access to a domain during a time interval for specific clients
# Format: DENY_DOMAIN <target_domain> <start_hour> <end_hour> <client_domain>
# Use * to block all clients
DENY_DOMAIN .uaic.ro 08 20 .y.ro
```

Changes can be applied at runtime with the `RELOAD_CONFIG` command — no restart needed.


## Cache

Cached files are stored in `./cache/`. The garbage collector runs every 15 seconds and removes files older than `TTL` seconds. Files larger than `MAX_CACHE` bytes are not cached.

Cache statistics are written to `froxy.log` every 15 seconds:
```
[STATS] Requests: 10 | Hits: 8 (80.0%) | Traffic: 1.24 MB | Saved: 0.98 MB
```


## Blocked File Types

The following extensions are rejected by the proxy regardless of the upstream server:

`.exe` `.sh` `.bat` `.mp4` `.iso`

---

## Log Format

All activity is written to `froxy.log`:

```
[2026-01-15 20:31:41][NEW SESSION] localhost (127.0.0.1)
[2026-01-15 20:31:41][CMD] localhost: USER anonymous@ftp.gnu.org
[2026-01-15 20:31:59][CACHE MISS] Descarc si salvez 'welcome.msg' ...
[2026-01-15 20:31:59][CACHE SAVED] Fisierul 'welcome.msg' salvat cu succes.
[2026-01-15 20:32:09][CACHE HIT] Servesc 'welcome.msg' din cache.
[2026-01-15 20:32:11][STATS] Requests: 1 | Hits: 1 (100.0%) | Traffic: 0.01 MB | Saved: 0.00 MB
```


## Project Structure

```
FTP-Proxy/
├── server.c          # Proxy server — threading, caching, access control
├── client.c          # Interactive FTP client for testing
├── froxy.conf        # Configuration file
├── makefile          # Build rules
├── cache/            # Cached files (auto-created at runtime)
└── downloads/        # Files downloaded by the client (auto-created at runtime)
```

## Roadmap
- [ ] Cache keyed by `host:filename` to avoid collisions across servers
- [ ] Proxy-level authentication
- [ ] Rate limiting per IP
- [ ] `epoll`-based event loop for higher concurrency
- [ ] FTPS (FTP over TLS) upstream support
- [ ] Embedded HTTP admin interface (stats, cache viewer, live config)


## License

MIT
