# SVMS Versioning, Updates, Native API, and KDMAPI Compatibility

Status: proposed design. The ABI described here is not frozen yet.

This document defines how the V3 driver, configurator, native SVMS API, and
legacy KDMAPI compatibility layer should coexist. The goal is to let each
component evolve independently without breaking old applications or making the
configurator a runtime dependency.

## Names and Components

The preferred public name is **SVMS API** (SuperVirtualMIDISynth API). Earlier
discussion called it SVMAPI or SMVS API; those names refer to the same proposal.

The intended deliverables are:

- `winmm.dll`: the existing drop-in Windows multimedia MIDI driver.
- `SVMSConfigurator.exe`: the optional configuration and diagnostics program.
- `SVMSAPI.dll`: the modern native API for new and maintained applications.
- `svmsapi.h`: the stable public C header for `SVMSAPI.dll`.
- `OmniMIDI.dll`: an optional KDMAPI-compatible facade backed by SVMS. It must
  be described clearly as an SVMS compatibility component, not as the actual
  OmniMIDI product.
- `libsvmsapi.so`: the eventual Linux native API implementation.
- `svms_core`: the internal engine shared by the driver, native API, offline
  renderer, compatibility facade, and Linux implementation.

Neither `SVMSAPI.dll` nor the KDMAPI facade may depend on the configurator. The
driver must also remain fully usable without the configurator.

## Version Vocabulary

Four different values are required because they answer different questions:

1. **Product version**, such as `0.7.0`, is the user-facing release version.
2. **Build number**, such as `184`, identifies one exact official build.
3. **Protocol version** identifies an IPC layout such as RuntimeLink V2 or V3.
4. **API ABI version** identifies one immutable native API function table.

Product and build numbers inform the user. Protocol versions protect shared
memory layouts. API ABI versions protect application binary compatibility.
Feature availability must be determined through capability flags rather than
by guessing from a build number.

### Shared Build Metadata

The driver and configurator in one release must consume the same generated
build metadata. It should contain at least:

- product major, minor, and patch version;
- monotonic official build number;
- source commit identifier;
- release channel (`stable`, `prerelease`, or local development);
- supported RuntimeLink protocol range;
- native API ABI range;
- component capability bits.

Official CI assigns monotonic build numbers. Local builds should use build
number zero plus the source commit identifier. Git commit counts are unsuitable
because rebases and history cleanup can change them.

The same information should be written to Windows version resources so DLL and
EXE properties remain useful without running either component.

## Configurator and Driver Compatibility

The configurator compares both build metadata and negotiated capabilities when
it connects to a running driver.

### Newer Driver, Older Configurator

The configurator should show a non-blocking notice that it is older than the
driver. It continues using the newest mutually supported RuntimeLink protocol
and exposes only capabilities it understands. Unknown fields and capabilities
must be ignored safely.

An already-released configurator cannot learn future protocol layouts
retroactively. Therefore, the first release using this design must include a
small, stable discovery header that future drivers continue publishing.

### Newer Configurator, Older Driver

The configurator should explain that the driver is older and that some new
features may be unavailable. Unsupported controls are disabled or marked as
requiring a newer driver. Supported controls continue working normally.

### No Compatible Live Protocol

If the components share no writable RuntimeLink protocol, the configurator may
show legacy/read-only discovery information but must not write live state. It
can still edit a configuration schema it understands, subject to the schema
rules below.

### RuntimeLink Evolution

RuntimeLink V2 is already deployed and must not be silently reinterpreted.
RuntimeLink V3 should be introduced alongside V2, with the driver publishing
both during a compatibility period.

RuntimeLink V3 should provide a fixed discovery header containing:

- magic and total layout size;
- protocol version and minimum compatible version;
- driver product/build information;
- capability flags;
- offsets and sizes for optional sections;
- heartbeat and process identity;
- read/write permissions for each section.

Readers must validate sizes and offsets before accessing optional sections.
New fields are appended or placed in separately sized sections; existing fields
never change meaning.

### Configuration Schema Compatibility

The JSON schema version is independent of the driver build and RuntimeLink
version. Unknown JSON fields are preserved.

An older configurator opening a newer unsupported schema must use read-only
mode unless it can prove that a particular edit is safe. It must never rewrite
the file into an older schema or discard fields it does not recognize. Driver
and configurator upgrades must not be required merely to keep using an existing
configuration.

## Update Checking and Installation

Update checking belongs only in the configurator. It must be optional,
asynchronous, cached, and never block startup or audio playback. Updates are
never forced.

