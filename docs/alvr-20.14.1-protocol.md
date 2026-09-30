# ALVR v20.14.1 wire protocol (client/headset side)

This document specifies the network protocol spoken by the ALVR **streamer** v20.14.1 (Windows,
SteamVR driver) so that a C/C++ client can interoperate with it without reading the Rust sources.

All paths are relative to the repository root `reference/alvr-20.14.1/`.

Conventions used below:

* "client" = headset side (your PS4 app). "server"/"streamer" = PC side.
* `LE` = little endian, `BE` = big endian.
* Hex dumps are byte sequences in wire order.
* "bincode" always means **bincode 1.3.3 with the legacy default configuration** (section 1).
* Where the source is ambiguous or where I could not verify something by running the Rust code,
  this is marked **AMBIGUOUS / UNVERIFIED**.

---

## 0. Overview

| Channel | Transport | Port (default) | Who listens | Who connects | Content |
|---|---|---|---|---|---|
| Discovery announce | UDP broadcast | 9943 → 255.255.255.255:9943 | server binds UDP 9943 | client sends | 56-byte fixed packet |
| Control socket | TCP | 9943 | **client** listens | **server** connects to client | u32-BE length-framed bincode messages |
| Stream socket | UDP (default) or TCP | 9944 (`connection.stream_port`) | UDP: both bind 9944 and `connect()`; TCP: **client** listens | server | sharded packets, 5 logical streams |

Key constants (`alvr/sockets/src/lib.rs`):

```rust
pub const CONTROL_PORT: u16 = 9943;
pub const HANDSHAKE_PACKET_SIZE_BYTES: usize = 56;
pub const KEEPALIVE_INTERVAL: Duration = Duration::from_millis(500);
pub const KEEPALIVE_TIMEOUT: Duration = Duration::from_secs(2);
pub const MDNS_SERVICE_TYPE: &str = "_alvr._tcp.local.";
pub const MDNS_PROTOCOL_KEY: &str = "protocol";
pub const MDNS_DEVICE_ID_KEY: &str = "device_id";
pub const WIRED_CLIENT_HOSTNAME: &str = "client.wired";
```

Stream IDs (`alvr/packets/src/lib.rs`):

```rust
pub const TRACKING: u16 = 0;   // client -> server, header = Tracking
pub const HAPTICS: u16 = 1;    // server -> client, header = Haptics
pub const AUDIO: u16 = 2;      // both directions (game audio s->c, microphone c->s), header = ()
pub const VIDEO: u16 = 3;      // server -> client, header = VideoPacketHeader, payload = NALs
pub const STATISTICS: u16 = 4; // client -> server, header = ClientStatistics
```

Handshake order (details in section 3):

```
client                                              server
  |  UDP broadcast "ALVR..." every ~0.5-2 s  ------->  |  (discovery, trust check)
  |  <------------- TCP connect to client:9943 ------  |
  |  ClientConnectionResult::ConnectionAccepted ---->  |
  |  <-------------------------- StreamConfigPacket    |
  |  <----------- ServerControlPacket::Restarting      |  (only if SteamVR must restart -> client disconnects)
  |  <----------- ServerControlPacket::StartStream     |
  |  (client binds stream socket on 9944)              |
  |  ClientControlPacket::StreamReady -------------->  |
  |  <======== stream socket established (UDP/TCP) ==> |
  |  KeepAlive every 500 ms (both ways), tracking, video, ...
```

---

## 1. Serialization format

### 1.1 bincode version and configuration

* `Cargo.lock`: `name = "bincode"`, `version = "1.3.3"`. `alvr/sockets/Cargo.toml` and
  `alvr/packets/Cargo.toml` depend on `bincode = "1"`.
* The only calls in the protocol path are the **top-level bincode 1.x functions**
  (`bincode::serialize`, `bincode::serialize_into`, `bincode::serialized_size`,
  `bincode::deserialize`, `bincode::deserialize_from`) in
  `alvr/sockets/src/control_socket.rs`, `alvr/sockets/src/stream_socket.rs` and
  `alvr/packets/src/lib.rs` (`RealTimeConfig::encode/decode`). There is no `bincode::options()`,
  `DefaultOptions`, `with_varint_encoding` etc. anywhere in the repo.
* The bincode 1.x top-level functions use the legacy config: **little endian, fixed-size integers
  ("fixint"), no size limit, trailing bytes allowed**. (Not the varint encoding of
  `DefaultOptions`, and not bincode 2.)
* `serde` is 1.0.218, `glam` 0.30.0 (with `serde` feature), `semver` 1.0.25.

### 1.2 Encoding rules (bincode 1, legacy config)

| Rust type | Wire encoding |
|---|---|
| `bool` | 1 byte, `0x00` or `0x01` (anything else is a decode error) |
| `u8/i8` | 1 byte |
| `u16/u32/u64` (and `i*`) | 2/4/8 bytes LE |
| `usize` | 8 bytes LE (u64) |
| `f32` / `f64` | IEEE-754 4/8 bytes LE |
| `String` / `&str` | `u64` LE byte length + UTF-8 bytes (no terminator) |
| `Vec<T>`, `HashSet<T>`, maps | `u64` LE element count + elements |
| `Vec<u8>` | `u64` LE length + raw bytes |
| `Option<T>` | `0x00` = None; `0x01` followed by `T` = Some |
| `[T; N]` (fixed array) | N elements back to back, **no length prefix** |
| tuple `(A, B)` | `A` then `B`, no prefix |
| struct | fields in declaration order, no prefix, no padding |
| `()` | 0 bytes |
| enum | `u32` LE **variant index in declaration order** (0-based), then the variant's data (newtype: inner value; tuple/struct variant: fields in order; unit: nothing). Explicit Rust discriminants (`= 3`) are **ignored** by serde. |
| `std::time::Duration` | struct `{ secs: u64, nanos: u32 }` = 12 bytes (8 LE + 4 LE). Always send normalized (`nanos < 1_000_000_000`). |
| `std::net::IpAddr` | enum: `u32` 0 = V4 followed by 4 octets (network order, e.g. 192.168.1.10 = `c0 a8 01 0a`); `u32` 1 = V6 followed by 16 octets |
| `glam::Vec2` / `Vec3` | 2 / 3 × f32 LE: x, y(, z) |
| `glam::UVec2` | 2 × u32 LE: x, y |
| `glam::Quat` | 4 × f32 LE in order **x, y, z, w** |

In JSON (serde_json, used for some string fields), glam types are arrays (`UVec2` → `[960,1080]`,
`Quat` → `[x,y,z,w]`), enums are externally tagged (`"Variant"` or `{"Variant": {...}}`), `Option`
is `null` or the value.

### 1.3 Control-socket framing (TCP)

`alvr/sockets/src/control_socket.rs`, `framed_send` / `framed_recv`:

```
+------------------------+---------------------------+
| u32 BE payload_length  | bincode(payload) (N bytes) |
+------------------------+---------------------------+
```

* The prefix is **big endian** and counts only the payload (not itself).
* One frame = one message. The receiver peeks 4 bytes, then reads exactly `4 + N` bytes.
* `TCP_NODELAY` is set on both ends.

### 1.4 Stream-socket framing (UDP datagram or TCP byte stream)

`alvr/sockets/src/stream_socket.rs`, `StreamSender::send`, `StreamSocket::recv`. Each logical
packet (= `bincode(header) ++ raw payload`) is split into **shards**; each shard is one UDP
datagram (or appended to the TCP byte stream):

```
offset size  field                  endianness
0      4     shard_len              BE   = total shard bytes - 4  (i.e. 14 + chunk_len)
4      2     stream_id              BE   (0..4, see table above)
6      4     packet_index           BE   per-(stream, direction) counter, starts at 0, +1 per packet, wraps
10     4     shards_count           BE   number of shards of this packet
14     4     shard_index            BE   0 .. shards_count-1
18     ...   chunk                        bytes [shard_index*D, shard_index*D + chunk_len) of the packet data
```

(`SHARD_PREFIX_SIZE` = 18.) Note the prefix is **big endian** (comment in source: `todo: switch
to little endian`), while everything inside the chunk is bincode (little endian).

Shard sizes:

* `packet_size` = `session_settings.connection.packet_size` (default **1400**, i32).
* `max_packet_size = packet_size + 4` (see `StreamSocketBuilder::accept_from_server/connect_to_client`:
  "`+4 is a workaround to retain compatibilty with old protocol`") → default **1404**.
* Max chunk per shard `D = max_packet_size - 18` → default **1386**.
* `shards_count = ceil(data_len / D)`. Every shard except the last carries exactly `D` bytes.
* Max datagram size on the wire = `max_packet_size` = 1404 bytes by default.

The packet data is `bincode(header)` immediately followed by the raw payload (no length for the
payload; payload = everything after the header up to the reassembled length).

Reassembly (receiver side, what the Rust code does):

* Chunk `i` is written at byte offset `i * D` of the packet buffer, where `D` is computed from the
  **receiver's own** `packet_size`. Both sides use the same `packet_size` from the session, so you
  **must** use `packet_size` from the session JSON both when sending and receiving.
* A packet is complete when all `shards_count` distinct `shard_index` values have arrived; its
  length is `(shards_count-1)*D + last_chunk_len`.
