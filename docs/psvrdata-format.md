# The .psvrdata recording format

[Back to the README](../README.md)

ALVR PS4 Tracking Viewer saves its recordings as `.psvrdata` files. A recording keeps every
tracking state the PS4 sent while it was recording, byte for byte as it arrived over the
network, with the time it arrived. Playing it back decodes those states again, so a
recording shows exactly what the live view showed.

This page describes version 1 of the file format, which carries version 1 of the network
state packet. The code that writes and reads these files is in `companion/trackview.cpp`
(`write_clip`, `read_clip`). The state packet is defined in `client/src/trackview_proto.h`
(`trackview_pack_state`, `trackview_unpack_state`). The PS4 app and the viewer compile the
same header.

## General rules

* **Byte order:** every number is little-endian.
* **Number types:**
  * `u8`, `u16` and `u32` are unsigned integers of 1, 2 and 4 bytes;
  * `f32` is an IEEE 754 single-precision float;
  * `char` is one byte.
* **No padding:** fields follow each other directly, with no alignment.
* **Units:** distances are in metres and times in milliseconds, unless a field says
  otherwise.

## File layout

A file is a header, followed by the frames one after the other. Nothing comes after the
last frame.

### Header (24 bytes)

| Offset | Type | Field | Value |
| --- | --- | --- | --- |
| 0 | 8 × `char` | magic | `PSVRDATA` (ASCII, no terminating zero) |
| 8 | `u32` | file format version | `1` |
| 12 | `u32` | wire version | `1`, the version of the state packets inside (`TRACKVIEW_VERSION`) |
| 16 | `u32` | frame count | number of frames that follow |
| 20 | `u32` | duration | time of the last frame, in ms (0 when the file has no frame) |

### Frame (6 bytes + packet)

| Offset in the frame | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | `u32` | time | ms since recording started, measured on the PC when the packet arrived |
| 4 | `u16` | size | packet size in bytes, at most 1400 (`TRACKVIEW_PACKET_MAX`) |
| 6 | `size` bytes | packet | one state packet, as received (see below) |

**Frame order and timing:**

* Frames are stored in arrival order, and their times never decrease.
* The first frame's time is usually not 0. It is the delay between clicking Record and the
  first state received.
* The PS4 sends at most one state every 15 ms (about 66 a second). In practice it sends
  about 45 a second, at the app's frame rate.
* Over UDP, a packet older than the last one received is dropped before it is recorded.
  The packets' sequence numbers can therefore skip.

**How the viewer checks a file it loads.** It rejects the file when any of these holds:

* the magic is wrong;
* either version differs from its own;
* a frame is larger than 1400 bytes;
* a frame's time goes backwards;
* the file ends before the last frame;
* the file has no frame at all.

## State packet (wire version 1)

A state packet is laid out in this order:

1. a header;
2. the floor and the play space;
3. the headset;
4. the list of visible PS Moves;
5. the list of visible DualShock 4 controllers.

### Coordinate frame

All positions and orientations are in the tracker's space (libSceVrTracker):

* **Origin and units:** the origin is the PS Camera, and distances are in metres.
* **Axes:** +Y points up. The play area lies in front of the camera, towards +Z. A user
  facing the camera looks towards -Z.
* **Floor:** the floor is below the camera, so its height is negative (minus the camera's
  height above the floor).

### Encoding of a pose (28 bytes)

A pose is 7 `f32`, in this order: `px py pz qx qy qz qw`.

* `px py pz` is the position.
* `qx qy qz qw` is a unit quaternion. It rotates the device's own frame into tracker space.
  The device frames are:
  * **headset:** +X right, +Y up, -Z forward (where the user looks);
  * **PS Move:** the handle along +Z, the sphere along -Z. The controller's forward
    (handle to sphere) is -Z;
  * **DualShock 4:** +X right, +Y up (the touchpad side), -Z forward (the light bar side).
    The origin is the light bar.

### Header (12 bytes)

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | `u32` | magic | `0x54345041`: the bytes `A` `P` `4` `T` |
| 4 | `u16` | version | `1` |
| 6 | `u16` | flags | bit 0: floor grid shown (the floor height is known, not just guessed); other bits 0 |
| 8 | `u32` | sequence number | +1 for each state the app sends. It restarts from 0 when the app restarts. |

