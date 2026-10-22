# Shared-ring ownership review baseline

This snapshot builds on `30649abb` (`shared-ring-v3`). It establishes explicit
SRB/slot ownership and independent preparation, publication and completion
stages. It retains experiment switches for review before a later cleanup.

## Review map

- `src/sys/ioq.c`: authoritative SRB/slot state, intrusive free/ready/completed
  FIFOs, batch transitions, opaque generation tokens and logical-SRB FLUSH fence.
- `src/sys/stgunit.c`: preparation/completion execution, single SQ producer,
  single CQ consumer, transport failure/rundown and optional payload mappings.
- `src/sys/scsi.c`, `io.c`: request preparation/completion, chunk continuations,
  non-CDB FLUSH/SHUTDOWN and zero-count SYNCHRONIZE CACHE forwarding.
- `src/sys/driver.h`: ownership states, slot resources and execution switches.
- `inc/winspd/ioctl.h`, `src/shared/stgunit.c`: ring V5 payload references and
  userspace pointer resolution/validation.
- `tst/winspd-tests/ring-test.c`: integration tests using real Storport requests,
  deterministic ownership barriers and separate-process mapping requesters.

The IOQ spinlock governs queue membership, admission and SRB/slot state. Workers
claim groups under it, perform payload copying outside it, and commit or retire
groups. SRB completion occurs after unlocking. The existing producer alone
publishes SQ entries; KICK alone consumes CQ entries and queues completion work.
Generation-tagged slot leases prevent stale completions from reaching reused
SRBs; unexpected CQ metadata fails the running session.

FLUSH is a FIFO scheduling fence. Later SRBs remain accepted but cannot execute
past it. Active logical requests retain their count across chunk continuations;
a FLUSH waits for all preceding logical requests to retire. Published I/O is
not reclaimed by an ordinary abort. Reset terminalizes the ring session.

STOP closes claiming/publication and drains kernel activity. Physical cleanup
of mapped published SRBs waits for cooperative backend rundown and CLOSE, or
owner-process termination. Kernel work retains the storage-unit lifetime.

## Defaults and experiments

MSBuild properties in `build/VStudio/winspd_sys.vcxproj` select implementations:

| Property | Default | Purpose |
| --- | ---: | --- |
| `RingPreparationThread` / `RingCompletionThread` | 0 / 0 | Work items; optional dedicated thread implementations remain available |
| `RingPreparationClaimLimit` | 32 | Maximum preparation claim batch |
| `RingCompletionClaimLimit` | 32 | Maximum completion claim batch |
| `RingReadyCommitLimit` / `RingReadyCopyLimit` | 8 / 262144 | Independent ready commit grouping |
| `RingPreparationTimeUs` | 100 | Soft budget; owned batches finish before yielding |
| `IoqQueuedSpinLock` | 0 | Ordinary IOQ spinlock; queued-lock experiment retained |
| `RingThreadPriority` | 8 | Optional thread mode also permits priority 12 |
| `RingLatencyDiagnostics` | 0 | Extended stage timing diagnostics |
| `RingMapThresholdBytes` | 0 | Mapped SRB payloads disabled by default |
| `RingMapOriginDiagnostics` | 0 | Optional source-MDL classification |

Ring V5 is required even with mapping disabled; use matching kernel/userspace
components. A payload reference contains a buffer ID, length and optional
backend-process user address. Zero address retains the preallocated copy path.
The legacy IOCTL transaction ABI remains available.

Optional mapping selects by logical SRB size. Eligible original MDLs describe
user ranges from a known requester other than the backend. Mapping/unmapping
uses private partial MDLs and short backend-process attachment in worker
context. Original MDLs/page locks remain borrowed until final SRB completion.
WRITE aliases are read-only; READ aliases permit backend writes. Slot buffers
are not shrunk, and unsuitable requests fall back to copying.

**Remaining limitation:** validating a CQ token or unmapping a user alias cannot
prove another I/O stack has released physical-page references acquired through
that alias. A dishonest premature CQ/CLOSE assertion remains outside the
prototype's lifetime guarantees. Mapping stays opt-in; crash/termination tests
and benchmark readiness do not establish malicious-backend isolation or
production readiness.

## Validation and companion project

The mapping-enabled Debug configuration passed 76 parent integration tests and
zero-count FLUSH checks. Release mapping variants passed 21 ring tests plus
FLUSH checks. Separate-process tests cover data integrity, read-only mappings,
multichunk continuations, cancellation at worker/list boundaries, STOP/reset,
backend termination and requester termination. The test cancellation control
shares post-lock completion/preparation wakeups with real Storport aborts.

A fresh review build with both mapping and extra diagnostics disabled also
passed all 76 parent tests and FLUSH checks. This validates the shipped default
selection separately from the mapping-enabled experiments.

Real WslPart/fio verification and stress retired 822,312 mapped leases, followed
by 901,497 leases in the QD32 cutoff measurements. All were released. QD32
sequential measurements were competitive with the saved VHDX baseline; the
comparison is historical and provisional, with mixed CPU efficiency. Every
mapped payload observed in that fio profile was 256 KiB; this is not a general
mapping minimum.

The companion WslPart review branch contains the implementation plan, detailed
validation checkpoints, benchmark configuration, raw-derived JSON/CSV reports
and limitations:

- [Reviewer entry point](https://github.com/edbordin/wslpart/blob/shared-ring-ownership-baseline-review/notes/winspd-baseline-review.md)
- [QD32 cutoff results](https://github.com/edbordin/wslpart/blob/shared-ring-ownership-baseline-review/notes/benchmarks/winspd-mapping-threshold-qd32.md)

Generated drivers, certificates/private keys, capture binaries, local shortcuts
and fio state files are not part of this snapshot.