* When a packet of stream S with index `k` completes, all incomplete packets of stream S with index
  older than `k` (wrapping comparison) are discarded.
* Delivery to the consumer compares with the last delivered index (wrapping u32 comparison,
  `wrapping_cmp`): `== last+1` → normal; `> last+1` → deliver and set `had_packet_loss = true`;
  `< last+1` → old packet, silently dropped. There is no retransmission, no ACK, no FEC.
* Each stream (and each direction) has its own `packet_index` counter starting at 0 at
  connection time.

Sender side: the client must shard its uplink packets (tracking, statistics, microphone) with the
same `D`, its own per-stream counter starting at 0, and send each shard as one datagram (UDP) or
back-to-back bytes (TCP).

A packet with zero data bytes is never sent (`shards_count` would be 0). All headers used on the
stream socket are non-empty except audio `()`, whose payload is non-empty.

### 1.5 Worked hex examples

Control socket, `ClientControlPacket::KeepAlive` (variant 2, unit):

```
00 00 00 04  02 00 00 00
```

`ClientControlPacket::StreamReady` (variant 3): `00 00 00 04 03 00 00 00`
`ClientControlPacket::RequestIdr` (variant 1): `00 00 00 04 01 00 00 00`
`ServerControlPacket::StartStream` (variant 0): `00 00 00 04 00 00 00 00`
`ServerControlPacket::Restarting` (variant 2): `00 00 00 04 02 00 00 00`
`ServerControlPacket::KeepAlive` (variant 3): `00 00 00 04 03 00 00 00`

`ClientControlPacket::Buttons(vec![ButtonEntry { path_id: hash("/user/hand/left/input/trigger/value"), value: ButtonValue::Scalar(0.5) }])`:

```
00 00 00 1c                 frame length 28
07 00 00 00                 variant 7 = Buttons
01 00 00 00 00 00 00 00     Vec length 1
9f c4 2d dc e3 bb 66 0b     path_id = 0x0b66bbe3dc2dc49f
01 00 00 00                 ButtonValue variant 1 = Scalar
00 00 00 3f                 0.5f
```

`Buttons` with `/user/hand/right/input/trackpad/click` = `Binary(true)`:

```
00 00 00 19  07 00 00 00  01 00 00 00 00 00 00 00
46 62 bd ff 3f ba ea 3b   00 00 00 00   01
```

`ActiveInteractionProfile { device_id: hash("/user/hand/left"), profile_id: hash("/interaction_profiles/htc/vive_controller") }`:

```
00 00 00 14  08 00 00 00
a2 07 2d be da d8 21 e5     device_id  0xe521d8dabe2d07a2
ea 1c 3d 6d 42 d9 2b 49     profile_id 0x492bd9426d3d1cea
```

`PlayspaceSync(Some(Vec2(2.0, 2.0)))` (the PS4 client sends 1.9 x 2.4, see
`alvr_client.cpp`):

```
00 00 00 0d  00 00 00 00  01  00 00 00 40  00 00 00 40
```

Typical `ServerControlPacket::ReservedBuffer(bincode(RealTimeConfig{None, None}))` sent every second:

```
00 00 00 0e  06 00 00 00  02 00 00 00 00 00 00 00  00 00
```

Stream socket, a `Tracking` packet with only the head (87 bytes of bincode, see section 6),
stream 0, packet index 5, single shard (UDP datagram of 105 bytes):

```
00 00 00 65                 shard_len = 101 = 14 + 87
00 00                       stream_id 0 (TRACKING)
00 00 00 05                 packet_index 5
00 00 00 01                 shards_count 1
00 00 00 00                 shard_index 0
0c 00 00 00 00 00 00 00     target_timestamp.secs  = 12
35 a4 9a 14                 target_timestamp.nanos = 345678901
01 00 00 00 00 00 00 00     device_motions len 1
38 25 20 c9 3d 85 90 5b     device id = hash("/user/head")
00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 3f   orientation x,y,z,w = 0,0,0,1
00 00 00 00 cd cc cc 3f 00 00 00 00               position 0, 1.6, 0
00 00 00 00 00 00 00 00 00 00 00 00               linear_velocity
00 00 00 00 00 00 00 00 00 00 00 00               angular_velocity
00 00                       hand_skeletons = [None, None]
00 00                       face_data.eye_gazes = [None, None]
00 00 00                    fb_face_expression, htc_eye_expression, htc_lip_expression = None
```

A 3013-byte video packet (13-byte `VideoPacketHeader` + 3000 NAL bytes), packet_size 1400,
stream 3, index 0 → 3 datagrams of 1404, 1404 and 259 bytes; prefixes:

```
shard 0: 00 00 05 78  00 03  00 00 00 00  00 00 00 03  00 00 00 00   (+1386 bytes)
shard 1: 00 00 05 78  00 03  00 00 00 00  00 00 00 03  00 00 00 01   (+1386 bytes)
shard 2: 00 00 00 ff  00 03  00 00 00 00  00 00 00 03  00 00 00 02   (+241 bytes)
```

---

## 2. Discovery / pairing

Sources: `alvr/client_core/src/sockets.rs` (`AnnouncerSocket`), `alvr/client_core/src/connection.rs`
(`connection_pipeline`), `alvr/server_core/src/sockets.rs` (`WelcomeSocket::recv_all`),
`alvr/server_core/src/connection.rs` (`handshake_loop`, `try_connect`), `alvr/common/src/version.rs`.

### 2.1 Protocol id

```rust
// alvr/common/src/version.rs
pub static ALVR_VERSION: Lazy<Version> = Lazy::new(|| Version::parse(env!("CARGO_PKG_VERSION")).unwrap());
pub fn hash_string(string: &str) -> u64 {
    let mut hasher = DefaultHasher::new();
    string.hash(&mut hasher);
    hasher.finish()
}
pub fn protocol_id() -> String {
    if ALVR_VERSION.pre.is_empty() { ALVR_VERSION.major.to_string() }
    else { format!("{}-{}", ALVR_VERSION.major, ALVR_VERSION.pre) }
}
pub fn protocol_id_u64() -> u64 { hash_string(&protocol_id()) }
```

Workspace version is `20.14.1` (root `Cargo.toml`), no pre-release, so:

* `protocol_id()` = `"20"` (string; used in mDNS TXT record and client config file).
* `protocol_id_u64()` = `hash_string("20")` = **`0x17667eafcf6d5d67`** = 1686174404061257063,
  LE bytes `67 5d 6d cf af 7e 66 17`.

Any 20.x.y streamer accepts this id. See section 6.4 for the hash algorithm.

### 2.2 Announce packet (client → broadcast)

The client binds a UDP socket to `0.0.0.0:9943`, enables `SO_BROADCAST`, and sends a 56-byte
datagram to **`255.255.255.255:9943`**:

```
offset size content
0      4    "ALVR" (41 4c 56 52)
4      12   zero bytes (server requires bytes 4..16 all 0)
16     8    protocol_id_u64() as u64 LE
24     32   hostname, UTF-8, NUL padded (server trims trailing NULs)
```

Server acceptance (`WelcomeSocket::recv_all`): datagram length must be exactly 56, bytes 0..4 =
"ALVR", bytes 4..16 zero, bytes 24..56 valid UTF-8. A protocol-id mismatch only logs a warning at
discovery time (the connection is later refused at handshake, section 3.2). The client IP is taken
from the datagram source address; the source port is not used.

Exact announce packet for hostname `1234.client`:

```
0000: 41 4c 56 52 00 00 00 00 00 00 00 00 00 00 00 00
0010: 67 5d 6d cf af 7e 66 17 31 32 33 34 2e 63 6c 69
0020: 65 6e 74 00 00 00 00 00 00 00 00 00 00 00 00 00
0030: 00 00 00 00 00 00 00 00
```

Frequency: the official client loops "announce, then try to accept a TCP connection"; the listener
has a 2 s accept timeout (`HANDSHAKE_ACTION_TIMEOUT`) and `DISCOVERY_RETRY_PAUSE` is 500 ms, so in
practice it announces every ~0.5-2 s. The server polls its UDP socket non-blockingly and sleeps
~1 s (`RETRY_CONNECT_MIN_INTERVAL`) between rounds. Recommendation: announce every 1 s while not
connected.

### 2.3 Hostname rules

* Default (`alvr/client_core/src/storage.rs`, `Config::default`): `format!("{}{}{}{}.client", d, d, d, d)`
  with 4 random digits, e.g. `1234.client`. It is persisted and reused (the streamer trusts clients
  by hostname).
* Max 32 bytes (the official client would panic above 32). Must be valid UTF-8, must not contain
  NUL. No other validation exists on the server or dashboard.
* The hostname is the key of the server's client list (`session.client_connections`).
* `client.wired` is reserved for the ADB/USB wired client. A loopback client IP forces `wired = true`
  and TCP streaming.

### 2.4 Trust / how the server decides to connect

`handshake_loop` (`alvr/server_core/src/connection.rs`), each round:

1. Wired (ADB) clients (irrelevant here).
2. **Manual IPs**: for every client-list entry in state `Disconnected` (except `client.wired`) with
   `manual_ips`, the server tries a TCP connect to `ip:9943` (1 s total timeout split across IPs).
   No announce is needed for this path. The dashboard "Add client manually" creates the entry with
   `trusted: true` (`alvr/dashboard/src/dashboard/components/devices.rs`).