When either component is older, the configurator may offer to check the latest
GitHub release. Stable and prerelease channels should remain distinct.

### Release Manifest

Each release should publish a signed machine-readable manifest containing:

- product version and build number;
- minimum supported OS and architecture;
- per-component asset names and URLs;
- file sizes and SHA-256 hashes;
- supported protocol and native ABI ranges;
- release channel and release-notes URL.

The configurator must verify the manifest signature and downloaded asset hash
before replacing anything.

### Transactional Configurator Update

The updater should:

1. Download to a uniquely named temporary file.
2. Verify the signature, architecture, and hash.
3. Launch a small temporary updater process.
4. Let the running configurator exit.
5. Move the old executable to a recoverable backup.
6. Atomically place the verified replacement where the old executable lived.
7. Relaunch it and roll back if replacement or startup fails.

If any step fails, the user is told that the update failed and the original
installation remains usable.

### Driver Update Safety

Replacing a loaded `winmm.dll` is more delicate than replacing the
configurator. The updater must identify the exact driver instance selected by
the user, stage the replacement, and wait until every host process using that
DLL has exited. It must never search for and replace arbitrary files named
`winmm.dll`.

## Native SVMS API

The native API is the preferred integration surface for new software. It should
be simpler and faster than WinMM or KDMAPI while retaining SVMS's exact timing.

### ABI Rules

The public interface is a C ABI. It must not expose C++ classes, STL types,
compiler-specific exceptions, or ownership that depends on one C runtime.

Public types use:

- fixed-width integers;
- UTF-8 strings unless a platform-specific entry point explicitly says
  otherwise;
- opaque 64-bit handles;
- stable numeric error codes;
- caller-owned structures with explicit sizes;
- reserved fields that callers initialize to zero;
- capability flags for optional behavior.

Every extensible structure begins with `struct_size` and `struct_version`.
Implementations read only the bytes provided by the caller and callers read
only the bytes reported by the implementation.

The DLL should expose one permanent bootstrap symbol conceptually equivalent
to:

```c
SVMS_Result SVMS_GetInterface(
    uint32_t requested_abi,
    uint32_t caller_table_size,
    SVMS_Interface* out_interface);
```

The returned function table is append-only within an ABI version. A future
runtime must continue returning older interface tables when requested.

### Backward and Forward Compatibility

- An application built for API 0.1 must continue working with a 4.5 runtime.
- A newer application running against an older runtime requests the newest ABI
  the runtime supports, checks capabilities, and falls back gracefully.
- New functionality cannot exist inside an old runtime; forward compatibility
  means predictable degradation, not pretending unsupported features work.
- Removing or changing the meaning of an existing function requires a new ABI,
  while adding optional behavior normally requires only a capability bit and an
  appended function pointer or sized structure.

Cross-version ABI tests should load every retained interface version and run a
small behavioral smoke test against it.

### Sessions

Unlike KDMAPI's process-global stream, the native API should support explicit
sessions. A process may create independent real-time synth, offline render, or
analysis sessions with separate SoundFonts and MIDI state.

The minimum useful API includes:

- runtime and capability discovery;
- session creation and destruction;
- audio backend selection and lifecycle;
- SoundFont loading and activation;
- reset and panic commands;
- short MIDI, SysEx, and bulk event submission;
- configuration and read-only telemetry queries;
- explicit buffer ownership and cancellation rules.

### Exact-Timing Bulk Submission

Bulk submission is the primary performance advantage over KDMAPI. One call can
submit many compact events while every event retains its own timestamp and
sequence position.

Supported timestamp domains should include:

- immediate/next writable frame;
- absolute output frame;
- QPC on Windows;
- monotonic nanoseconds for portable clients.

Batching is only call and queue amortization. It must never quantize all events
to the batch boundary. Equal-frame events retain submission order, and events
with different timestamps retain their exact output-frame placement.

The API should expose both strict-lossless and priority-aware ingress modes,
cancellable backpressure, queue capacity information, and queue-pressure
telemetry. SysEx submission must define whether data is copied immediately or
held until a completion notification.

## KDMAPI-Compatible Replacement

The KDMAPI facade exists for old or unmaintained applications that cannot be
changed to use the native API. It should be a drop-in binary replacement, not a
second synthesizer implementation.

```text
Modern application -> SVMSAPI.dll -------------------+
                                                     |
Legacy application -> KDMAPI-compatible OmniMIDI.dll +-> svms_core
                                                     |
WinMM application -> winmm.dll ----------------------+ 
```

