# Deploying omega_server (132on2d, 132on2g, 132on2h)

`omega_server` is the port's own lobby server (PORT-ONLY; design in [ONLINE.md](ONLINE.md) §5 – §6). It does the
following:
- serves the game's online screens: the policies and announcements, sign-up and login, the lobby, its chat and
  player list, game create / list / join, and the agent statistics;
- hands each joiner the host's address;
- (132on2e, optional) relays the in-level UDP traffic of players who cannot reach each other directly (§6.11);
- (132on2g) keeps each agent's contacts and recent players, and filters rude words out of the chat (§3.7, §6.15);
- (132on2h) rate-limits logins and chat per address and per account, and blocks flooding addresses (§9 below).

The level itself runs peer to peer between the players' games (§6.7). The server carries in-level traffic only when
its relay is on and a direct connection fails (§2.3 below).

This page covers running it on a Windows or Linux host. Section references (§) are to ONLINE.md.

## 1. Build

**Windows** (the project's toolchain, Visual Studio 2022+ with CMake):

```
cmake --build local\build-native\Release --target omega_server
```

The result is `local\build-native\Release\omega_server.exe`. It needs no DLLs beyond the system's (Winsock).

**Linux.** The server is portable C++20:
- the sources are `src/port/online/*.cpp` and `src/port/app/omega_server_main.cpp`;
- only `online_tcp.cpp` touches sockets, with BSD sockets outside `_WIN32`;
- it depends on nothing else in the repository.

A direct build:

```
g++ -std=c++20 -O2 -pthread -Isrc/port/online src/port/online/*.cpp src/port/app/omega_server_main.cpp -o omega_server
```

[U] The Linux build has not been run in this project yet (the agent's WSL had no compiler), so expect small fixes the
first time. The Dockerfile (§5.4) runs the same command.

**Deployment files** (132on2g, `tools/deploy/`, no secrets in any of them):
- `omega_server.example.cfg`: every config key with its default (§4);
- `omega_server.service`: the systemd unit (§5.2);
- `Dockerfile` and `Dockerfile.dockerignore`: a container image of the server alone (§5.4).

## 2. Ports and network

| What | Protocol / port | Who connects |
|---|---|---|
| omega_server | TCP **10075** (the original's `MAS_PORT`; `port=` / `--port`) | every player's game, and `omega_server admin` |
| The relay (optional, 132on2e) | UDP, `relay_port=` / `--relay-port` (off by default; for example **10076**) | port players' games whose direct connection failed (§2.3) |
| The in-level game | UDP **3658** on the hosting player's PC (the original's DME peer port; `online_game_port=` in the game), sent to the server with HostEndpoint | the joiners, straight to the host (§2.4) |

- **Bind.** The default bind is `127.0.0.1` (this machine only). Use `bind=0.0.0.0` for a LAN or public server and
  open TCP 10075 in the firewall:
  - Windows: `New-NetFirewallRule -DisplayName omega_server -Direction Inbound -Protocol TCP -LocalPort 10075 -Action Allow`;
  - Linux with ufw: `ufw allow 10075/tcp`.
- **`public_ip`.** Set it to the address the players reach the server at. The login reply's ConnectInfo carries it
  (§3.3), and a PS2 client would move to it (§8.3).
- **Clients.** Each player sets `online_server=HOST:10075` in the game's `port_settings.txt`, or starts
  `omega_play --frontend --online-server HOST:10075`.

### 2.1 How a joiner reaches the host

A joiner's game tries, in this order (132on2e):
1. **Direct** to the address omega_server hands over: the host's address as the server sees it, with the host's game
   port, or the port its router mapped with UPnP (§2.4). This works on one LAN, over a VPN, when the host's router
   forwards UDP 3658 (UPnP or by hand), and when the host has a public address.
2. **Through the relay** (§2.3), when the direct join had no answer and the server's relay is on.

The game's log names the path: `online: in-level path: direct to the host` or `online: in-level path: through
omega_server's relay (port-only)`. PS2 sessions are never offered the relay (§8.1).

### 2.2 Players behind NAT without the relay

Without the relay, a joiner must reach the host's UDP port: one LAN, a virtual LAN / VPN (WireGuard, Tailscale,
ZeroTier and the like), or the host's router forwarding UDP 3658 (UPnP, §2.4, or by hand, §2.5).

### 2.3 The relay (PORT-ONLY)

Turn it on with `relay_port=10076` in the config (or `--relay-port 10076`) and open that UDP port:
- Windows: `New-NetFirewallRule -DisplayName "omega_server relay" -Direction Inbound -Protocol UDP -LocalPort 10076 -Action Allow`;
- Linux with ufw: `ufw allow 10076/udp`;
- a cloud host: allow UDP 10076 in its security group / firewall too.

It binds the same address as the TCP port (`bind`). The players need no setting: the server tells each port client
the relay's port at login, and the client sends its datagrams to the server's address it already uses. A client
sets `online_relay=off` to never use it, or `force` to always use it (tests).

| Key | Default | Effect |
|---|---|---|
| `relay_port` | 0 (off) | the relay's UDP port |
| `relay_max_endpoints` | 64 | logged-in port players with a relay endpoint at once (later logins get none and play direct only) |
| `relay_packet_rate` | 200 | datagrams per second per player (a one-second burst); more are dropped |
| `relay_byte_rate` | 262144 | payload bytes per second per player (a one-second burst) |
| `relay_idle` | 60 | seconds a player may stay silent before it must register again (the game registers every 5 s) |

**Safety.** A relay datagram must carry the player's 16-byte random token, which the server gives only over that
player's logged-in TCP session; a datagram is forwarded only to another player of the same game, so the relay cannot
be used to reach other hosts. The endpoint closes with the login (logout, kick, ban). Like the TCP framing, the relayed
game traffic is not encrypted.

**Bandwidth.** Every relayed datagram crosses the server twice. A two-player game measured at most about 44 datagrams and
2 KB per second per player (§6.11; the current in-level traffic, which sends a move only when the player moved, is
far less); four players about double each player's share. 64 endpoints at the default rate
limits bound the worst case near 16 MB/s in, the same out.

**Watching it.** The log has `relay: endpoint ...` when a player gets an endpoint, its observed address when it
registers, the first datagram each way per game, a status line every 10 s while traffic flows (`... F forwarded (B
bytes); dropped: ...`) and the counts when an endpoint closes. The console's `relay` command prints the status with
one line per endpoint.

### 2.4 The host's game port, UPnP, PCP and NAT-PMP (PORT-ONLY behaviour on the original's port)

The hosting player's game opens UDP **3658**, the original game's fixed peer port (ONLINE.md §6.12), so it can be
forwarded once. If 3658 is busy (another program, a second game on the same PC), the game logs it and uses a port the
system picks for that game. The game setting `online_game_port=` changes it (0: always system-picked).

With `online_upnp=1` (the default) the hosting game asks the home router to forward that UDP port when the game is
created, trying three methods in order (ONLINE.md §6.13):
1. **UPnP IGD**;
2. **PCP** (RFC 6887);
3. **NAT-PMP** (RFC 6886), the predecessor of PCP that Apple routers and many open firmwares speak.

PCP and NAT-PMP use the default gateway's UDP port 5351. Their mappings last 7200 s and the game renews them at half
that. The game removes the forward when the game is left or the program exits. `online_upnp=0` turns all three off.

The log says what happened (`online: port mapping: ...`):
- mapped, with the method, the gateway, the external address and port, and the lifetime;
- or failed, with each method's reason (no router answered, refused, ...).

omega_server then gives joiners the external port. If the router's external address differs from the address the
server sees (a second NAT, for example a carrier's), the server logs a warning: those players need the relay. A
router with none of the three needs the port forwarded by hand (§2.5).

**What the player sees** (PORT-ONLY, port clients only; never on a PS2): one line on the front end's notice overlay
and on the level's HUD ticker:
- `Connection: UPnP|PCP|NAT-PMP mapped ext port N`: the host's forward is in place;
- `Connection: port mapping failed - see the port-forward guide (UDP N)`: forward the port by hand (§2.5), or have
  the joiners use the relay;
- `Connection: hosting on UDP N (port mapping off - ...)`: `online_upnp=0`;
- `Connection: direct to the host` / `Connection: relay through omega_server`: the joiner's in-level path.

### 2.5 Forwarding the game port by hand (routers without UPnP, PCP or NAT-PMP)

For the player who **hosts** games (joiners need nothing):
1. Give the PC a fixed LAN address (a DHCP reservation in the router, or a static address).
2. In the router's *Port forwarding* / *Virtual server* / *NAT* page, forward **UDP 3658** (external) to that PC's
   address, **UDP 3658** (internal). Only UDP; no TCP port is needed by the game.
3. Allow it in the PC's firewall:
   - Windows (an administrator PowerShell): `New-NetFirewallRule -DisplayName "Omega Strain online game" -Direction
     Inbound -Protocol UDP -LocalPort 3658 -Action Allow` (or allow `OmegaStrain.exe` / `omega_play.exe` when Windows
     asks on the first hosted game);
   - Linux with ufw: `ufw allow 3658/udp`.
4. Keep `online_game_port=3658` (the default). If you change it, forward that port instead. Turn UPnP off
   (`online_upnp=0`) if you prefer the router to keep only your manual rule.
5. Check: host a game and look for `game port 3658` in the game's log; a joiner's log should say `in-level path:
   direct to the host`.

A carrier-grade NAT (the router's WAN address is in 100.64.0.0/10 or another private range) cannot be forwarded from
home: use the relay or a VPN.

## 3. Files

All paths are relative to the working directory unless given in full. The service account needs to write there.

| File | Key / option | Default | Contents |
|---|---|---|---|
| config | `--config` | none | `key=value` lines (below) |
| accounts | `accounts=` / `--accounts` | `omega_accounts.txt` | one account per line: id, name, salt, PBKDF2 digest, iterations, admin, ban, created, accepted policy version, ban reason. **No passwords** |
| statistics | `stats_file=` / `--stats` | `omega_stats.txt` | each account's agent statistics record and the last `max_results` finished games (§6.10) |
| policy | `policy_file=` / `--policy` | `omega_policy.txt` | the usage (TOS) and privacy pages and the announcements (MOTD), with their version. Written on the first start if missing |
| social (132on2g) | `social_file=` / `--social` | `omega_social.txt` | the contacts, the pending contact invitations and the recent players, by account id (§6.15) |
| log | `log=` / `--log` | `omega_server.log` | appended, rotated at `log_max_bytes` (§8); the same lines go to stdout |

`--data-dir DIR` (132on2g) makes DIR the working directory first, so every relative path above (and `--config`)
resolves there. A service should always pass it.

**Crash safety** (132on2g; ONLINE.md §6.14).
- **Atomic and durable writes.** Each store is written to `FILE.tmp`, flushed to the disk, and renamed over the old
  file write-through. A crash or a power cut leaves the old or the new file, never a torn one, and a save that
  finished is on the disk.
- **When the stores are written.** The accounts, the policy and the social store at every change; the statistics at
  most 30 s apart and at every stop (so a hard kill or a power cut loses at most the last 30 s of statistics). A
  graceful stop (§5.5) writes everything.
- **Recovery at start.** A store that does not parse is moved aside to `FILE.corrupt-<unix time>` and the newest backup
  that parses (`FILE.bak.1`, then `.bak.2` ...) is copied over it, logged as `store FILE: recovered from FILE.bak.N`.
  If no backup parses, the bad file is put back and the server refuses to start (rather than start with no accounts):
  fix or replace the file by hand. A leftover `FILE.tmp` (a write that was cut short) is logged and removed; the
  file itself is the last complete save.

**Backups.** Before a write replaces a file, the old file becomes `FILE.bak.1`, the older ones move up, and anything
past `backups` (default 5, 0 turns them off) is dropped.
- The accounts and statistics are backed up at most every `backup_interval` (default `1h`).
- The policy file is backed up at every edit.
- To restore by hand: stop the server and copy `FILE.bak.N` over `FILE`.
- Copy the files off the machine as part of the host's normal backups. The `.bak` files guard against a bad edit,
  not a lost disk.

## 4. Configuration

An example `omega_server.cfg` (also in `tools/deploy/omega_server.example.cfg`). Every key is optional. Comments must be on their own lines (a `#` after a value is
part of the value). The values are the defaults, except those marked "example":

```
# --- identity ---
server_name=omega_server
# example: the address the players reach (default 127.0.0.1)
public_ip=203.0.113.10
# example: every interface (default 127.0.0.1, this machine only)
bind=0.0.0.0
port=10075
# players per game, 2..4
max_players=4
# 1 (the current development default) lets each player's own cheats work online, for that player only (§6.5)
allow_cheats=1
# gameplay (the default: a joiner's gameplay mods must be the game creator's) | any | cosmetic | none (§6.9)
mod_policy=gameplay
# example; empty (the default) disables token admin
admin_token=change-me-to-something-long

# --- files ---
accounts=omega_accounts.txt
stats_file=omega_stats.txt
policy_file=omega_policy.txt
social_file=omega_social.txt
log=omega_server.log
# text or json (one JSON object per line)
log_format=text
# the log rotates at this size in bytes (0: never), keeping log_keep older files (§8)
log_max_bytes=10485760
log_keep=5
# finished games kept
max_results=200
backups=5
# seconds, or 30m / 2h / 1d
backup_interval=1h

# --- security and limits (0 turns a limit off; §6.10) ---
pbkdf2_iterations=120000
# threads computing the password digests off the server loop (0: half the CPUs, 1..4; 132on2h)
password_threads=0
max_conns_per_ip=16
# frames per second per connection, and the burst
message_rate=100
message_burst=1000
# failed logins per name and address (4x that per address) within the window lock it for the lockout
login_failures=5
login_window=10m
login_lockout=5m
# new accounts per address per hour
registrations_per_hour=20

# --- anti-abuse (132on2h, §9 and ONLINE.md §6.16; 0 turns one off) ---
# login and account-creation attempts per address per minute; login attempts per account name per minute
login_attempts_per_ip=30
login_attempts_per_account=10
# lobby chat lines per player per minute and their burst, and per address per minute
chat_per_minute=30
chat_burst=10
chat_per_minute_ip=120
# dropped chat lines within a minute that disconnect the player
chat_flood_kick=5
# flood disconnects (message_rate, chat_flood_kick) from one address within flood_window block it for flood_block
flood_block_after=3
flood_window=10m
flood_block=15m

# --- lobby chat (132on2g, §6.15) ---
# 1 masks rude words for every player who has not chosen for themself (port players can: online_chat_filter)
chat_filter=1
# example (empty, the default: the built-in list): a word file, one per line; a first line "+" keeps the built-in list
chat_filter_words=omega_chat_words.txt

# --- the in-level relay (§2.3; 0 = off) ---
# example (default 0)
relay_port=10076
relay_max_endpoints=64
relay_packet_rate=200
relay_byte_rate=262144
relay_idle=60
```

Start it with `omega_server --config omega_server.cfg`. Command-line options win over the file.

## 5. Running it

### 5.1 In a console

`omega_server --config omega_server.cfg` (Windows or Linux).
- Its stdin is the admin console (§6 below).
- `quit`, Ctrl+C or `shutdown now` stops it cleanly (§5.5): the games keep their results, the stores are saved, and
  the log ends with `omega_server stopping`.

### 5.2 Linux: systemd

The server stops gracefully on SIGTERM. A service has no stdin, so it runs with `--no-console` and is administered
remotely (§6). The unit is `tools/deploy/omega_server.service`:

```
[Unit]
Description=omega_server (Syphon Filter: The Omega Strain port lobby)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=omega
Group=omega
WorkingDirectory=/var/lib/omega_server
ExecStart=/opt/omega_server/omega_server --no-console --data-dir /var/lib/omega_server --config omega_server.cfg
KillSignal=SIGTERM
TimeoutStopSec=30
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/omega_server

[Install]
WantedBy=multi-user.target
```

Install:

```
sudo install -D -m 0755 omega_server /opt/omega_server/omega_server
sudo useradd --system --home-dir /var/lib/omega_server --shell /usr/sbin/nologin omega
sudo install -d -o omega -g omega -m 0750 /var/lib/omega_server
sudo install -o omega -g omega -m 0640 omega_server.cfg /var/lib/omega_server/   # your copy of the example
sudo cp tools/deploy/omega_server.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now omega_server
```

- The log: `journalctl -u omega_server -f` (stdout), and the rotated log file in `/var/lib/omega_server`.
  Add `--log-format json` to `ExecStart` for a log collector.
- `systemctl stop omega_server` is a graceful shutdown now; `Restart=on-failure` brings back a crashed server.
- A warned stop or restart (the players get 60 s of notice): `omega_server admin 127.0.0.1:10075 --token TOKEN
  shutdown 60 maintenance` (or `restart 60`). An in-process restart (§5.5) keeps the process, so systemd sees no stop.

[U] The unit has not been run on a Linux host in this project (no Linux host or compiler here).

### 5.3 Windows: a service

`omega_server --service` runs under the Service Control Manager (132on2g): the service's Stop and the system's
shutdown are a graceful shutdown now, and the server reports Running / Stop pending / Stopped to the SCM. Started
any other way, `--service` refuses (`not started by the Service Control Manager (error 1063)`, exit code 1).

From an elevated PowerShell (the paths are examples; keep the data folder outside `Program Files` so the service can
write it):

```
New-Item -ItemType Directory C:\omega_server\data
Copy-Item omega_server.exe C:\omega_server\
Copy-Item omega_server.example.cfg C:\omega_server\data\omega_server.cfg   # then edit it
sc.exe create omega_server binPath= "C:\omega_server\omega_server.exe --service --data-dir C:\omega_server\data --config omega_server.cfg" start= auto obj= "NT AUTHORITY\LocalService" DisplayName= "omega_server"
sc.exe description omega_server "Syphon Filter: The Omega Strain port lobby server"
sc.exe failure omega_server reset= 86400 actions= restart/5000/restart/5000/restart/60000
icacls C:\omega_server\data /grant "NT AUTHORITY\LocalService:(OI)(CI)M"
New-NetFirewallRule -DisplayName omega_server -Direction Inbound -Program C:\omega_server\omega_server.exe -Action Allow
Start-Service omega_server
```

- `LocalService` is an unprivileged account; it needs only modify rights on the data folder (the `icacls` line).
- `--no-console` is implied. The log is the file in the data folder (there is no stdout).
- `Stop-Service omega_server` stops it gracefully. Remove it with `sc.exe delete omega_server` after a stop.
- A warned stop or restart and the health check: `omega_server admin` (§6).

[U] The SCM path is built and its refusal outside the SCM was checked; installing the service needs an elevated
shell, which the agent did not use. The 132on2d alternative still works: Task Scheduler, *At startup*, *Run whether
user is logged on or not*, `omega_server.exe` with `--no-console --data-dir C:\omega_server\data --config
omega_server.cfg`.

### 5.4 Docker

`tools/deploy/Dockerfile` builds the server alone (a `g++` build stage, then a slim Debian image running as the
unprivileged user `omega`). `Dockerfile.dockerignore` sends only the server's sources to the build (BuildKit, the
default builder since Docker 23).

```
docker build -f tools/deploy/Dockerfile -t omega_server .
docker volume create omega_data
docker run -d --name omega_server --restart unless-stopped --stop-timeout 30 \
    -p 10075:10075/tcp -p 10076:10076/udp -v omega_data:/data omega_server
```

- `/data` holds the config, the stores and the log. Put your `omega_server.cfg` there (from
  `/opt/omega_server/omega_server.example.cfg` in the image) with `bind=0.0.0.0` and your own `admin_token`. Without
  one the server starts with the defaults on `0.0.0.0:10075` (relay off). No secret is built into the image.
- `docker stop` sends SIGTERM: a graceful shutdown now. `--stop-timeout 30` leaves it time to save.
- Admin and health check from the host: `omega_server admin 127.0.0.1:10075 --token TOKEN status`, or inside the
  container: `docker exec omega_server /opt/omega_server/omega_server admin 127.0.0.1:10075 --token TOKEN status`.
- The in-level game traffic is between the players' games (§2.1), so only the lobby port and the relay's port are
  published.

[U] The image has not been built here (no Docker on the agent's machine); its build command is the Linux build of §1.

### 5.5 Stopping and restarting

| Command (console or `omega_server admin`) | Effect |
|---|---|
| `shutdown [now\|SECONDS\|30s\|5m] [reason]` | stops the server after the delay (default 60 s) |
| `restart [now\|SECONDS\|30s\|5m] [reason]` | the same, then starts again in the same process |
| `shutdown cancel` / `restart cancel` | cancels a pending stop |

- **The warnings.** Every logged-in player is told at once (`The server shuts down in 60 s: maintenance`) and again at
  60, 30, 10 and 5 s left; a cancel is announced too. Port players get a notice; a player without notices (a PS2)
  gets the same text as a private chat message from `omega_server`.
- **When it is due.** Each open game is closed and its result kept (§6.10). Every player gets the last notice and is
  logged out, the statistics and the social store are saved, and the connections close:
  `lifecycle: N connection(s) closed (...)`.
- **Shutdown** then ends the process (exit code 0). **Restart** keeps the process and its sockets: the config file,
  the stores and the policy file are read again and the log says `listening on ...; ... (restart N)`. Use it after
  editing the config or a store by hand. Changing `bind`, `port` or `relay_port` needs a process restart
  (`systemctl restart`, `Restart-Service`, `docker restart`).
- **Signals.** Ctrl+C, SIGTERM, the service's Stop, the console's `quit`, `--run-seconds` and the end of the
  `--parent-pid` process are a `shutdown now`.
- `status` shows a pending stop (`shutdown pending in N s: reason`).

## 6. Administration

Admin commands run in the console, or remotely:

```
omega_server admin HOST:10075 --token TOKEN <command...>
omega_server admin HOST:10075 --user NAME --password PASSWORD <command...>
```

- `--token` needs `admin_token=` in the config.
- `--user` needs an account made admin with `admin NAME on`.
- Remote admin crosses the same unencrypted framing as logins (§7), so use it over a trusted network or a tunnel.
- A logged-in admin's game client can send the same lines (the port's Admin extension). Every admin action is logged
  as `admin: ...`.

| Command | Effect |
|---|---|
| `help` | the list |
| `players` / `games` / `accounts` | who is online (with platform and game), the open games (host address, players), every account (admin, policy version, ban) |
| `ban NAME 30m\|2h\|7d\|SECONDS\|perm [reason]` / `unban NAME` | a ban kicks the player now and refuses its logins (with the reason and time left) |
| `kick NAME [reason]` | disconnects the player |
| `close WORLD` | closes a game (its players get a notice) |
| `broadcast TEXT` | a notice to every port client online |
| `cheats [on\|off]` | shows or sets `allow_cheats` live |
| `mods` | the mod policy and each connection's mod report |
| `relay` | the relay's limits and counters, one line per endpoint (132on2e) |
| `status` | a health check: uptime, connections by kind, players logged in and in games, games, accounts and bans, failed-login locks, statistics, and the relay's endpoints and bytes (132on2f; sample below) |
| `admin NAME on\|off` | makes an account an admin |
| `password NAME NEWPASSWORD` | sets a password (the log hides it) |
| `create NAME PASSWORD` | makes an account |
| `stats [NAME]` | every account's statistics record, or one in full |
| `results [N]` | the last N finished games (default 10) and each player's change |
| `limits [clear]` | the limits, the current login locks, the address blocks and the abuse counters (132on2h); `clear` lifts the locks, the account-creation counts, the login / chat rate buckets and the address blocks |
| `policy` | shows the usage and privacy pages and their version |
| `policy usage TEXT` / `policy privacy TEXT` / `policy privacy clear` | edits a page (`\n` is a new line). It **bumps the version**, so every account must accept again at its next connect. An empty privacy page is skipped |
| `announce list` / `announce add TEXT` / `announce del N` / `announce clear` | the announcement pages (MOTD), one page each, shown after the policies |
| `motd TEXT` | replaces announcement 1 (adds it if there is none) |
| `shutdown [now\|SECONDS\|5m\|cancel] [reason]` / `restart ...` | a warned graceful stop or in-process restart (132on2g; §5.5) |
| `contacts NAME` / `recent NAME` | an account's contact list and pending invitations, and its recent players (132on2g; ONLINE.md §6.15) |
| `filter [on\|off]` / `filter test TEXT` | the chat filter's default for players who have not chosen (not saved; set `chat_filter=` for good), and what it makes of a text (132on2g) |
| `quit` / `exit` | stops the server (console only; a `shutdown now`) |

**Health check.** `status` answers in one call, so a monitor (a scheduled task, a cron job, a container health
probe) can run `omega_server admin 127.0.0.1:10075 --token TOKEN status` and treat a first line that starts with
`status ok:` as healthy. A server that does not answer makes the command fail with a non-zero exit code. Sample:

```
status ok: omega_server, up 1d 01:01:01 (90061 s, since unix 1760000000)
  connections 3 (port 2, original 1), players logged in 2 (in a game 2, admins 0)
  games 1 (2 players in them, max_players 4)
  accounts 3 (banned 1), failed-login locks 0
  statistics: 2 records, 1 results
  relay: UDP port 10076, endpoints 2/64, datagrams received 4210, forwarded 4180 (612344 bytes), dropped 30
  social: 2 contact entries, 0 pending invitation(s); chat_filter=1
  abuse: 0 blocked address(es); since the start 3 login attempt(s) refused, 12 chat message(s) dropped, 1 flood disconnect(s)
  tick: 1805420 loop iterations, 52211 frames; work per iteration (last 4096) p50 0.012 ms, p99 0.410 ms, max 9.877 ms
  process: cpu 61.20 s, working set 7.1 MB (peak 12.3 MB)
  password work (PBKDF2): 41 job(s) since the start, 0 pending (worker threads)
```

`relay: off (relay_port=0)` replaces the relay line when the relay is off. `(unsaved changes)` after the statistics
means a save is pending (the server writes statistics at most 30 s apart, and at exit). The last four lines (132on2h)
are for capacity checks:
- `abuse:` the addresses blocked now and the anti-abuse counters (`limits` has the detail).
- `tick:` the server loop. "Work per iteration" is the time each loop pass spent on work (the wait for traffic
  excluded), over the last 4096 passes. A p99 of a few milliseconds is normal; tens of milliseconds mean the server
  is saturated (see §12).
- `process:` the CPU time used since the start, and the working set (memory) now and at its peak.
- `password work`: the PBKDF2 digests computed for logins and sign-ups. A `pending` count that stays high means the
  `password_threads` cannot keep up (raise them, or lower `pbkdf2_iterations` only if you must).

## 7. Editing the TOS and MOTD

**From the console** (recommended): `policy usage ...`, `policy privacy ...`, `announce ...` and `motd ...` take
effect at once and are saved to the policy file.

**By hand.** Edit `omega_policy.txt` while the server is stopped (it reads the file at start):

```
version=3
[usage]
The usage policy (the game's "Terms of use" page). Plain text; the game wraps it.
[privacy]
The privacy policy page. Leave the section empty to skip the page.
[announcement]
First announcement (the MOTD).
[announcement]
A second announcement page.
```

- Raise `version=` when the usage or privacy text changes, so that every player is asked to accept it again (the
  console does this itself). Announcements need no version change.
- Keep the text short. The game shows each page in its own scrolling box.

## 8. Logs

- **Text** (the default): `YYYY-MM-DD HH:MM:SS line`, in local time.
- **JSON** (`log_format=json`): one object per line, for a log collector:

  ```
  {"ts":"2026-10-20T23:06:04Z","level":"info","event":"result","msg":"result: game 1 '...', 184 s, 2 player(s)"}
  ```

  - `ts` is UTC.
  - `event` is the line's first word: `login`, `logout`, `account`, `conn`, `game`, `join`, `chat`, `stats`,
    `result`, `limit`, `admin`, `policy`, `mods`, ...
  - `level` is `warn` for limits, refusals and denials.
- **Worth watching:**
  - `limit:` lines (floods, password guessing);
  - `abuse:` lines (132on2h: refused login attempts, dropped chat, flood disconnects, blocked addresses; level `warn`);
  - `login refused`;
  - `admin:`;
  - `result:` (the finished games).
- **Rotation** (132on2g). When the log reaches `log_max_bytes` (default 10 MiB) it is renamed to `FILE.1` (the older
  ones move up to `FILE.2` ... `FILE.<log_keep>`, the oldest is deleted) and a new file is started. `log_max_bytes=0`
  turns this off, for a host whose own tools rotate it (logrotate with `copytruncate`, or journald under systemd).
- **Lifecycle lines:** `lifecycle: shutdown in N s (reason)`, `lifecycle: N connection(s) closed (...)`,
  `omega_server stopping`, and after a restart `listening on ... (restart N)`. Store recovery lines start with
  `store FILE`.
- The log file is opened shareable, so it can be read (or tailed) while the server runs.

## 9. Security notes

- **Passwords.** Stored as PBKDF2-HMAC-SHA-256 digests with a random 16-byte salt per account and 120000 iterations by
  default.
  - Raise `pbkdf2_iterations` on a fast host. Older digests are upgraded at each player's next login.
  - Unknown names are hashed against a dummy, so timing does not reveal which names exist.
- **The wire is not encrypted** (§5.1, §6.10). Logins and admin commands cross in clear text. For anything beyond a
  trusted LAN, put the server behind a VPN or an encrypted tunnel, and tell players not to reuse a password from
  elsewhere.
- **Guessing and floods.** These are limited by the login throttle, the per-address connection and registration
  limits and the per-connection frame rate (§6.10). Tune them with the keys in §4.
- **Anti-abuse** (132on2h; ONLINE.md §6.16). Every limit is a key in §4, and `0` turns it off:
  - **Login attempts** per address (`login_attempts_per_ip`, sign-ups included) and per account name from any
    address (`login_attempts_per_account`), per minute. Past them a login is refused before any password work, with a
    notice, so a password-guessing script costs the server almost nothing. This is on top of the failed-login
    lockout (`login_failures`), which only counts wrong passwords.
  - **Lobby chat** per player (`chat_per_minute`, `chat_burst`) and per address (`chat_per_minute_ip`). A line over
    the limit is dropped and the sender told: a notice for a port client, or a private chat line from
    `omega_server` for an original (PS2-kind) client, so a PS2 never receives port-only data. `chat_flood_kick`
    dropped lines within a minute disconnect the player.
  - **Flood disconnect.** A connection over `message_rate` / `message_burst`, or kicked for chat flooding, is a
    flood disconnect. `flood_block_after` of them from one address within `flood_window` block the address for
    `flood_block`: its new connections are closed at once. `limits` lists the blocks, and `limits clear` lifts them.
  - **Logging.** Each refusal, drop, kick and block is an `abuse:` line (`warn` in JSON), and `status` / `limits`
    count them.
  - A host behind which many honest players share one address (a LAN party, a carrier NAT) may need the per-address
    keys raised.
- **Password work off the loop** (132on2h). The PBKDF2 digests of logins and sign-ups are computed on
  `password_threads` worker threads, so a burst of logins no longer stalls the other players (§12). The admin
  `password` and `create` commands, and the one-time digest upgrade after `pbkdf2_iterations` is raised, still hash
  on the loop; they are rare.
- **The relay.** Off unless `relay_port` is set. Its tokens, same-game forwarding and rate limits are in §2.3.
- **Admin token.** Make `admin_token` long and random, or leave it empty and use admin accounts. Wrong tokens are throttled like failed logins, and a refused token is never written to the log.
- **Run unprivileged.** Run it as an unprivileged user that owns only its working directory.

## 10. PS2 clients

A real PS2 (or PCSX2) cannot reach omega_server yet. It needs:
- DNS pointed at this host;
- DNAS handled;
- an original-wire (SCE-RT) listener.

ONLINE.md §8.3 lists the steps. When that layer exists, PS2 sessions will receive only the original message set (§8.1).

## 11. Test scripts and ports (132on2g)

The project's online test scripts (`tools/run_online_loopback.ps1`, `run_online_screens.ps1`,
`run_online_two_instances.ps1`) never use a fixed port. Each starts its own `omega_server` with `--port 0` (and
`--relay-port any` for the relay run) through `tools/online_server_common.ps1`, which reads the ports the server
chose from its log and passes them to the game instances. At the end, and on any failure, the script asks its
server to shut down (`omega_server admin ... --token <per-run token> shutdown now`), waits, stops it if it is still
there, and fails the run if a server is left. The server also has `--parent-pid PID`: it shuts down when that process
ends, so a killed script cannot leave it listening. A real server on 10075 on the same machine is therefore never
disturbed by a test run, and a test never collides with another agent's.

`tools/run_online_load.ps1` (132on2h, §12) follows the same rules: its servers use `--port 0`, `--relay-port any`, a
per-run admin token and `--parent-pid`, and the script checks that they are gone at the end.

## 12. Load test (132on2h)

`omega_server load HOST:PORT --clients N [--group 4] [--chat 3] [--original-every 8] [--relay-seconds S] [--relay-rate R] [--prefix NAME] [--token TOKEN]` is a headless client
simulator (`src/port/app/omega_server_load.cpp`). It starts N clients at once, each on its own thread with the port's
client transport, and runs them through the server's whole path in phases (a barrier between phases, so every
phase is under full load):
1. connect, the hello (every 8th client is original-kind, as a PS2 would be), session begin, the policy and
   announcement pages, sign-up and login;