3. **Discovery** (only if `connection.client_discovery` is enabled, default enabled): for each
   announced hostname: `AddIfMissing { trusted: false }`; if `auto_trust_clients` (default `false`
   in release builds: `cfg!(debug_assertions)`) → mark trusted. If trusted and `Disconnected`, it
   calls `try_connect` (TCP connect to `client_ip:9943`, 1 s timeout).
4. Untrusted entries appear in the dashboard; the user must click **Trust**. Untrusted entries are
   deleted when the streamer restarts (`ServerSessionManager::clean_client_list`,
   `alvr/server_io/src/lib.rs`), so keep announcing.

### 2.5 mDNS (optional alternative)

The server also browses mDNS service type `_alvr._tcp.local.`. For a resolved service it uses TXT
key `device_id` as hostname (falls back to the mDNS host name) and requires TXT key `protocol`
(compared as a string with `protocol_id()`, i.e. `"20"`); the first resolved address is the client
IP. The service port is not used (the server always connects to 9943). client_core only exposes
the service name (`alvr_mdns_service` in `alvr/client_core/src/c_api.rs`); registration is done by
platform code. Not required if you broadcast.

---

## 3. Control socket and handshake

### 3.1 Transport and direction

* TCP. The **client listens** on `0.0.0.0:9943` (`alvr_sockets::get_server_listener`), the
  **server connects** to `client_ip:9943` (`tcp::connect_to_client`). The client learns the server
  IP from the accepted connection's peer address.
* Both ends set `TCP_NODELAY`. Framing per section 1.3.
* Before the "split", both sides exchange untyped messages (`ProtoControlSocket::send/recv`); after
  it, client→server messages are always `ClientControlPacket` and server→client messages are
  always `ServerControlPacket`.

### 3.2 Handshake sequence (exact order)

| # | Dir | Message | Timeout on the waiting side |
|---|---|---|---|
| 0 | S→C | TCP connect to client:9943 | server connect timeout 1 s |
| 1 | C→S | `ClientConnectionResult::ConnectionAccepted {...}` (send immediately after accept) | server waits 2 s (`HANDSHAKE_ACTION_TIMEOUT`) |
| 2 | S→C | `StreamConfigPacket { session, negotiated }` (not wrapped in an enum) | client waits 2 s |
| 3a | S→C | *optional* `ServerControlPacket::Restarting` | — |
| 3b | S→C | `ServerControlPacket::StartStream` | client waits 2 s |
| 4 | C | bind the stream socket (UDP bind / TCP listen on `stream_port`) **before** step 5 | — |
| 5 | C→S | `ClientControlPacket::StreamReady` | server waits 2 s; any other packet → handshake error |
| 6 | S→C | UDP: server binds `0.0.0.0:stream_port` and `connect()`s to `client_ip:stream_port`. TCP: server connects to `client_ip:stream_port`, client accepts (2 s). | — |
| 7 | both | streaming: KeepAlives, tracking, video, audio, ... | 2 s keepalive timeout |

Details and server-side checks (`alvr/server_core/src/connection.rs`, `connection_pipeline`):

* Step 1: if `client_protocol_id != protocol_id_u64()` the server logs "Trusted client is
  incompatible!" and closes. `ClientStandby` → server closes and retries later.
  `streaming_capabilities` must be `Some` ("Only streaming clients are supported for now").
* Step 3a: after sending the `StreamConfigPacket` the server computes a new `OpenvrConfig`
  (resolution, refresh rate, codec, controller emulation, ...). If it differs from the one SteamVR
  was started with, it sends `Restarting`, asks the dashboard to restart SteamVR, and still sends
  `StartStream`. The official client treats `Restarting` (received where `StartStream` is
  expected) as "disconnect and retry": close everything, go back to discovery. **The first
  connection with a new headset/resolution normally ends this way**; the second attempt (after
  SteamVR restarted) proceeds.
* Any other packet or a timeout at step 3 → client disconnects.
* Both sides use `STREAMING_RECV_TIMEOUT` = 500 ms socket read timeouts after the split.

### 3.3 `ClientConnectionResult` (step 1)

```rust
// alvr/packets/src/lib.rs
#[derive(Serialize, Deserialize)]
pub enum ClientConnectionResult {
    ConnectionAccepted {                                   // variant 0
        client_protocol_id: u64,
        display_name: String,
        server_ip: IpAddr,
        streaming_capabilities: Option<VideoStreamingCapabilitiesLegacy>, // todo: use String
    },
    ClientStandby,                                         // variant 1
}

#[derive(Serialize, Deserialize, Clone)]
pub struct VideoStreamingCapabilitiesLegacy {
    pub default_view_resolution: UVec2,
    pub supported_refresh_rates_plus_extra_data: Vec<f32>,
    pub microphone_sample_rate: u32,
}
```

Byte layout of the payload:

```
u32  0                                  variant ConnectionAccepted
u64  client_protocol_id                 0x17667eafcf6d5d67
u64  len(display_name) + bytes          free text shown in dashboard (official: platform name)
u32  0 (IpAddr::V4) + 4 octets          the server IP as seen by the client (server ignores it)
u8   1                                  Some
u32  default_view_resolution.x          per-eye width
u32  default_view_resolution.y          per-eye height
u64  count, then count x f32            supported_refresh_rates_plus_extra_data
u32  microphone_sample_rate             Hz
```

`supported_refresh_rates_plus_extra_data` is an extensibility hack
(`encode_video_streaming_capabilities` / `decode_video_streaming_capabilities`):

1. First the real refresh rates as positive f32 (e.g. `90.0, 120.0`).
2. Then, for every byte `b` of the UTF-8 JSON serialization of `VideoStreamingCapabilities`, the
   value `-(b as f32)` (always negative).

The server takes positive entries as refresh rates and converts negative entries back to bytes
(`(-rate) as u8`), then parses them as JSON. Missing JSON keys take defaults that are **dangerous
for a PS4 client** (`supports_foveated_encoding` → true, `encoder_av1` → true,
`encoder_10_bits` → true, `encoder_high_profile` → true, `prefer_full_range` → true,
`preferred_encoding_gamma` → 1.0, others false). If the JSON is invalid, all defaults apply. Always
send every key explicitly. Do not send a refresh rate of 0.

```rust
// Note: not a network packet (only transported as the JSON above)
pub struct VideoStreamingCapabilities {
    pub default_view_resolution: UVec2,
    pub supported_refresh_rates: Vec<f32>,
    pub microphone_sample_rate: u32,
    pub supports_foveated_encoding: bool,
    pub encoder_high_profile: bool,
    pub encoder_10_bits: bool,
    pub encoder_av1: bool,
    pub multimodal_protocol: bool,
    pub prefer_10bit: bool,
    pub prefer_full_range: bool,
    pub preferred_encoding_gamma: f32,
    pub prefer_hdr: bool,
}
```

Recommended JSON for the PSVR (339 bytes, key order irrelevant for the server):

```json
{"default_view_resolution":[960,1080],"supported_refresh_rates":[90.0,120.0],"microphone_sample_rate":48000,"supports_foveated_encoding":false,"encoder_high_profile":true,"encoder_10_bits":false,"encoder_av1":false,"multimodal_protocol":false,"prefer_10bit":false,"prefer_full_range":true,"preferred_encoding_gamma":1.0,"prefer_hdr":false}
```

Resulting frame (display_name `PlayStation 4`, server IP 192.168.1.10): 1430 bytes total, frame
length `00 00 05 92` (1426); beginning:

```
0000: 00 00 05 92 00 00 00 00 67 5d 6d cf af 7e 66 17
0010: 0d 00 00 00 00 00 00 00 50 6c 61 79 53 74 61 74   len 13 "PlaySta
0020: 69 6f 6e 20 34 00 00 00 00 c0 a8 01 0a 01 c0 03   tion 4" V4 192.168.1.10 Some 960
0030: 00 00 38 04 00 00 55 01 00 00 00 00 00 00 00 00   1080, count 341
0040: b4 42 00 00 f0 42 00 00 f6 c2 00 00 08 c2 00 00   90.0 120.0 -123('{') -34('"')
0050: c8 c2 00 00 ca c2 ...                             -100('d') -101('e') ...
...   ... 00 00 fa c2 80 bb 00 00                       -125('}')  mic 48000
```

(`multimodal_protocol`: the official client always sends `true`. It only changes behaviour when
hand skeletons are sent; a PSVR client can send either. `encoder_high_profile`: set to what your
H.264 decoder supports.)

### 3.4 `StreamConfigPacket` (step 2)

```rust
#[derive(Serialize, Deserialize)]
pub struct StreamConfigPacket {
    pub session: String,    // JSON session that allows for extrapolation
    pub negotiated: String, // Encoded NegotiatedVideoStreamingConfig
}
```

Binary layout: `u64 len + session JSON bytes`, `u64 len + negotiated JSON bytes`. No enum tag.
Both are **JSON strings** (serde_json). The session JSON is large (tens of KB).

`negotiated` JSON = `NegotiatedStreamingConfig`:

```rust
pub struct NegotiatedStreamingConfig {
    pub view_resolution: UVec2,          // per-eye size of the video stream, e.g. [960,1056]
    pub refresh_rate_hint: f32,          // chosen fps
    pub game_audio_sample_rate: u32,     // Hz, 0 if game audio disabled
    pub enable_foveated_encoding: bool,
    pub use_multimodal_protocol: bool,   // echo of client's multimodal_protocol
    pub use_full_range: bool,
    pub encoding_gamma: f32,
    pub enable_hdr: bool,
    pub wired: bool,                     // true if client IP is loopback -> TCP stream forced
}
```

Example: `{"view_resolution":[960,1056],"refresh_rate_hint":90.0,"game_audio_sample_rate":48000,"enable_foveated_encoding":false,"use_multimodal_protocol":false,"use_full_range":true,"encoding_gamma":1.0,"enable_hdr":false,"wired":false}`.
Client-side defaults if keys are missing (`decode_stream_config`): foveation = session setting,
multimodal false, full range = session setting, gamma 1.0, hdr false, wired false.

How the server computes it (`connection_pipeline`):

* `view_resolution` = `align32(get_view_res(video.transcoding_view_resolution, default_view_resolution))`
  where `align32(v) = floor(v/32)*32` for **each** axis. `FrameSize::Scale(s)` → `default*s`;
  `FrameSize::Absolute{width, height: None}` → `(width, width*default.y/default.x)`.
  **Default setting is `Absolute { width: 2144, height: None }`**, which for a 960x1080 client gives
  2144 x 2400 per eye (4288x2400 video). For a PSVR set the streamer's "Transcoding view
  resolution" to e.g. `Scale 1.0` (→ 960 x 1056, because 1080 is floored to a multiple of 32) or
  `Absolute width 960, height 1088` (→ 960x1088). Same for "Emulated headset view resolution"
  (SteamVR render target, `target_eye_resolution_*`, not sent to the client).
* `refresh_rate_hint` = the client rate closest to `video.preferred_fps` (default 72 → 90 with
  `[90,120]`).
* `enable_foveated_encoding` = setting enabled (default **enabled**) AND (client
  `supports_foveated_encoding` OR `force_enable`). Report `false` unless you implement ALVR's
  foveation decompression.
* `game_audio_sample_rate` = sample rate of the Windows game-audio device (typically 48000).

`session` JSON = serialized `SessionConfig` (`alvr/session/src/lib.rs`):

```rust
pub struct SessionConfig {
    pub server_version: Version,          // JSON string "20.14.1"
    pub openvr_config: OpenvrConfig,
    pub client_connections: HashMap<String, ClientConnectionConfig>,
    pub session_settings: SessionSettings, // = SettingsDefault (all branches, UI representation)
}
```

`session_settings` uses the settings-schema "default" representation (see
`extrapolate_session_settings_from_session_settings` in `alvr/session/src/lib.rs`):

| Schema node | JSON |
|---|---|
| section (struct) | object with one key per field (+ `"gui_collapsed"` if collapsible) |
| choice (enum) | `{"variant": "Name", "<VariantWithData>": <data>, ...}` (data of *all* variants present) |
| `Switch<T>` | `{"enabled": bool, "content": T}` |
| `Option<T>` (schema Optional) | `{"set": bool, "content": T}` |
| fixed array | `{"gui_collapsed": bool, "content": [..]}` |
| `Vec<T>` | `{"gui_collapsed": bool, "element": T, "content": [..]}` |
| dictionary | `{"gui_collapsed": bool, "key": .., "value": .., "content": [..]}` |
| bool / number / string | plain JSON |

Fields the client needs (path → default):

| JSON path under `session_settings` | Default | Use |
|---|---|---|
| `connection.stream_protocol.variant` | `"Udp"` | `"Udp"` or `"Tcp"`; ignored if negotiated `wired` (then TCP) |
| `connection.stream_port` | 9944 | stream socket port (both sides) |
| `connection.packet_size` | 1400 | shard size, section 1.4 |
| `connection.avoid_video_glitching` | false | drop frames until IDR after loss |
| `connection.statistics_history_size` | 256 | latency averaging window |
| `connection.client_send_buffer_bytes` / `client_recv_buffer_bytes` | choice `Default`/`Maximum`/`Custom` | socket buffer sizes (optional) |
| `connection.dscp` | `{"set": false,...}` | optional IP TOS |
| `audio.game_audio.enabled` | true | receive game audio |
| `audio.game_audio.content.buffering.average_buffering_ms` / `batch_ms` | 50 / 10 | jitter buffer hints |
| `audio.microphone.enabled` | false on Windows | send microphone |
| `headset.max_prediction_ms` | 100 | prediction cap |
| `headset.controllers.enabled` | true | controllers exist in SteamVR |
| `headset.controllers.content.steamvr_pipeline_frames` | 2.1 | controller prediction |
| `headset.controllers.content.emulation_mode.variant` | `"Quest2Touch"` | set to `"ViveWand"` in the dashboard for Vive wands |
| `video.preferred_codec.variant` | `"H264"` | informational; the real codec comes in `DecoderConfig` |
| `extra.velocities_multiplier` | 1.0 | velocity scaling |
| `extra.logging.client_log_report_level` | `{"enabled":true,"content":{"variant":"Info"}}` | optional Log uplink |

**AMBIGUOUS**: the settings-schema crate itself is a git dependency not vendored in this checkout;
the representation above is derived from the ALVR code that reads/writes it (session extrapolation
and dashboard controls), not from the crate source.

### 3.5 Streaming-phase control messages

```rust
// alvr/packets/src/lib.rs
#[derive(Serialize, Deserialize)]
pub enum ServerControlPacket {            // server -> client
    StartStream,                          // 0
    DecoderConfig(DecoderInitializationConfig), // 1
    Restarting,                           // 2
    KeepAlive,                            // 3
    ServerPredictionAverage(Duration),    // 4  (not sent by 20.14.1)
    Reserved(String),                     // 5  (not sent by 20.14.1)
    ReservedBuffer(Vec<u8>),              // 6  bincode(RealTimeConfig), every 1 s
}

#[derive(Serialize, Deserialize, Clone)]
pub struct DecoderInitializationConfig {
    pub codec: CodecType,       // u32: 0 = H264, 1 = Hevc, 2 = AV1
    pub config_buffer: Vec<u8>, // e.g. SPS + PPS NALs (Annex B)
}

#[derive(Serialize, Deserialize)]
pub enum ClientControlPacket {            // client -> server
    PlayspaceSync(Option<Vec2>),          // 0
    RequestIdr,                           // 1
    KeepAlive,                            // 2
    StreamReady,                          // 3
    ViewsConfig(ViewsConfig),             // 4
    Battery(BatteryInfo),                 // 5
    VideoErrorReport,                     // 6  legacy: = packet loss + IDR request
    Buttons(Vec<ButtonEntry>),            // 7
    ActiveInteractionProfile { device_id: u64, profile_id: u64 }, // 8
    Log { level: LogSeverity, message: String },                  // 9
    Reserved(String),                     // 10 JSON of ReservedClientControlPacket
    ReservedBuffer(Vec<u8>),              // 11 (ignored by server)
}

pub struct ViewsConfig { pub ipd_m: f32, pub fov: [Fov; 2] }
pub struct Fov { pub left: f32, pub right: f32, pub up: f32, pub down: f32 } // radians
pub struct BatteryInfo { pub device_id: u64, pub gauge_value: f32 /* 0..1 */, pub is_plugged: bool }
pub enum ButtonValue { Binary(bool) /*0*/, Scalar(f32) /*1*/ }
pub struct ButtonEntry { pub path_id: u64, pub value: ButtonValue }

pub enum ReservedClientControlPacket {
    CustomInteractionProfile { device_id: u64, input_ids: HashSet<u64> },
}

// alvr/common/src/logging.rs -- serde uses DECLARATION ORDER, not the "= n" values:
pub enum LogSeverity { Error = 3 /*idx 0*/, Warning = 2 /*idx 1*/, Info = 1 /*idx 2*/, Debug = 0 /*idx 3*/ }

// alvr/session/src/settings.rs
#[repr(u8)] pub enum CodecType { H264 = 0, Hevc = 1, AV1 = 2 } // bincode idx 0,1,2 (same)
```

`RealTimeConfig` (inside `ReservedBuffer`) is bincode of
`{ passthrough: Option<PassthroughMode>, clientside_post_processing: Option<ClientsidePostProcessingConfig> }`;
the source comment says it is not version-stable; the official client silently ignores decode
failures. A PSVR client can skip it entirely (it is length-prefixed).

Server handling of client packets (`control_receive_thread` in `connection_pipeline`):

* `PlayspaceSync(area)`: unless `headset.tracking_ref_only`, **recenters** the tracking origin using
  the last received head pose (default position mode `LocalFloor` = keep y, move x/z origin;
  rotation mode `Yaw`) and sets the SteamVR chaperone to `area` (w x h metres, `None` or invalid →
  2x2). The official client sends it once at stream start and whenever the reference space
  changes. Use it as your "recenter" action.
* `RequestIdr`: if the server has a decoder config, replies with `DecoderConfig`, then asks the
  encoder for an IDR (rate-limited by `minimum_idr_interval_ms`, default 100 ms).
* `ViewsConfig`: eye transforms = ±`ipd_m/2` along x, FOVs passed to SteamVR (section 11).
* `Battery`, `Buttons` (section 7), `ActiveInteractionProfile` / `Reserved` (section 7),
  `Log` (printed), `KeepAlive`/others: only reset the timeout.