The facade exports the expected KDMAPI symbols using the exact calling
conventions, parameter widths, symbol names, return values, and x86/x64 ABI.
The final export list must be produced from an explicit compatibility inventory
and tested against real KDMAPI clients. XP receives its own x86-compatible
binary.

### Legacy Stream Mapping

KDMAPI's global behavior maps to one hidden process-global SVMS session:

- stream initialization creates or references the default session;
- stream termination decrements the reference and performs orderly shutdown;
- direct short-message calls translate to compact native events;
- direct long-message calls submit SysEx with compatible buffer semantics;
- reset becomes an ordered timestamped engine command;
- repeated initialization, reset, and termination follow observed KDMAPI
  behavior rather than imposing the native session model;
- unsupported OmniMIDI-specific settings return compatible defaults or a clear
  failure without corrupting the session.

The facade must not open the configurator, contact the network, or trigger an
update.

### Compatibility Versus Performance

The facade can make each KDMAPI call very cheap by translating directly into
the common event pipeline. However, an application issuing millions of
individual calls still pays millions of DLL-call boundaries. That cost cannot
be removed without changing the caller.

Maintained applications should therefore migrate to native bulk submission.
Unmaintained applications receive the fastest compatible implementation that
requires no source changes.

### Optional SVMS Discovery Extensions

The compatibility DLL may export uniquely named, optional symbols such as
`SVMS_QueryCompatibilityInfo`. Old applications ignore them. A maintained
application can detect that its KDMAPI provider is SVMS, negotiate capabilities,
and switch to native bulk submission without mistaking the facade for another
provider.

These extensions must never alter the behavior or layout of existing KDMAPI
exports.

### Branding and Distribution

The binary filename may need to be `OmniMIDI.dll` because legacy applications
load that exact name. Documentation, file metadata, and diagnostics must state
that it is an SVMS KDMAPI compatibility facade. No OmniMIDI artwork, product
identity, or implication of official affiliation should be used.

## Optional API Discovery in the Configurator

Native API runtimes may publish a small read-only discovery record containing:

- process and session identity;
- runtime product/build version;
- requested and negotiated API ABI;
- capability flags;
- basic health information.

The configurator may display a quiet notice when an application uses a very old
ABI. It must not update, reconfigure, interrupt, or take ownership of that API
session. Discovery must be part of the first public ABI if old runtimes are to
remain observable by future configurators.

## Implementation Sequence

Current implementation status (2026-08-23):

1. [x] Generate shared product/build metadata for all binaries.
2. [x] Add correct Windows version resources and expose build information in the
   diagnostic UI.
3. [x] Define the stable discovery header and dual-publish RuntimeLink V2 and V3.
4. [x] Make configurator controls capability-driven and add mismatch notices.
5. [x] Add optional, read-only GitHub release checking.
6. [ ] Add the verified transactional updater and rollback path. This is
   intentionally blocked until releases provide a pinned signing identity and
   signed manifests; the unsigned replacement path was rejected.
7. [x] Formalize the shared runtime: all Windows frontend names are
   byte-identical aliases over one engine and ownership model. A deeper portable
   core extraction remains useful, but is not required for frontend parity.
8. [x] Freeze the first `svmsapi.h` ABI and implement `SVMS_GetInterface`.
9. [x] Implement native sessions, exact single-event submission, bulk submission,
   SysEx, and telemetry.
10. [x] Inventory and implement the KDMAPI-compatible export surface as a thin
    facade over a hidden native session.
11. [x] Add C/C++ ABI checks and x86, x64, and XP x86 compatibility tests.
    Linux native-API publication remains future work.
12. [x] Retain RuntimeLink V2, existing KDMAPI exports, and compatibility binary
    names in automated tests before expanding either API.

These should land as independently testable commits. The versioning and
discovery foundation should precede the updater and public ABI freeze.

## Non-Goals and Invariants

- The configurator is never required for playback or API use.
- No update is forced and no audio component checks the network.
- Build-number mismatches never disable otherwise compatible functionality.
- Capability checks, not version guesses, control optional features.
- Old ABI layouts and KDMAPI exports are never repurposed.
- Bulk submission never reduces timestamp precision.
- The KDMAPI facade and native API never develop separate synthesis behavior.
- Unsupported features fail predictably without damaging configuration or
  replacing working binaries.

The guiding rule is: **build numbers inform, capabilities select behavior, and
ABI versions protect layouts**.