2. the lobby: the player list, three chat lines each (with the chat filter), the game list;
3. every 4th client creates a game, the other three join it;
4. each port joiner registers with the relay and sends R datagrams per second for S seconds to its host, which
   echoes them (the round trip is timed);
5. leave, logout, disconnect.

It prints the count and p50 / p99 / max latency of every operation, the relay's delivery, the extension frames the
original-kind clients received (must be 0), and the server's `status`. `--abuse` instead runs the anti-abuse
scenario against the default limits: a login hammer, a chat flood and three frame floods followed by a connection
from the then-blocked address.

`tools/run_online_load.ps1 [-Out DIR] [-Clients 64,128] [-RelaySeconds 10] [-RelayRate 30]` starts a server whose
per-address limits are off (all the clients share 127.0.0.1), runs the load for each client count, samples the
server process every 250 ms (CPU, working set, handles, threads), then starts a second server with the default
limits for the abuse scenario and checks its `abuse:` log lines. It ends with `LOAD RESULT ok` or `FAIL`.

**Results** (Windows 11, i9-13900H with 20 logical CPUs, Release build, server and clients on one machine,
`pbkdf2_iterations=120000`, relay 30 datagrams/s for 5 s per joiner; the 128-client run follows the 64-client one on
the same server):

| | before 132on2h | password work on 4 threads | + faster PBKDF2, batched saves | + coalesced roster (now) |
|---|---|---|---|---|
| 64 clients | **failed**: 63 of 64 sign-ups unanswered | ok in 16.8 s | ok in 11.0 s | ok in 11.5 s |
| sign-up p50 / p99 (64) | — | 2371 / 4324 ms | 850 / 1732 ms | 782 / 1690 ms |
| login p50 / p99 (64) | — | 4252 / 4415 ms | 1568 / 1737 ms | 1545 / 1675 ms |
| 128 clients | not reached | ok in 27.3 s | ok in 14.1 s | ok in 14.9 s |
| sign-up p50 / p99 (128) | — | 4551 / 8659 ms | 1335 / 3020 ms | 1602 / 3128 ms |
| login p50 / p99 (128) | — | 9024 / 9538 ms | 3174 / 3451 ms | 3237 / 3339 ms |
| join p50 (128) | — | 800 ms | 95 ms | 59 ms |
| relay round trip p50 / p99 (128) | — | 3.8 / 33.1 ms | 2.6 / 15.1 ms | 2.8 / 16.8 ms |
| loop work per pass p50 / p99 / max | (no answer) | 0.07 / 19.1 / 804 ms | 0.49 / 23.0 / 134 ms | 0.17 / 14.6 / 72 ms |
| server CPU (128): average / peak, of one core | — | 241% / 397% | 159% / 419% | 169% / 393% |
| peak working set | 6.5 MB | 36.1 MB | 34.5 MB | 12.1 MB |