---

## 4. Stream socket

Sources: `alvr/sockets/src/stream_socket.rs`, `alvr/sockets/src/backend/{udp,tcp}.rs`,
`alvr/client_core/src/connection.rs`, `alvr/server_core/src/connection.rs`.

* Transport: `session_settings.connection.stream_protocol` (default **UDP**); forced TCP if
  negotiated `wired` is true.
* Port: `connection.stream_port` (default **9944**) on **both** sides.
* UDP: client binds `0.0.0.0:9944` and `connect()`s to `server_ip:9944`; server binds
  `0.0.0.0:9944` and `connect()`s to `client_ip:9944`. Because the server's UDP socket is
  connected, **the client must send from local port 9944** to `server_ip:9944`, and should only
  accept datagrams from `server_ip:9944`. Nothing is sent on the stream socket to "open" it; the
  server starts sending once SteamVR produces frames/haptics/audio.
* TCP: client listens on 9944 (bind before sending `StreamReady`), server connects, client accepts
  within 2 s and checks the peer IP equals the control-socket server IP. The same shard framing is
  used on the byte stream (read 18-byte prefix, then `shard_len - 14` chunk bytes).
* Framing, sharding, reassembly, ordering and loss handling: section 1.4.
* Streams used: client subscribes to VIDEO(3), AUDIO(2), HAPTICS(1); client sends TRACKING(0),
  STATISTICS(4) and, if microphone enabled, AUDIO(2).
* The official client keeps up to 10 unread packets per stream (`MAX_UNREAD_PACKETS`) and discards
  shards beyond that.

---

## 5. Video

### 5.1 Codec negotiation

There is no real negotiation: the codec is the streamer setting `video.preferred_codec` (default
**H264**), except that `AV1` becomes `Hevc` if the client reported `encoder_av1: false`. There is
**no fallback from HEVC to H264**, so configure the streamer for H264 if the PS4 only decodes
H.264. Other negotiated encoder parameters: H.264 profile `High` → `Main` if
`encoder_high_profile` is false; 10-bit only if `prefer_10bit && encoder_10_bits` (unless server
override); full range / gamma / HDR from client preferences unless server overrides
(`connection_pipeline`, `alvr/server_core/src/connection.rs`). The actual codec is announced in
`ServerControlPacket::DecoderConfig.codec`.

### 5.2 Decoder configuration

`alvr/server_openvr/cpp/alvr_server/NalParsing.cpp` (`ParseFrameNals`, `processH264Nals`,
`processHevcNals`, `sendHeaders`):

* Encoder output is **Annex B** (3- or 4-byte start codes `00 00 01` / `00 00 00 01`).
* If a frame starts with an AUD (H.264 type 9 / HEVC type 35), the AUD is removed.
* If the (next) NAL is H.264 SPS (type 7) → the SPS+PPS (2 NALs) are **cut out of the frame** and
  stored as the decoder config; HEVC VPS (type 32) → VPS+SPS+PPS (3 NALs). The config buffer keeps
  its Annex-B start codes, e.g. `00 00 00 01 67 ... 00 00 00 01 68 ...`.
* AV1: config buffer is empty (sent once with length 0). **UNVERIFIED**: the AV1 payload is the
  OBU stream as emitted by NVENC/AMF (sequence header inside key frames); not relevant for H.264.
* The config is sent to the client **only in reply to `RequestIdr`** (as
  `ServerControlPacket::DecoderConfig`). So the client must send `RequestIdr` once it is ready to
  decode (the official client does it in `set_decoder_input_callback` and whenever it sees a
  non-IDR frame while it has not yet decoded an IDR).
* IDR frames arriving on the video stream therefore do **not** contain SPS/PPS: feed the
  `config_buffer` to the decoder first (or prepend it to the IDR access unit).
* The official client uses the first `DecoderConfig` and ignores later identical ones
  (`ClientCoreEvent::DecoderConfig` comment in `alvr/client_core/src/lib.rs`).

### 5.3 Per-frame video packet (stream 3)

```rust
pub struct VideoPacketHeader { pub timestamp: Duration, pub is_idr: bool }
```

Packet data = `u64 secs, u32 nanos, u8 is_idr` (13 bytes) followed by the encoded frame (one
complete access unit, Annex B, config NALs and leading AUD stripped as above). No per-NAL length
prefixes.

* `timestamp` is the `target_timestamp` of the client `Tracking` packet whose head pose SteamVR
  used to render this frame (found via `PoseHistory::GetBestPoseMatch` in
  `alvr/server_openvr/cpp/alvr_server/PoseHistory.cpp`, called from
  `OvrDirectModeComponent::SubmitLayer`). It is 0 if no pose matched. Use it to look up the head
  pose you sent with that timestamp for reprojection/timewarp (official client:
  `report_compositor_start`, `head_pose_queue`).
* `is_idr` is the encoder's IDR flag.
* Frame layout (non-foveated): **side by side**, width `2*view_resolution.x`, height
  `view_resolution.y`, left eye on the left half (`alvr/server_openvr/cpp/platform/win32/FrameRender.cpp`,
  `m_viewportL/m_viewportR`). The encoder may pad to macroblock size (cropping in SPS).
* If `enable_foveated_encoding` is true the image is foveation-compressed and must be
  decompressed by the client (shader in `alvr/graphics`); avoid by reporting
  `supports_foveated_encoding: false`.
* Server-side: frames are dropped until the first IDR when `avoid_video_glitching` is enabled; if
  its send queue is full it drops the frame and schedules an IDR.

### 5.4 What the client sends back

There is **no per-frame ACK / "VideoFrameReceived" packet** in 20.14.1. The client sends:

* `ClientControlPacket::RequestIdr` (control socket) at start and whenever a packet was lost
  (`had_packet_loss` on a non-IDR frame) or the decoder could not accept a frame.
* `ClientStatistics` on stream 4 once per displayed frame (optional; used for the dashboard's
  latency and fps graphs and statistics summary, and by the adaptive bitrate mode. In 20.14.1 it
  does not drive any prediction: `get_motion_to_photon_latency` is commented out and returns 0,
  `tracker_pose_time_offset` is the constant `steamvr_pipeline_frames` x frame interval, and
  `GameRenderLatencyFeedback` only triggers a driver resync on Linux). The server matches it to
  the frame by `target_timestamp` in its history (frames whose tracking it received):

```rust
pub struct ClientStatistics {
    pub target_timestamp: Duration, // identifies the frame (= VideoPacketHeader.timestamp)
    pub frame_interval: Duration,       // time between the last two vsyncs
    pub video_decode: Duration,         // packet received -> decoded
    pub video_decoder_queue: Duration,  // decoded -> compositor start
    pub rendering: Duration,            // compositor start -> submit
    pub vsync_queue: Duration,          // submit -> vsync
    pub total_pipeline_latency: Duration, // tracking acquired -> vsync (drives prediction)
}
```

84 bytes (7 x 12), no payload. See `alvr/client_core/src/statistics.rs` for exactly how each
duration is measured.

---

## 6. Tracking uplink (stream 0)

Sources: `alvr/packets/src/lib.rs` (`Tracking`, `FaceData`), `alvr/common/src/primitives.rs`
(`Pose`, `DeviceMotion`), `alvr/common/src/inputs.rs` (device paths), `alvr/client_core/src/lib.rs`
(`send_tracking`), `alvr/server_core/src/tracking/mod.rs` (`tracking_loop`,
`TrackingManager::report_device_motions`, `get_device_motion`), `alvr/server_openvr/src/lib.rs`
(`ServerCoreEvent::Tracking` handler).

### 6.1 Structures

```rust
pub struct Tracking {
    pub target_timestamp: Duration,
    pub device_motions: Vec<(u64, DeviceMotion)>,
    pub hand_skeletons: [Option<[Pose; 26]>; 2],   // [left, right]
    pub face_data: FaceData,
}
pub struct DeviceMotion { pub pose: Pose, pub linear_velocity: Vec3, pub angular_velocity: Vec3 }
pub struct Pose { pub orientation: Quat, pub position: Vec3 }
pub struct FaceData {
    pub eye_gazes: [Option<Pose>; 2],
    pub fb_face_expression: Option<Vec<f32>>,
    pub htc_eye_expression: Option<Vec<f32>>,
    pub htc_lip_expression: Option<Vec<f32>>,
}
```

Binary layout (header only, no payload):

```
Duration target_timestamp        12 bytes (u64 secs, u32 nanos)
u64      device count N
N x {
  u64  device_id                 hash of path, see 6.4
  f32  qx, qy, qz, qw            orientation
  f32  px, py, pz                position (m)
  f32  vx, vy, vz                linear velocity (m/s)
  f32  wx, wy, wz                angular velocity (rad/s)
}                                60 bytes each
u8   left skeleton tag  (0 = None; 1 = Some + 26 x 28-byte Pose)
u8   right skeleton tag
u8   eye gaze left tag  (0; or 1 + 28-byte Pose)
u8   eye gaze right tag
u8   fb_face_expression tag (0; or 1 + u64 count + f32s)
u8   htc_eye_expression tag
u8   htc_lip_expression tag
```

Head-only packet = 87 bytes; head + 2 controllers = 207 bytes (always a single shard).

### 6.2 Semantics