### Floor and play space (12 bytes)

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 12 | `f32` | floor_y | height of the floor |
| 16 | `f32` | center_x | x of the play space centre, on the floor |
| 20 | `f32` | center_z | z of the play space centre (the floor grid is aligned on it) |

### Headset (29 bytes)

| Offset | Type | Field | Meaning |
| --- | --- | --- | --- |
| 24 | `u8` | flags | bit 0: visible (the headset tracking has started); bit 1: tracked (the camera sees it now) |
| 25 | pose | pose | centre between the eyes |

### PS Moves

The list starts with `u8 count` at offset 53, the number of entries that follow. Only
visible controllers are listed. Each entry is 53 bytes:

| Offset in the entry | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | `u8` | slot | 0-7, see below |
| 1 | `u8` | flags | see below |
| 2 | `char` | label | drawn on the handle; see below |
| 3 | `u16` | buttons | pressed buttons; see below |
| 5 | pose | pose | position of the sphere centre, and orientation |
| 33 | `u32` | colour | sphere colour, `0x00RRGGBB` |
| 37 | `f32` | trigger | 0 (released) to 1 (fully pulled) |
| 41 | `f32` | battery | 0 to 1 in steps of 0.2 (the controller reports 6 levels); negative when unknown |
| 45 | `f32` | pad_x | emulated Vive trackpad point, -1 (left) to 1 (right) |
| 49 | `f32` | pad_y | -1 (bottom) to 1 (top) |

**Slots:**

* Slots 0 and 1 are the PS Moves of the user playing: slot 0 is the right hand (`R`),
  slot 1 the left hand (`L`).
* Slots 2-7 are the PS Moves of up to three other users signed in on the PS4. Each of
  these users has two slots: their first slot is 2, 4 or 6, and the next slot is their
  second Move.

**Flags:**

