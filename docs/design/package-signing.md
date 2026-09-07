# Package signing — device-key HMAC vs. package provenance

Found and fixed 2026-09-07 (project chat, Simon's review) while trying to
push the js-api.md acceptance-test face to a second device: the original
signing design conflated two different questions, and answered only one
of them.

## 1. The two questions, and why HMAC only answers one

- **"May this be installed on this device?"** — a permission check. This
  is what `install_impl()` (`app_store.c`) has always verified, and what
  it still verifies.
- **"Who built this package?"** — a provenance/authorship check. HMAC
  cannot answer this at all: it's a *symmetric* scheme, both sides hold
  the same secret. Anyone who can *verify* a package with a given key
  could equally well have *signed* one with it. There is no cryptographic
  distinction between "the device's owner installing their own package"
  and "someone who happens to know the device's key forging one" - HMAC
  was never designed to prove the second thing, only the first.

## 2. The bug this caused

The original design (`kb_store_install_default_face()`'s launcher-states.md
history, and every `atelier.py pack --key ...` invocation before this
doc) signed a package **at pack time**, baking one specific device's key
into the `.comp` file itself. That works for exactly one device - the one
whose key was used - and breaks distribution completely: a package
published for others to install could only ever be installed by whoever
already had the *signer's* key, which in practice meant "the same person
who built it, on the same device they built it for." Found live trying to
push the same dashboard face to a second board (2026-09-07): it needed a
*second*, manually-extracted device key and a *second* signed `.comp` -
not because the face was different, but because HMAC's own math requires
matching the target's secret, which a pack-time signature can't do for a
package meant to reach more than one device.

## 3. The fix: sign at push time, over the target's key

- **`atelier pack`** no longer signs at all. It produces a plain,
  unsigned `.comp` - manifest.json + bytecode entries only. This file is
  freely distributable: the same bytes work as the input to a push
  against any device, because nothing device-specific is baked in yet.
- **`atelier push --key <hex>`** does the signing, immediately before
  sending: it reads the unsigned `.comp`, computes
  `hmac-sha256(manifest.json + sorted bytecode entries, key)`, and wires
  a `sig` entry onto a fresh copy of the tar before POSTing. `--key` is
  the *target* device's own key - the same value `install_impl()` was
  always going to check the signature against, just supplied explicitly
  at the point that actually needs it instead of hidden inside a file
  built earlier, possibly by someone else, possibly for a different
  device entirely.
- **Pairing a new device**: `net_svc.c`'s sync screen now shows the
  device's HMAC key as a fourth line, alongside the WiFi SSID/password/IP
  it already showed. Reading that key off the screen once and passing it
  to `atelier push --key ...` *is* the pairing step - "the regular way
  atelier learns a new device" (project chat 2026-09-07), not a
  debug-only escape hatch. `kb_store_get_hmac_key_hex()` (`app_store.h`)
  is the public accessor this reads from; `get_hmac_key()` itself stays
  private to `app_store.c`.

`kb_store_install_default_face()` is the one exception, and stays
exactly as it was: it signs the embedded, unsigned default-face bytes
**on-device**, at first boot, with `get_hmac_key()`'s own output - there
is no separate "device to push to" for this one package, it was never
going through `atelier` in the first place, so the provenance problem
above never applied to it.

## 3a. A deliberate tradeoff, flagged rather than left silent

**The sync screen showing the device's key in plaintext is a real
exposure, stated here on purpose** (review round, project chat
2026-09-07 - same discipline js-api.md §1a already applies to its own
divergences, worth repeating here rather than letting this one pass
silently): anyone who can see the screen during sync mode can read the
install secret, same as they could already read the WiFi
SSID/password shown right above it. Acceptable for a hobby project's
threat model, not a property to forget about later - the mitigating
facts are that sync mode is **user-initiated** (a tap/button press, not
something that happens on its own) and **time-limited**
(`KALIBER_NET_SYNC_TIMEOUT_S`, the AP and the screen both go away on
their own), so the exposure window is short and requires physical
proximity, not an always-on broadcast. Worth revisiting if this project
ever moves past "read it off my own watch and type it into my own
laptop" - not designed further here.

## 4. Package format: forward-compatible on purpose

The old `sig.hmac` entry held a bare 64-hex-char signature and nothing
else - reasonable when HMAC was the only scheme that would ever exist,
wrong the moment a second one might. The new `sig` entry holds
**`<scheme>:<key-id>:<hex-signature>`**, e.g.
`hmac-sha256:default:9f86d0...`:

- `scheme` lets `install_impl()` dispatch instead of assuming HMAC -
  today there's exactly one branch (`hmac-sha256`), an unrecognized
  scheme is rejected cleanly with a clear log line, not silently treated
  as HMAC or silently accepted.
- `key-id` is unused for HMAC (`"default"`, always - a device has
  exactly one key, there is nothing to select between) but present and
  checked anyway. An asymmetric scheme with multiple trusted publisher
  keys will need this field to already exist in every package format out
  there, not retrofitted later - retrofitting would invalidate every
  already-published package's signature field, which changing the field
  *now*, while nothing has shipped externally yet, avoids paying for
  twice.

This is a **breaking format change** - packages signed under the old
`sig.hmac`/bare-hex format are rejected by the new `install_impl()` (no
`sig` entry, or one it can't parse as `scheme:keyid:hex`). Acceptable
now (nothing has shipped to real users yet, per project chat
2026-09-07's own framing: "besser jetzt zu sehen als in zehn
Zifferblättern," said about a different bug the same day, equally true
here) - not acceptable a second time, which is the whole reason for
building the scheme/key-id room now instead of only when a second scheme
actually exists.

## 5. Package provenance — concept only, not built

The harder problem this doc's fix does *not* solve: proving a package
genuinely came from a specific publisher, so a device (or its owner)
could decide whether to trust an unfamiliar source at all. HMAC cannot
do this (§1) - it needs an asymmetric scheme:

- The publisher signs with a **private** key (Ed25519 is the obvious
  choice - small keys/signatures, no patent history, already common in
  this kind of embedded-signing role).
- The device verifies with the matching **public** key, which it must
  already have - meaning devices need a small **trust store** (a list of
  known-good public keys, likely NVS-resident like the HMAC key already
  is) and a UI moment for "this package is signed by a publisher you
  haven't trusted yet - install anyway?" for the first-contact case.
- This is the actual mechanism a future app store or "install from a
  website" flow would need. Zepp OS's own model (referenced in project
  chat 2026-09-07) is the special case of this where there's exactly one
  trusted publisher key (Zepp's own) baked in, and anything outside that
  channel (sideloading) is a deliberately separate path with its own,
  looser rules - not a general answer, just the shape a single-store
  ecosystem needs.

Not designed further here - flagged so a future "who really built this"
requirement doesn't get bolted onto the device-permission HMAC check
above by mistake, the way the original design bolted "may install" and
"who built it" together without meaning to.