* **Coordinate system**: the OpenXR STAGE space convention (official client uses
  `xr::ReferenceSpaceType::STAGE`): right-handed, **+Y up, -Z forward, +X right, metres, origin on
  the floor**. SteamVR uses the same convention; the driver copies values straight into
  `DriverPose_t` (`Controller::OnPoseUpdate`, `Hmd::OnPoseUpdated`). Head `y` should be the height
  above the floor (≈1.6 m standing) because the default recentering keeps `y`.
  The server's `TrackingManager` lives as long as the driver (not per connection): its
  `last_head_pose` survives a reconnection, and the recentering done on each `PlayspaceSync`
  uses it. With the default modes (LocalFloor + Yaw) a client that reconnects gets the play
  area turned to where the head faced before; the PS4 setup sets both modes to Disabled.
* Quaternion order on the wire: **x, y, z, w**, unit length.
* `linear_velocity` in m/s and `angular_velocity` in rad/s, both expressed in the **world (stage)
  frame** (as OpenXR `XrSpaceVelocity`); the server converts angular velocity to the local frame
  (`motion.pose.orientation.conjugate() * angular_velocity`) and zeroes values under
  `linear_velocity_cutoff` (0.05 m/s) / `angular_velocity_cutoff` (10 °/s).
* Head velocities: the official client predicts the head pose itself and then **sends zero head
  velocities** ("backward compatibility for the v20 protocol"). The HMD driver ignores head
  velocities anyway.
* Controller poses: the OpenXR *grip* pose. The server then applies
  `left_controller_position_offset` (default `[0, 0, -0.11]` m, mirrored in x for the right hand)
  and `left_controller_rotation_offset` (default 0) in the controller's local frame.
* `target_timestamp`: in `send_tracking` it is the **poll time** of the sample on the client's
  clock (the field name is historical); the poses themselves are predicted forward by
  `min(average total_pipeline_latency, max_prediction_ms)` for the head and by
  `min(latency - steamvr_pipeline_frames*frame_time, max_prediction)` for controllers. Any
  monotonic clock in nanoseconds works (official: OpenXR `XrTime`). It must be **unique and
  strictly increasing**: the server uses exact equality to associate the sample with stats,
  skeletons and the returned video frame, and SteamVR's pose history is keyed by it.
* Rate: the official client sends one Tracking packet every `1/(3*refresh_rate)` seconds
  (`stream_input_loop`, `alvr/client_openxr/src/stream.rs`). Each received packet immediately
  triggers a SteamVR pose update (`SetTracking`), so send at least once per display frame.
* `hand_skeletons` / `face_data`: send `None` (zero tags) for a PSVR client.
* Legacy (non-multimodal) protocol: if `use_multimodal_protocol` is false and a hand skeleton is
  sent, the server drops the corresponding `/user/hand/*` motion. Irrelevant without skeletons.

### 6.3 Device ids used in `device_motions`

Devices recognised by the server (`devices!` macro in `alvr/common/src/inputs.rs`):
`/user/head`, `/user/hand/left`, `/user/hand/right`, `/user/hand_tracker/left|right`,
`/user/body/{chest,waist,left_elbow,right_elbow,left_knee,left_foot,right_knee,right_foot}`,
`/user/detached_controller_meta/left|right`. For a PSVR + 2 controllers client use only the first
three.

### 6.4 Hash algorithm (`hash_string`)

`std::collections::hash_map::DefaultHasher::new()` + `str::hash` + `finish()`:

* Algorithm: **SipHash-1-3** (1 compression round, 3 finalization rounds) with **key k0 = k1 = 0**.
* Input bytes: the UTF-8 bytes of the string **followed by one extra byte `0xFF`**
  (`Hash for str` = `write(bytes)` + `write_u8(0xff)`).
* Output: the 64-bit SipHash result, used as a `u64` (serialized LE on the wire).
* The ALVR source comment says this is "consistent across architectures, might not be consistent
  across different compiler versions"; in practice Rust's `DefaultHasher` has been SipHash-1-3 with
  zero keys for all relevant Rust versions. **UNVERIFIED by running Rust**: the values below were
  computed with an independent implementation that reproduces the official SipHash-2-4 reference
  test vectors when run with (2,4) rounds. Sanity-check once against a real streamer (e.g. the
  dashboard's "Unknown (ID: ...)" log lines or the protocol-id warning).

Reference C implementation (not compiled in this environment; mirrors the verified Python):

```c
#include <stdint.h>
#include <string.h>
static uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define SIPROUND do { \
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32); \
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;                       \
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;                       \
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32); \
} while (0)
/* ALVR hash_string(): SipHash-1-3, key 0, message = s || 0xFF */
uint64_t alvr_hash_string(const char *s) {
    const size_t len = strlen(s), n = len + 1;
#define MSG(j) ((j) < len ? (uint64_t)(uint8_t)s[j] : (uint64_t)0xFF)
    uint64_t v0 = 0x736f6d6570736575ULL, v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL, v3 = 0x7465646279746573ULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m = 0;
        for (int k = 0; k < 8; k++) m |= MSG(i + k) << (8 * k);
        v3 ^= m; SIPROUND; v0 ^= m;
    }
    uint64_t b = (uint64_t)(n & 0xff) << 56;
    for (size_t k = 0; i + k < n; k++) b |= MSG(i + k) << (8 * k);
    v3 ^= b; SIPROUND; v0 ^= b;
    v2 ^= 0xff; SIPROUND; SIPROUND; SIPROUND;
#undef MSG
    return v0 ^ v1 ^ v2 ^ v3;
}
```

Example ids:

| Path | id (hex) | wire bytes (LE) |
|---|---|---|
| `20` (protocol id) | `0x17667eafcf6d5d67` | `67 5d 6d cf af 7e 66 17` |
| `/user/head` | `0x5b90853dc9202538` | `38 25 20 c9 3d 85 90 5b` |
| `/user/hand/left` | `0xe521d8dabe2d07a2` | `a2 07 2d be da d8 21 e5` |
| `/user/hand/right` | `0xf6b81330eb3dbdd8` | `d8 bd 3d eb 30 13 b8 f6` |
| `/user/hand_tracker/left` | `0x2c89477570e27f48` | |
| `/user/hand_tracker/right` | `0x8de3f222ebcb8f34` | |
| `/interaction_profiles/htc/vive_controller` | `0x492bd9426d3d1cea` | `ea 1c 3d 6d 42 d9 2b 49` |
| `/interaction_profiles/oculus/touch_controller` | `0x0812fd85c84384a6` | |
| `/interaction_profiles/valve/index_controller` | `0x41c90244d8f880a9` | |

---

## 7. Button / input uplink

Sources: `alvr/common/src/inputs.rs` (`controller_inputs!`, `CONTROLLER_PROFILE_INFO`),
`alvr/server_core/src/input_mapping.rs` (`registered_button_set`, `automatic_bindings`,
`ButtonMappingManager`), `alvr/server_core/src/connection.rs` (control receive thread),
`alvr/server_openvr/cpp/alvr_server/Paths.cpp`, `Controller.cpp`, `alvr/server_openvr/src/props.rs`.

### 7.1 Wire format

`ClientControlPacket::Buttons(Vec<ButtonEntry>)` on the **control socket** (TCP), not the stream
socket. Each entry: `u64 path_id` + `ButtonValue` (`u32 0` + `u8 bool` for Binary, or `u32 1` +
`f32` for Scalar). See the hex examples in 1.5. The official client sends only entries whose value
changed since the last poll (`update_buttons` in `alvr/client_openxr/src/interaction.rs`); it sends
nothing when nothing changed. Scalars: trigger/squeeze/force 0..1, thumbstick/trackpad axes -1..1.
The server drops binary entries whose value equals the last value it saw for that id.

Buttons are applied to SteamVR only while the controller pose is valid
(`Controller::SetButton` returns early if `!last_pose.poseIsValid`).

### 7.2 Input path ids

`path_id = hash_string(path)`. All paths the server knows (`BUTTON_INFO`), for `<h>` = `left` or
`right`: `/user/hand/<h>/input/` + one of
`system/click, system/touch, menu/click, back/click, a/click, a/touch, b/click, b/touch, x/click,
x/touch, y/click, y/touch, squeeze/click, squeeze/touch, squeeze/value (scalar), squeeze/force
(scalar), trigger/click, trigger/value (scalar), trigger/touch, thumbstick/x (scalar),
thumbstick/y (scalar), thumbstick/click, thumbstick/touch, trackpad/x (scalar), trackpad/y
(scalar), trackpad/click, trackpad/force (scalar), trackpad/touch, thumbrest/touch`.

Ids relevant for a Vive-wand-like controller:

| Path | left id | right id |
|---|---|---|
| `.../input/system/click` | `0x4282986ff96edd00` | `0x19729bd97073d474` |
| `.../input/menu/click` | `0x6268a669e87cf910` | `0xed68cbf384f9f033` |
| `.../input/squeeze/click` | `0x8b4961b606fca1bd` | `0x3dd6a3baec0ce32c` |
| `.../input/squeeze/value` | `0x01f1b9c689804b82` | `0xdd5935cc005136f1` |
| `.../input/trigger/click` | `0x43481636889dfed0` | `0xc6aa9987f4ccc0a7` |
| `.../input/trigger/value` | `0x0b66bbe3dc2dc49f` | `0xede7e982d5eedf21` |
| `.../input/trigger/touch` | `0x4fa57f59dbae033a` | `0xe0a7e848dde340b0` |
| `.../input/trackpad/x` | `0x540c8edffb6b6cf3` | `0xcde5500915944bbc` |
| `.../input/trackpad/y` | `0x546bd12c864c9e52` | `0x301f2c4b4c39b890` |
| `.../input/trackpad/click` | `0x346d7e36d4dced73` | `0x3beaba3fffbd6246` |
| `.../input/trackpad/touch` | `0x5b8a90e1e5ff47a8` | `0x655bd7d571d251a7` |
| `.../input/thumbstick/x` | `0xc3d709ed0d794ea1` | `0xd7c23648f060c6df` |
| `.../input/thumbstick/y` | `0xcb7d928706263554` | `0xcee8c338e02e0131` |
| `.../input/thumbstick/click` | `0xdfc8a61273cf2569` | `0x00ac9a92f4453d61` |
| `.../input/thumbstick/touch` | `0x0966df80f4989bfc` | `0xef43fe49dfc87556` |
| `.../input/a/click` | `0x066555b895808431` | `0xc72a0ba6ca52fba7` |
| `.../input/b/click` | `0x5c88af3a2dbf9a1e` | `0x955047466e34669b` |
| `.../input/x/click` | `0xb1a13c485685e33c` | `0x99e1215a0d426b4a` |
| `.../input/y/click` | `0x521496d23e3e9883` | `0xaafe5312f54dd9d8` |

(Other ids can be computed with `alvr_hash_string`.)

### 7.3 How the server maps inputs (important)

The client's ids are **source** inputs. The server translates them to **destination** inputs of
the emulated controller chosen in the streamer setting `headset.controllers.emulation_mode`
(`registered_button_set`). For `ViveWand` the destination set is the Vive profile set:

```
/user/hand/{left,right}/input/system/click
/user/hand/{left,right}/input/squeeze/click
/user/hand/{left,right}/input/menu/click
/user/hand/{left,right}/input/trigger/click
/user/hand/{left,right}/input/trigger/value
/user/hand/{left,right}/input/trackpad/x
/user/hand/{left,right}/input/trackpad/y
/user/hand/{left,right}/input/trackpad/click
/user/hand/{left,right}/input/trackpad/touch
```

and SteamVR components (`Paths.cpp`): system/click → `/input/system/click`; menu/click →
`/input/system/click` **and** `/input/application_menu/click`; squeeze/click → `/input/grip/click`;
trigger/click → `/input/trigger/click`; trigger/value → `/input/trigger/value`; trackpad/x|y|click|touch
→ `/input/trackpad/x|y|click|touch`. Device props for ViveWand (`props.rs`): controller type
`vive_controller`, render model `vr_controller_vive_1_5`, registered device type
`htc/vive_controller_Left|Right`, but **`InputProfilePathString` = `{oculus}/input/touch_profile.json`**
(as written in the source; possibly a bug; it affects SteamVR bindings UI).

The mapping from source to destination (`ButtonMappingManager`):

* **Source set** = the button set of the client's *active interaction profile*. Initially (before
  any message) it is the **Oculus Touch** set. It is replaced by
  `ActiveInteractionProfile { device_id, profile_id }` (`profile_id` = hash of a profile path in
  `CONTROLLER_PROFILE_INFO`; `device_id` is ignored; one mapping for both hands) or by
  `Reserved(JSON CustomInteractionProfile { device_id, input_ids })` (arbitrary source set).
