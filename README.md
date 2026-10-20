# LunaViewer

Experimental Linux relay controller implemented in simple C++17 with Qt6 Widgets. The source-defined relay path, modern screen decoder and gated mouse/keyboard input are implemented. Offline tests pass. An authorized live connect-only check completed registration, lookup, relay TLS and peer initialization, then disconnected without input. The user subsequently confirmed live screen rendering and updates. Live input/control interoperability and native X11/Wayland backend coverage remain unvalidated.

## Build and offline checks

Dependencies: a C++17 compiler, CMake 3.20+, pkg-config, Qt6 Core/Network/Widgets, OpenSSL, libzstd and libjpeg-turbo (TurboJPEG API). Use distribution development packages. No dependencies are downloaded by the build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Tests use synthetic protocol/screen/session fixtures, an authenticated local TLS server and Qt offscreen rendering. They do not contact UltraViewer services. To exercise the local dictionary separately:

```sh
./build/screen_tests /absolute/path/to/existing/8bit.dat
```

## Run

Create a private local profile from the example using the commands below. First identify your active interface and its IPv4 address:

```sh
ip -4 -brief address show scope global
```

Set the two values from that output, for example `enp3s0` and `192.168.1.20` (use your actual values):

```sh
export LUNAVIEWER_INTERFACE='enp3s0'
export LUNAVIEWER_LOCAL_IP='192.168.1.20'
python3 - <<'PROFILE'
import base64, getpass, hashlib, ipaddress, json, os, pathlib, secrets, socket, uuid
profile = json.loads(pathlib.Path('identity-profile.example.json').read_text())
f = profile['registration_fields']
f[1] = hashlib.md5(uuid.uuid4().bytes).hexdigest().upper()
f[2] = ''.join(secrets.choice('0123456789') for _ in range(8))
f[4] = str(ipaddress.IPv4Address(os.environ['LUNAVIEWER_LOCAL_IP']))
f[6] = base64.b64encode(socket.gethostname().encode('utf-8')).decode('ascii')
interface = os.environ['LUNAVIEWER_INTERFACE']
if '/' in interface or interface in ('.', '..'):
    raise ValueError('Use an interface name, not a path')
mac = pathlib.Path('/sys/class/net', interface, 'address').read_text().strip()
f[8] = mac.replace(':', '').upper()
user = getpass.getuser()
f[16] = user if user.isascii() and ':' not in user else base64.b64encode(user.encode()).decode()
fd = os.open('identity-profile.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as out:
    json.dump(profile, out, indent=2)
    out.write('\n')
print('Created private identity-profile.json; keep it for future sessions.')
PROFILE
```

Run this once from the application directory. It refuses to overwrite an existing identity and does not contact any server. Device identity is a stable per-installation 32-character uppercase hexadecimal value; the local registration password is eight random ASCII digits, matching the reference profile conventions. This local password is separate from the Windows partner password entered in the UI. Hostname, interface MAC and username come from this Linux machine. No Windows hardware identity is invented.