The relay delivered and echoed every datagram (10800 of 10800 at 128 clients), and the original-kind clients
received no extension frame. The hot spots found and fixed:
1. **PBKDF2 on the server loop.** Every sign-up and login hashed its password (120000 iterations) on the one server
   thread, so 64 simultaneous sign-ups held the loop for seconds and the clients timed out. The password work now
   runs on `password_threads` workers; the answer is sent when it is done, and a second login or sign-up of the same
   connection meanwhile is refused.
2. **The PBKDF2 itself.** Each iteration re-hashed the HMAC key pads and padded byte by byte. The pads are now hashed
   once, and each iteration is two single-block compressions (the same digests; the server's CPU per digest in the
   load fell from about 0.25 s to 0.1 s).
3. **A store write per change.** Each sign-up rewrote the accounts file and each join the contacts file (an atomic
   write with a flush to disk each time). These are now written at most once a second, and at shutdown.
4. **The roster.** Each login, join and logout sent the whole player roster to every port client (n² frames of n
   entries). It is now sent once per loop pass, which also cut the memory peak to a third.

What remains is by design: sign-up and login latency under a burst is the password work itself (at 120000
iterations about 0.1 s of CPU per digest, four at a time here). A real burst of that size is unlikely; raise
`password_threads` on a bigger host. The remaining loop p99 of about 15 ms is the once-a-second store write
(an fsync).