* If the streamer has manual `headset.controllers.button_mappings` set, those are used instead
  (paths as strings, `ButtonBindingTarget { destination, mapping_type: Passthrough |
  HysteresisThreshold | BinaryToScalar | Remap, binary_conditions }`).
* Otherwise `automatic_bindings(source, destination)` produces bindings only for these source
  inputs: menu/click, X/A, Y/B, squeeze, trigger, thumbstick x/y/click/touch, thumbrest/touch.
  **There are no rules whose source is `trackpad/*` or `system/click`**, so those source ids are
  logged as "Received button not mapped" and dropped, even if the destination is a Vive wand.

Consequences for a Vive-wand emulation (streamer emulation mode = "Vive Wand"):

* Option A (recommended, no streamer config besides emulation mode): after `StartStream`/connection,
  send `ClientControlPacket::Reserved` with
  `{"CustomInteractionProfile":{"device_id":<hash /user/hand/left>,"input_ids":[...]}}` listing,
  for both hands, `menu/click, squeeze/click, trigger/click, trigger/value, thumbstick/x,
  thumbstick/y, thumbstick/click, thumbstick/touch`. Then send PS Move inputs using **those**
  source paths. The automatic bindings become: menu→menu, squeeze/click→grip click,
  trigger/click→trigger click, trigger/value→trigger value, **thumbstick/x|y → trackpad/x|y,
  thumbstick/click → trackpad/click, thumbstick/touch → trackpad/touch**. Write the u64 ids in the
  JSON as exact integers (they exceed 2^53; do not format through a double). JSON example:
  `{"CustomInteractionProfile":{"device_id":16510716142922041250,"input_ids":[821550569367651487, ...]}}`.
* Option B: send `ActiveInteractionProfile` with `profile_id = 0x492bd9426d3d1cea` (Vive) and Vive
  paths. Then only menu/click, squeeze/click, trigger/click, trigger/value work; **trackpad and
  system are not forwarded**.
* Option C: configure manual `button_mappings` in the streamer (Passthrough from any path, e.g.
  `/user/hand/left/input/system/click` → same path) to get system/click and native trackpad paths.
* `system/click` for the Vive wand is only reachable via Option C (automatic rules map
  `menu/click` → `system/click` only when the destination lacks `menu/click`, which Vive has).
* Sending no profile message (default Touch source set) with Vive destination: left X / right A
  click → trackpad click, thumbstick x/y/click/touch → trackpad x/y/click/touch, thumbrest/touch →
  trackpad touch, left menu → menu, squeeze/value → grip click (hysteresis 0.5±0.05),
  trigger/value → trigger click (hysteresis) + trigger value; right `menu/click` is not in the
  Touch set and `system/click` has no rule, so neither is forwarded.

Automatic-mapping thresholds (`button_mapping_config` defaults): click threshold 0.5 ± 0.05,
touch threshold 0.1 ± 0.05, force threshold 0.8.

---

## 8. Haptics downlink (stream 1)

```rust
pub struct Haptics {
    pub device_id: u64,     // hash of /user/hand/left or /user/hand/right
    pub duration: Duration, // seconds+nanos
    pub frequency: f32,     // Hz, as given by SteamVR (fFrequency)
    pub amplitude: f32,     // 0..1 from SteamVR, then scaled
}
```

Header only (28 bytes), empty payload, stream id 1. Generated from SteamVR
`VREvent_Input_HapticVibration` (`alvr/server_openvr/cpp/alvr_server/alvr_server.cpp`,
`HapticsSend`) and post-processed by `alvr/server_core/src/haptics.rs` (`map_haptics`):
`duration = max(duration, min_duration_s)` (default 0.01 s), `amplitude = intensity_multiplier *
amplitude^amplitude_curve` (defaults 1.0, 1.0). Only sent if `headset.controllers.content.haptics`
is enabled (default enabled). Frequency may be 0 (SteamVR default) and duration may be 0 before
clamping; amplitude after scaling may exceed 1 if the multiplier > 1.

Example (left hand, 10 ms, 160 Hz, 0.8), datagram:

```
00 00 00 2a  00 01  00 00 00 00  00 00 00 01  00 00 00 00
a2 07 2d be da d8 21 e5  00 00 00 00 00 00 00 00  80 96 98 00  00 00 20 43  cd cc 4c 3f
```

---

## 9. Audio

Sources: `alvr/audio/src/lib.rs` (`record_audio_blocking`, `play_audio_loop`,
`receive_samples_loop`), `alvr/server_core/src/connection.rs`, `alvr/client_core/src/connection.rs`.

* Both directions use stream id **2 (AUDIO)**; header type `()` (0 bytes), so the reassembled
  packet is pure PCM.
* Sample format: **signed 16-bit PCM, interleaved, native endianness** of the sender
  (`to_ne_bytes`/`from_ne_bytes`); x86 Windows and PS4 are both little endian → s16le.
* **Game audio (server → client)**: 2 channels (stereo, L R), sample rate =
  `negotiated.game_audio_sample_rate` (the Windows device rate, usually 48000; the server does not
  resample). Only if `session_settings.audio.game_audio.enabled`. Packet size = whatever the WASAPI
  capture callback delivered (typically ~10 ms, not fixed); do not assume a fixed frame count.
  Multichannel devices are downmixed to stereo on the server.
* **Microphone (client → server)**: 1 channel (mono) s16le at the `microphone_sample_rate` the
  client reported in its capabilities. Only if `session_settings.audio.microphone.enabled`
  (default **off** on Windows; requires a virtual audio cable). Keep each packet ≤ `D` bytes
  (1386 by default) or shard correctly; ~10 ms chunks (960 bytes at 48 kHz) are fine.
* No timestamps or sequence information besides the stream `packet_index` (loss triggers a
  fade in the official player). Jitter buffer hints: `buffering.average_buffering_ms` (50) and
  `batch_ms` (10).