| Bit | Meaning |
| --- | --- |
| 0 | tracked: the camera sees the sphere now. When it is clear, the position is a prediction (the tracker's estimate or the arm model, see [Usage](usage.md)). |
| 1 | trackpad touched |
| 2 | trackpad clicked |
| 3 | charging |

**Labels:** `L` or `R` for the user playing; `2`, `3` or `4` for another user, by their
user number.

**Buttons:**

| Bit | Button |
| --- | --- |
| `0x0001` | SELECT |
| `0x0002` | T (trigger touched, from about 16 % of the travel) |
| `0x0004` | MOVE |
| `0x0008` | START |
| `0x0010` | Triangle |
| `0x0020` | Circle |
| `0x0040` | Cross |
| `0x0080` | Square |
| `0x8000` | PS |

**Colour values:** the tracker gives each controller one of these sphere colours:

| Colour | Value |
| --- | --- |
| blue | `0x0000FF` |
| red | `0xFF0000` |
| cyan | `0x00FFFF` |
| magenta | `0xFF00FF` |
| yellow | `0xFFFF00` |
| white | `0xFFFFFF` (none assigned) |

**Note on the trackpad fields:** `pad_x`, `pad_y` and the touch and click flags are filled
only for slots 0 and 1. They are 0 for the other users' controllers.

### DualShock 4 controllers

This list follows the PS Moves. It starts with `u8 count`, then the entries. Only visible
pads are listed. Each entry is 91 bytes:

| Offset in the entry | Type | Field | Meaning |
| --- | --- | --- | --- |
| 0 | `u8` | slot | 0: the user playing; 1-3: other users |
| 1 | `u8` | flags | see below |
| 2 | `char` | label | 0 for the user playing, else the user number `2`-`4` |
| 3 | `u32` | buttons | pressed buttons; see below |
| 7 | pose | pose | light bar centre and orientation; see the note on floating pads below |
| 35 | `u32` | colour | light bar colour, `0x00RRGGBB` |
| 39 | 13 × `f32` | analog values | in this order: `lx ly rx ry l2 r2 touch_x0 touch_x1 touch_y0 touch_y1 battery rumble_large rumble_small` |

**Flags:**

| Bit | Meaning |
| --- | --- |
| 0 | tracked: the camera sees the light bar now |
| 1 | touchpad finger 0 down |
| 2 | touchpad finger 1 down |
| 3 | charging |
| 4 | floating |

**Floating pads:** a floating pad is connected but not tracked (the tracker runs two
controllers, and the PS Moves come first). Its pose is a fixed place in front of the play
area, where the viewer draws it, not a measured one.

**Buttons:**

| Bit | Button |
| --- | --- |
| `0x0002` | L3 |
| `0x0004` | R3 |
| `0x0008` | OPTIONS |
| `0x0010` | Up |
| `0x0020` | Right |
| `0x0040` | Down |
| `0x0080` | Left |
| `0x0100` | L2 |
| `0x0200` | R2 |
| `0x0400` | L1 |
| `0x0800` | R1 |
| `0x1000` | Triangle |
| `0x2000` | Circle |
| `0x4000` | Cross |
| `0x8000` | Square |
| `0x100000` | touchpad click |

**Analog values:**

* sticks `lx ly rx ry`: -1 to 1, with +y up;
* triggers `l2 r2`: 0 to 1;
* touch points: 0 to 1, measured from the touchpad's top left corner;
* battery: 0 to 1, negative when unknown;
* rumble motors: 0 to 1.

### Packet sizes

A packet is 55 bytes, plus 53 bytes per PS Move and 91 bytes per DualShock 4.

* Two PS Moves and no pad make 161 bytes, so a recording of them takes about 7.5 KB a
  second.
* The largest possible packet (8 Moves, 4 pads) is 843 bytes.

## Reading a file (Python)

This reader decodes every frame of a recording:

```python
import struct

def read_psvrdata(path):
    data = open(path, "rb").read()
    if data[:8] != b"PSVRDATA":
        raise ValueError("not a .psvrdata file")
    file_version, wire_version, count, duration_ms = struct.unpack_from("<4I", data, 8)
    if file_version != 1 or wire_version != 1:
        raise ValueError("unsupported version")
    pos = 24
    for _ in range(count):
        time_ms, size = struct.unpack_from("<IH", data, pos)
        packet = data[pos + 6:pos + 6 + size]
        pos += 6 + size
        yield time_ms, parse_state(packet)

def parse_state(p):
    magic, version, flags, seq = struct.unpack_from("<IHHI", p, 0)
    assert magic == 0x54345041 and version == 1
    floor_y, center_x, center_z = struct.unpack_from("<3f", p, 12)
    head_flags = p[24]
    state = {
        "seq": seq, "grid": bool(flags & 1), "floor_y": floor_y, "center": (center_x, center_z),
        "headset": {"visible": bool(head_flags & 1), "tracked": bool(head_flags & 2),
                    "pose": struct.unpack_from("<7f", p, 25)},
        "moves": [], "pads": [],
    }
    count, o = p[53], 54  # PS Moves
    for _ in range(count):
        slot, f, label = p[o], p[o + 1], chr(p[o + 2])
        buttons, = struct.unpack_from("<H", p, o + 3)
        pose = struct.unpack_from("<7f", p, o + 5)
        rgb, = struct.unpack_from("<I", p, o + 33)
        trigger, battery, pad_x, pad_y = struct.unpack_from("<4f", p, o + 37)
        state["moves"].append({"slot": slot, "tracked": bool(f & 1), "pad_touch": bool(f & 2),
                               "pad_click": bool(f & 4), "charging": bool(f & 8), "label": label,
                               "buttons": buttons, "pose": pose, "rgb": rgb, "trigger": trigger,
                               "battery": battery, "pad": (pad_x, pad_y)})
        o += 53
    count, o = p[o], o + 1
    for _ in range(count):
        slot, f, label = p[o], p[o + 1], p[o + 2]
        buttons, = struct.unpack_from("<I", p, o + 3)
        pose = struct.unpack_from("<7f", p, o + 7)
        rgb, = struct.unpack_from("<I", p, o + 35)
        values = struct.unpack_from("<13f", p, o + 39)
        state["pads"].append({"slot": slot, "tracked": bool(f & 1), "touch": (bool(f & 2), bool(f & 4)),
                              "charging": bool(f & 8), "floating": bool(f & 16),
                              "label": chr(label) if label else "", "buttons": buttons, "pose": pose,
                              "rgb": rgb, "values": values})
        o += 91
    return state
```

## Versions

**When the format changes:**

* A change to the state packet raises `TRACKVIEW_VERSION` in `client/src/trackview_proto.h`.
  The app and the viewer must then be the same version: the viewer's title bar says so when
  they are not.
* A change to the file layout itself raises `PSVRDATA_VERSION` in `companion/trackview.cpp`.

**Old recordings:** the viewer opens only files whose two versions match its own. A
recording made with an older viewer then needs that viewer to be played back.