Only fields 1,2,4,6,8,16 need local values; the command fills them. Other example slots are the explicit relay-controller defaults or empty/zero for unavailable metadata: disk identity, OS-version mapping, folder ticks, optional license/CAPTCHA/referral data. Their acceptance by the current service remains unverified. All 28 fields are documented in the [research field table](../lunaviewer-docs/docs/protocol.md#command-3-local-registration).

The wire compatibility value `6.6.133` follows `MacRegistrationProfile.swift:245` in the reference source; LunaViewer's actual software version is `0.1.0`. Keep the profile private and retain it across runs; credentials do not belong in progress messages.

Supply an existing local UltraViewer dictionary (112640 bytes, SHA-256 `9aa45789d9ff9d329a623a685d57b67e33c777019de9ce1f273f21860e4bfe6b`). The dictionary is not distributed in this repository and is excluded from MacViewer's MIT license.

```sh
./build/lunaviewer --profile identity-profile.json --dictionary /home/thkinh/Projects/for-linux/MacViewer/Sources/UltraViewerProtocol/Resources/8bit.dat
```

The dictionary path above is the existing local reference asset in this workspace; use your own path on another machine.

Enter an authorized Windows peer's displayed ID/password in the application. Connect first with control disabled. After accepted initialization, enable the local **Enable remote control** checkbox when control is available. The source-defined default permits control eligibility if optional host command49 was omitted; any explicit command49 denial before or after initialization overrides that default and releases held input. A successful lookup is followed by peer initialization, decode and ACK flow control. TLS verifies the trust chain and configured hostname; connect address, SNI, certificate hostname and HTTP Host remain separate. Optional `ca_file` in the profile adds an explicitly selected trust file. No verification bypass is provided. Some relays omit the Sectigo R36 intermediate; the client supplies that public chain certificate while still requiring a system/custom trusted root, valid dates and hostname. Partial-chain trust is disabled. TLS failures report command-versus-peer stage and nonsecret endpoint/verification details to the UI and stderr.

The controller uses Qt's native X11 or Wayland window backend. It does not capture the Linux desktop or inject global Linux input. Hosting, portals/PipeWire/XTEST, LAN upgrade, clipboard/files/chat/audio, dynamic geometry changes and older protocol variants are deferred.

## Architecture and current limits

`src/protocol` handles bounded bytes, framing, envelopes and OpenSSL crypto. `src/transport` integrates nonblocking OpenSSL sockets with Qt readiness/timers. `src/screen` owns the ordered Zstd/RGB555/temp/scroll/cache/JPEG pipeline. `app/session` orchestrates registration/lookup/login, initialization, ACKs, heartbeats and input permissions independently of Qt. `src/ui` owns presentation and local event mapping. `app/main.cpp` runs the session on one serialized worker and coalesces only completed presentation snapshots. UI input is bounded; every screen delta is decoded in order.

Decode failure or unsupported geometry changes close the current session with a status; automatic screen-refresh recovery is not implemented. Disconnect releases tracked input and allows a bounded transport drain; network failure cannot guarantee delivery. Initial keyboard support uses US-layout virtual-key/scancode mapping; international layouts/IME, initial Caps Lock state and native desktop shortcuts still need validation. Binary keyboard requires peer version 6.2.13 or newer; viewing supports the source modern path from 6.0.0. Frame sequence-reset semantics at display reconfiguration remain deferred.

An authorized Windows peer and valid explicit identity profile are required for further compatibility checks. Credentials are not included in source or progress files. The manual `build/connection_check` helper reads one JSON credential line from private stdin, never enables control, and exits after peer initialization or failure.

## Progress and attribution

The [agent progress index](agent-progress/README.md) links each worker's scope, status and append-only user/agent messages. [CONTRACTS.md](CONTRACTS.md) documents ownership and interfaces. Source behavior is derived from the MacViewer snapshot documented in the sibling research; [MACVIEWER-LICENSE.txt](MACVIEWER-LICENSE.txt) preserves its MIT notice. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Permission diagnostics identify `connection-start` (unavailable), `accepted-init-default` (source-defined capability), `host-command49-byte-*` (explicit remote state), and `local-toggle`. Zero in command49 allows control; any nonzero value denies it. Before initialization/disconnected, the UI says unavailable rather than claiming a host denial. A fresh session resets explicit permission history and leaves local control off.

## Session logs

The app writes bounded metadata logs to `${XDG_STATE_HOME:-~/.local/state}/lunaviewer/logs/session.log` (on this machine: `/home/thkinh/.local/state/lunaviewer/logs/session.log`). Startup prints the resolved path to stderr and shows it in the status bar. Use `--log-dir PATH` to select another directory and `--debug` to include DEBUG records. The active file is limited to 1 MiB by default, with three rotated backups; new log directories use mode0700 and files use0600. If the log cannot be opened, the app continues and reports the problem in the status bar.

Logs include connection stages, peer initialization version/parser/mode, permission transitions, TLS and protocol errors, and aggregate input gate counts. They do not record credentials, profile contents, screen payloads, typed text or key codes. To inspect the latest session:

```sh
tail -n 100 "${XDG_STATE_HOME:-$HOME/.local/state}/lunaviewer/logs/session.log"
rg 'peer_init|keyboard_gate|input_summary|permission|tls' "${XDG_STATE_HOME:-$HOME/.local/state}/lunaviewer/logs/session.log"
```