---

## 10. Timing, keepalive and liveness requirements

Constants: `alvr/sockets/src/lib.rs`, `alvr/client_core/src/connection.rs`,
`alvr/server_core/src/connection.rs`.

| Item | Value | Consequence |
|---|---|---|
| `KEEPALIVE_INTERVAL` | 500 ms | both sides send `KeepAlive` on the control socket at this period |
| `KEEPALIVE_TIMEOUT` | 2 s | the server disconnects if it receives **no control packet of any kind** for 2 s; the client does the same |
| `HANDSHAKE_ACTION_TIMEOUT` | 2 s | every handshake wait (section 3.2) |
| server TCP connect | 1 s | client must accept quickly |
| `STREAMING_RECV_TIMEOUT` | 500 ms | socket read timeout granularity |
| RealTimeConfig | every 1 s | server → client `ReservedBuffer` |

The stream socket has **no** keepalive: the connection lives as long as the control socket does.
A TCP error on either socket (or a stream socket error in TCP mode) ends the session. After a
disconnect, close both sockets and restart discovery (the server returns the client to
`Disconnected` and reconnects on the next announce/manual-IP round).

Minimum the client must do for the stream to start and stay alive:

1. Listen TCP 9943, announce on UDP 9943 (or be added by manual IP and trusted).
2. Send `ConnectionAccepted` with the right protocol id and valid capabilities.
3. Receive `StreamConfigPacket`, then `StartStream` (reconnect if `Restarting`).
4. Bind the stream socket on `stream_port`, then send `StreamReady` within 2 s.
5. Send `KeepAlive` every 500 ms (any control packet resets the server timer).
6. Send `Tracking` with at least the head continuously (SteamVR poses come only from it).
7. Send `RequestIdr` to get `DecoderConfig` + first IDR.

Optional but recommended: `ViewsConfig` (otherwise SteamVR keeps the default ±1 rad FOV and 63 mm
IPD from `Hmd` constructor in `HMD.cpp`), `PlayspaceSync`, `ActiveInteractionProfile` /
`CustomInteractionProfile`, `ClientStatistics` per frame, `Battery` (official: every 5 s for the
head, device_id = head id).

---

## 11. Capabilities reporting (PSVR)

| What | Where | Units / format |
|---|---|---|
| per-eye display resolution | `ConnectionAccepted.streaming_capabilities.default_view_resolution` (and JSON copy) | pixels, per eye: `[960, 1080]` |
| refresh rates | positive entries of `supported_refresh_rates_plus_extra_data` (and JSON copy) | Hz f32: `90.0, 120.0` |
| codecs | JSON keys `encoder_av1`, `encoder_10_bits`, `encoder_high_profile`, prefs | booleans (no way to refuse HEVC or request H.264; configure the streamer) |
| foveation support | JSON `supports_foveated_encoding` | send `false` |
| microphone rate | `microphone_sample_rate` | Hz |
| FOV + IPD | `ClientControlPacket::ViewsConfig` after the stream starts | radians / metres |

`ViewsConfig`:

```rust
pub struct ViewsConfig {
    // Note: the head-to-eye transform is always a translation along the x axis
    pub ipd_m: f32,     // metres
    pub fov: [Fov; 2],  // [left eye, right eye]
}
pub struct Fov { pub left: f32, pub right: f32, pub up: f32, pub down: f32 } // radians
```

* Angles in radians of the OpenXR `XrFovf` convention: `left` and `down` are **negative**, `right`
  and `up` positive. The driver converts with `tan()` (`fov_to_projection` in
  `alvr/server_openvr/cpp/alvr_server/HMD.cpp`).
* The server builds eye poses at `(∓ipd/2, 0, 0)` relative to the head, identity rotation.
* The official client sends it on the first tracking iteration and whenever the IPD changes by
  more than an epsilon (`get_head_data` in `alvr/client_openxr/src/interaction.rs`); it must be sent
  after streaming starts (the control sender only exists then). For canted displays the client
  first converts views to parallel ones (`canted_view_to_proportional_circumscribed_orthogonal`,
  `alvr/client_core/src/lib.rs`); PSVR views are parallel, so send the raw angles.
* Example frame (ipd 0.063, placeholder FOV left eye [-50°, 40°, 50°, -50°], right eye
  [-40°, 50°, 50°, -50°] — **replace with measured PSVR values**):

```
00 00 00 28  04 00 00 00  25 06 81 3d
f3 66 5f bf  c2 b8 32 3f  f3 66 5f 3f  f3 66 5f bf
c2 b8 32 bf  f3 66 5f 3f  f3 66 5f 3f  f3 66 5f bf
```

Resolution caveat: see 3.4 — with the streamer defaults a 960x1080 client gets a 2144x2400-per-eye
stream. Set "Transcoding view resolution" (and "Emulated headset view resolution") on the
streamer, remember the 32-pixel flooring (1080 → 1056), and make sure your H.264 decoder supports
the resulting size and level at 90/120 fps.

---

## 12. Tracking-lost semantics

* **Controllers**: simply **omit** the device (`/user/hand/left` or `/user/hand/right`) from
  `device_motions` in the Tracking packets while it is not tracked. `TrackingManager::get_device_motion`
  then finds no sample at or after that timestamp and returns `None`; the driver
  (`Controller::OnPoseUpdate`, `Controller.cpp`) submits `poseIsValid = false`,
  `deviceIsConnected = false`, `result = TrackingResult_Uninitialized`, and button updates are
  ignored while invalid. There is **no field for "connected but not tracked" / "out of range"**
  in the 20.14.1 protocol, so SteamVR shows the controller as not connected/off rather than as
  "searching" (**AMBIGUOUS**: the exact SteamVR icon state for `deviceIsConnected=false` was not
  verified). Sending a pose with zeroed data does **not** mark it lost — it is treated as a valid
  pose. The streamer setting `headset.controllers.content.tracked = false` forces both controllers
  off.
* **Headset**: cannot be reported as lost. `Hmd::OnPoseUpdated` always submits
  `poseIsValid = true`; if `/user/head` is missing from a packet the server uses a zeroed
  `FfiDeviceMotion` (quaternion 0,0,0,0 — invalid) for that update. Always send the head (last
  known pose if tracking is lost).

---

## 13. Minimal client implementation checklist

1. **Hashing & bincode helpers**: `alvr_hash_string` (SipHash-1-3, key 0, `s||0xFF`); LE
   writers/readers for u32/u64/f32/bool/String/Vec/Option/enum index/Duration; verify against the
   hex examples in this document.
2. **Discovery**: persist a hostname like `NNNN.client` (≤ 32 bytes). Bind UDP `0.0.0.0:9943`
   with `SO_BROADCAST`, broadcast the 56-byte announce to `255.255.255.255:9943` every ~1 s.
   Listen TCP on `0.0.0.0:9943`. Click "Trust" in the dashboard (or add a manual IP).
3. **Control handshake**: on accept, remember the peer IP as `server_ip`; send framed
   `ConnectionAccepted` (protocol id `0x17667eafcf6d5d67`, capabilities JSON with every key,
   foveation/AV1/10-bit false). Receive `StreamConfigPacket`, parse `negotiated` and the needed
   `session_settings` paths (stream_protocol, stream_port, packet_size, audio switches).
4. **Stream start**: read one `ServerControlPacket`; `Restarting` → close and go back to 2;
   `StartStream` → bind UDP `0.0.0.0:stream_port` connected to `server_ip:stream_port` (or listen
   TCP), then send `StreamReady`; (TCP) accept the server within 2 s.
5. **Liveness**: control-socket thread sending `KeepAlive` every 500 ms; receive loop handling
   `KeepAlive`, `DecoderConfig`, `Restarting` (→ disconnect), `ReservedBuffer` (ignore); disconnect
   if nothing arrives for 2 s.
6. **Shard layer**: 18-byte BE prefix, per-stream counters, reassembly with `D = packet_size + 4 - 18`,
   loss detection via packet_index.
7. **Video**: send `RequestIdr`; on `DecoderConfig` initialise the H.264 decoder with the Annex-B
   SPS/PPS; parse 13-byte `VideoPacketHeader` + Annex-B access unit; drop non-IDR frames and send
   `RequestIdr` after any loss until the next `is_idr`; render side-by-side halves using the head
   pose you sent with `timestamp`.
8. **Tracking**: at ≥ display rate (official: 3x) send `Tracking` with head (+ controllers when
   tracked, omitted when lost), stage-space floor-relative metres, quaternion x,y,z,w, strictly
   increasing ns timestamps; send `ViewsConfig` (radians, IPD metres) once after start and on
   change; send `PlayspaceSync` once (it recenters).
9. **Input**: set streamer emulation mode to Vive Wand; send `Reserved(CustomInteractionProfile)`
   (Option A in 7.3); send `Buttons` entries on change (binary click/touch, scalar values).
10. **Statistics** (recommended): per displayed frame send `ClientStatistics` on stream 4.
11. **Haptics**: subscribe to stream 1, parse the 28-byte `Haptics` header, map `device_id` to
    the left/right PS Move rumble.
12. **Audio**: stream 2 downlink = s16le stereo at `game_audio_sample_rate`; optional microphone
    uplink = s16le mono at your reported `microphone_sample_rate` (only if enabled in the session).
