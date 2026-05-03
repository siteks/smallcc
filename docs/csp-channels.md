# CSP-Style Channels — Research and Design Sketch (not implemented)

**Status:** research / scoping. No code or RTL change yet. This document
surveys the design space, proposes a minimal ISA + compiler + hardware
delta to add single-buffer CSP-style channels to CPU4, and lists the
open questions.

## Problem statement

The CPU4 hardware (`../cpu4_hardware`) is an 8-context barrel processor:
8 hardware threads share an 8-stage pipeline with one context per stage.
Each context already has its own PC, SP, BP, LR, halt flag, and 8 GPRs;
contexts are functionally independent processes that happen to share BRAM
and the data port.

Today, contexts can only communicate through shared memory and ad-hoc
busy-wait. We want first-class **CSP-style channels** as a small set of
ISA primitives, compiler intrinsics, and hardware blocks, supporting:

1. **Intra-CPU**: channels between contexts on the same CPU instance.
2. **Inter-CPU**: channels between contexts on different CPU instances
   (different cores in a single FPGA, or across FPGAs over a serial link).
3. **Single-buffer semantics**: each channel has exactly one slot. Send
   blocks if full; receive blocks if empty. Symmetric dual to pure
   rendezvous, identical to `make(chan T, 1)` in Go.

Goals:

- **Minimal ISA delta** — ideally 3–4 new instructions.
- **No new C language syntax** — channels exposed via compiler intrinsics
  and a small header, the same way XMOS exposes thread/timer primitives.
- **Cheap blocking** — blocking a context costs nothing in throughput
  because the barrel just does not issue its slot (same mechanism the
  divider already uses for context stalls).
- **Extensible to inter-CPU** without re-revving the ISA — a channel ID
  encoding that can route locally or remotely with no instruction change.

Non-goals (deferred):

- Multi-element FIFO channels. (Single-buffer covers most needs;
  bulk transfer can be built on top via shared SDRAM + a single
  doorbell-style channel.)
- Typed channels at the language level. (Channels carry one 32-bit word;
  type discipline is the user's responsibility, helped by header macros.)
- Full XMOS-style `select` with priority and guards. (We sketch a path
  but recommend deferring; non-blocking probe + software loop covers it.)

---

## Reference designs surveyed

### XMOS xC / xCORE (closest match)

The xCORE family is essentially the modern industrial successor to occam
on the Transputer, and is the canonical reference for hardware-supported
CSP channels. Key facts:

- xC declares channels as first-class types: `chan c;` allocates two
  `chanend`s; `c <: expr;` sends, `c :> var;` receives. Both are
  rendezvous (zero-buffer) by default; `streaming chan` is FIFO-buffered.
- `chanend` is a hardware resource: each tile has a fixed number of
  hardware channel ends, allocated at compile/link time. Channels route
  through the **xCONNECT switch fabric** for inter-tile/inter-chip
  communication using packets headed by the destination chanend ID.
- The XS1/XS2 ISA has dedicated instructions: `OUT`, `IN`, `OUTT`, `INT`,
  `OUTCT`, `INCT`, `CHKCT`, `TESTCT`, `SETD`, `GETR`, `FREER`. Data
  tokens (`OUT`/`IN`) and control tokens (`OUTCT`/`INCT`) share the same
  link but are distinguished by a tag bit.
- `select { case c1 :> v: stmt; case c2 <: x: stmt; }` is non-deterministic
  choice, supported by hardware: a thread can wait on a set of resources
  and the scheduler wakes it when any becomes ready.
- Threads (xC `par`) are scheduled onto the 8 hardware contexts of an
  xCORE tile, one C-level task per context. CPU4's barrel is structurally
  identical to an xCORE tile in this respect.

The closest CPU4 analogue is to copy the xC operation set in spirit
(`OUT`, `IN`, non-blocking probe, eventual `select`), but to keep the
encoding compact and to specialise to single-buffer-per-channel rather
than the richer XS1 token-stream model.

### Transputer / occam (the original)

The Inmos Transputer (1985) was the original hardware-CSP machine. occam
channels were rendezvous, untyped 1-byte tokens by default. Each channel
was a single word in memory, encoded as either "no waiter" (NULL),
"sender waiting" (process descriptor pointing into the sender's
workspace), or "receiver waiting". The microcoded `in`/`out`
instructions did atomic check-and-update on this word; if the partner
was waiting, the data was copied directly between workspaces. This is
exactly the state machine we want, but adapted to single-buffer (so the
slot can hold data even when neither side is waiting).

### Go (software channels, useful semantics reference)

Go's `make(chan T, 1)` is the **canonical single-buffer channel** and
captures the semantics we want exactly. The runtime uses a mutex + two
goroutine queues per channel; this is an implementation we are
hardware-translating. Notable: Go's `select` over multiple channels is
the same primitive as XMOS `select`; the design recommendation here is
to omit it in v1 and provide non-blocking probe instead.

### Tilera / MPPA / Adapteva (NoC-based mailboxes)

These systems use packet-routed network-on-chip with per-core mailbox
registers. The interesting takeaway is the **chanend ID encoding**:
splitting the ID into `[node][core][port]` lets the same instruction
target a local or remote endpoint, with hardware routing chosen by the
high bits. Worth borrowing for CPU4's chid encoding so inter-CPU is a
hardware-only extension.

### Cray MTA / Tera (full/empty bits)

Less directly relevant, but worth flagging: the MTA had a full/empty bit
on every memory word, with load-and-clear / store-when-empty semantics.
This is a generalisation of single-buffer channels to "every word is a
channel". Probably overkill for CPU4 but informs the per-channel state
representation.

---

## Channel semantics — single-buffer

Each channel has a small state:

| Field | Bits | Meaning |
|---|---|---|
| `data` | 32 | the buffered word |
| `full` | 1 | data is valid |
| `waiter_present` | 1 | a context is blocked on this channel |
| `waiter_dir` | 1 | 0 = blocked sender, 1 = blocked receiver |
| `waiter_ctx` | 3 | which of the 8 contexts is blocked |
| `waiter_reg` | 3 | which GPR holds the data (sender) or receives it (receiver) |

Total: 41 bits per channel. With 64 channels, ~328 bytes — trivial in
ECP5 distributed RAM or a single DP16KD slice.

**Invariant:** at most one context is blocked per channel. A blocked
sender can only exist when `full=1`; a blocked receiver can only exist
when `full=0`. Both directions blocked simultaneously is impossible
(would have rendezvoused).

State transitions:

```
send(data, ctx, src_reg):
  full=0, no waiter            → store data, full=1                              (return)
  full=1, no waiter            → block: waiter=(ctx, src_reg, OUT)                (slot stalls)
  full=0, waiter=(c2, dr, IN)  → wake c2, write data → c2.r[dr], state cleared    (return)
  full=1, waiter=(c2, sr, OUT) → impossible (invariant)

recv(ctx, dst_reg):
  full=1, no waiter            → load data, full=0                                (return)
  full=0, no waiter            → block: waiter=(ctx, dst_reg, IN)                  (slot stalls)
  full=1, waiter=(c2, sr, OUT) → load data, take c2.r[sr] into buffer, full=1,
                                 wake c2, state cleared (or buffer stays full
                                 with c2's data as the new buffered value)         (return)
  full=0, waiter=(c2, dr, IN)  → impossible (invariant)
```

The "second sender on full" case has a subtle choice: when a receiver
arrives and finds both `full=1` and a waiting sender, does the receiver
take the buffered data and the waiter then deposits its data into the
buffer, or does the receiver take the waiter's data and the buffer
drains to empty? Both are correct CSP-buffered semantics; the **hand-off
in FIFO order** (receiver takes buffer, waiter deposits) is the natural
choice and matches Go's runtime.

**Blocking mechanism (CPU4 barrel):** when a context blocks, the channel
unit raises a per-context `block[ctx]` signal. The pipeline's D stage
checks `block[ctx]` and turns the slot into a NOP without advancing PC,
exactly the way the divider stalls a context (`d_div_stall` →
`slot_restarted`). On wake-up, `block[ctx]` is cleared and the context
retries on its next 8-cycle rotation, picking up the just-written
register and the post-channel PC. The barrel structure means the wake
latency is at most 8 cycles, with no flush.

---

## Proposed ISA additions

The minimum operation set is:

1. **`chsend rs, chid`** — send `rs` to channel `chid`. Block on full.
2. **`chrecv rd, chid`** — receive from channel `chid` into `rd`. Block on empty.
3. **`chprobe rd, chid`** — non-blocking. Write a 2-bit status to `rd`:
   `0=empty`, `1=full`, `2=peer-waiting-send`, `3=peer-waiting-recv`.
   Lets software implement a select-style poll loop.
4. **(optional, v2)** **`chsel rd, mask`** — block until any channel in
   the mask becomes ready; write the chid that woke us into `rd`. This
   is the XMOS `select` analogue and removes the busy-wait penalty of
   software-emulated select. Recommend deferring to v2.

Additional control:

5. **(optional)** **`chsendr rs_data, rs_chid`** / **`chrecvr rd, rs_chid`** —
   register-indirect channel ID, for arrays of channels. Likely needed
   for any non-trivial use; can fold via the assembler if reg form is
   provided.

### Encoding analysis

Given the slot inventory in `docs/isa/cpu4.md`:

| Format | Free slots | Best fit for |
|---|---|---|
| F0a (1B, no operand) | 6 | — |
| F0b (3B, 2 reg + imm9) | 8 | reg-indirect chid with imm9 offset |
| F1a (2B, 3 reg) | 4 | reg-indirect chid (3rd reg unused) |
| F1b (2B, 1 reg + 6b subop) | 49 | — (no second operand field) |
| **F2  (2B, 1 reg + imm7)** | **3** | **immediate chid, common case** |
| F3c (3B, 2 reg + imm10) | 1 | — |
| F3d (3B, 1 reg + imm10) | 6 | reg + 10-bit chid (ample room) |

Recommended allocation:

- **`chsend rs, chid_imm7`** — F2, one of the 3 free slots. 2-byte op.
- **`chrecv rd, chid_imm7`** — F2, second free slot. 2-byte op.
- **`chprobe rd, chid_imm7`** — F2, third free slot. 2-byte op.
- **`chsendr rs_data, rs_chid`** / **`chrecvr rd, rs_chid`** — F1a slots
  (4 free), 2-byte ops; the third register field is don't-care.
- **`chsel rd, mask_imm10`** (v2) — F3d, one of the 6 free slots. 3-byte op.

This consumes all 3 F2 slots — F2 is the format with the tightest
budget, so we should be sure these are the right ops to put there. The
alternative is to use F3d (one-reg + imm10) for the immediate-chid
forms, which gives 10-bit chids (1024 channels) at 3 bytes per op
instead of 2 bytes per op for 128 channels. Recommend F2 for the local
case (128 channels per CPU is plenty) and F3d for `chsel`.

Encoding sketch:

```
chsend  rx, imm7   :  10 oooo xxx iiiiiii
chrecv  rx, imm7   :  10 oooo xxx iiiiiii
chprobe rx, imm7   :  10 oooo xxx iiiiiii
chsendr rx, ry     :  01 ooooo --- xxx yyy   (F1a, dst field unused)
chrecvr rx, ry     :  01 ooooo xxx --- yyy   (F1a)
chsel   rx, imm10  :  11011111 xxx ooo iiiiiiiiii  (F3d)
```

Total cost: 3 F2 slots (out of 3 free), 2 F1a slots (out of 4), 1 F3d
slot (out of 6). All free encoding space remains plentiful for future
extensions.

### Channel ID space

7-bit immediate chids (F2) → 128 channels per CPU. For inter-CPU,
**reserve the high bit of chid as a routing hint**:

```
chid[6]   = 0 → local channel
chid[6]   = 1 → remote channel; chid[5:3] = remote node, chid[2:0] = remote-local-chid
```

This gives 64 local channels + 8 remote nodes × 8 remote channels each.
Software writes the chid as a normal immediate; hardware decodes the high
bit and routes to the local channel unit or to the inter-CPU link. **No
instruction change is needed to add inter-CPU later** — only the
hardware behind the chsend/chrecv unit changes.

For the 10-bit chsel mask, the 64-local-channel encoding fits with room
to spare (one bit per channel = 64 bits of mask, which won't fit in 10
bits — `chsel` would need either a smaller addressable subset or an
auxiliary mask register written via a control-register move).

---

## Hardware sketch

A new `chan_unit.v` module in `../cpu4_hardware/rtl/`:

```
chan_unit
  inputs from CPU pipeline (X3-ish stage, where stores already drive):
    op            (chsend / chrecv / chprobe / chsendr / chrecvr / chsel)
    ctx           (3 bits — issuing context)
    chid          (7 bits)
    src_reg_idx   (3 bits — for chsend, which reg to read on wake)
    dst_reg_idx   (3 bits — for chrecv, which reg to write on completion)
    src_data      (32 bits — for chsend, value to deposit if buffer empty)

  outputs to pipeline:
    block[ctx]    (1 bit per context — when high, D stalls the slot)
    wake_ctx      (3 bits — which ctx to wake this cycle)
    wake_reg      (3 bits — which reg to write)
    wake_data     (32 bits)
    wake_valid    (1 bit)
    probe_result  (2 bits — for chprobe)

  storage:
    channel_state[63:0]  (41 bits each — see table above)
```

**Pipeline integration**: a `chsend` or `chrecv` decodes in D, reads its
source register in X1 (just like a store), and presents the request to
`chan_unit` at X3 (where stores already drive the data port). The unit
combinationally produces either:

- "no block, completing": writes `dst_reg_idx` directly via a regfile
  write port at W (chrecv hit, chprobe always); chsend hit needs no
  writeback.
- "blocking": asserts `block[ctx]`, latches the request into the
  channel's waiter slot. The pipeline's W stage does not write
  anything for this instruction; the context's PC stays put (just like
  a divider stall).

When a peer's send/recv unblocks a waiter, `chan_unit` raises
`wake_valid` for one cycle, supplying the data + reg + ctx. Because the
barrel rotates through contexts every cycle, the woken context is
guaranteed to land in W within 8 cycles, so the wake path can register
the writeback into a small per-context "deferred write" latch that is
applied on the woken context's next W stage. The PC bump happens at the
same time.

**Cost estimate (ECP5):**
- 64 channels × 41 bits = ~328 bytes of state. Fits in 2× DP16KD
  (or distributed RAM if access pattern allows).
- 8 deferred-write latches × 38 bits (3 reg + 3 ctx + 32 data) = 304
  bits in flops.
- A small FSM (or pure combinational table) for the 4-state transition.
- Routing into D's stall logic and W's writeback mux.

This is small relative to the existing CPU. The dominant new path is
the 32-bit data hand-off mux into the regfile write port; should not
become a critical path because the woken context only writes on its
next scheduled W, not the same cycle as the wake event.

### Inter-CPU extension

Two natural paths:

1. **Shared SDRAM with hardware mailboxes.** Reserve a region of SDRAM
   (or BRAM) shared between CPU instances. Each remote channel maps to
   a control word in this region with `full / waiter_ctx / waiter_reg /
   waiter_dir / data`. A small per-CPU "remote channel proxy" reads
   the shared word atomically and updates it via the pbus arbiter. The
   pbus is 32-bit and has explicit arbitration (see
   `cpu4_hardware/docs/pbus_architecture.md`), so a CAS-style update is
   feasible with a couple of new bus commands.
2. **Dedicated link.** LVDS or serial bytes between FPGAs (or between
   CPU tiles in a multi-tile FPGA design), carrying packets like
   `[src_node][dst_node][dst_chid][type=send|ack|recv|data][32-bit data]`.
   The chan_unit at each end decodes the packet and updates its local
   channel state.

For first cut, option 1 is much simpler because the pbus already
exists. Option 2 is the long-term answer for off-chip links.

Either way, **the ISA does not change**; only the routing logic behind
the high chid bit changes.

---

## Compiler integration

### Proposed builtins

The compiler exposes the channel ops as builtins, recognised in the
parser the same way `va_start`/`va_arg` are today. No new C syntax.

```c
// include/chan.h  (proposed)

// Channel IDs are small unsigned integers (0..127 local).
typedef unsigned int chid_t;

// Send a 32-bit word; block until accepted.
void __builtin_chan_send(chid_t chid, int value);

// Receive a 32-bit word; block until available.
int  __builtin_chan_recv(chid_t chid);

// Non-blocking status.
//   0 = empty, 1 = full, 2 = peer waiting to send, 3 = peer waiting to recv
int  __builtin_chan_probe(chid_t chid);

// (v2) Block until any channel in mask is ready; return its chid.
chid_t __builtin_chan_select(unsigned int mask);

// Convenience macros (since the buffer is one register-width word,
// pointers and ints both fit; user types must be ≤ 4 bytes).
#define chan_send(c, v)  __builtin_chan_send((c), (int)(v))
#define chan_recv(c)     __builtin_chan_recv(c)
#define chan_probe(c)    __builtin_chan_probe(c)
```

### Recognition + lowering

In `parser.c`'s primary-expression handler, treat
`__builtin_chan_send`/`recv`/`probe` like `va_start` etc.: a special
node kind (`ND_CHAN_SEND`, `ND_CHAN_RECV`, `ND_CHAN_PROBE`).

In `braun.c`, lower these to new IR opcodes:

- `IK_CHSEND chid_op, data_op` (no result)
- `IK_CHRECV chid_op` (result = received value)
- `IK_CHPROBE chid_op` (result = status)

Each opcode carries the chid as either an immediate (when the source
expression is a constant) or an operand (when dynamic). At emission,
emit `chsend`/`chrecv`/`chprobe` (F2) for the immediate case or
`chsendr`/`chrecvr` (F1a) plus a probe-via-register variant for the
register case.

These opcodes are **side-effecting** — they cannot be CSE'd, hoisted,
or eliminated. Mark them in the optimization pass infrastructure with
the same flags used for `IK_LOAD`/`IK_STORE` plus a "blocking" flag
that prevents speculative reordering across them. (The pipeline's
single-issue-per-context property means in practice this is just "do
not move other side-effecting ops past a chan op", same rule as
volatile loads.)

For register allocation: treat `IK_CHSEND` like a store that uses a
fixed register (since the data must end up in the GPR field encoded by
the instruction), and `IK_CHRECV` like a load that defines a
freely-allocatable result. No new ABI constraints.

### Optimisation pass impact

- **R2D copy_prop**: chan ops have side effects; do not propagate
  through them. Already handled by `is_pure` checks.
- **R2E CSE**: never CSE a chan op (always treated as having unique
  side effects, like LOAD).
- **R2F LICM**: never hoist a chan op out of a loop.
- **R2I jump threading**: chan-op blocks are not "thin" — preserve
  them.
- **Inlining**: functions containing chan ops can be inlined; no
  special handling needed.

### Header sketch

A small `include/chan.h` with the builtins above plus a few helpers
for typed channels:

```c
// Typed channel macros — pure compile-time decoration.
#define CHAN_INT(id)    (id)
#define CHAN_PTR(id)    (id)

// Static channel ID assignment helpers — application allocates.
#define CHAN_LOCAL(n)   (n)         // 0..63
#define CHAN_REMOTE(node, port)   (0x40 | ((node) << 3) | (port))
```

No language-level channel typing in v1 — the user is responsible for
matching types between sender and receiver. This is the same pragmatic
trade-off that the variadic-function builtins already make.

---

## Process model — how do contexts get launched?

CPU4 contexts are statically scheduled — at reset, all 8 contexts begin
execution at the reset vector. To run different C functions on
different contexts, today you would:

1. Have `main()` read the context ID (would need a new MMIO register
   or a new instruction `getctx rd`) and dispatch via switch.
2. Set up per-context stack pointers (each context's SP must point into
   a private stack region in the 64 KB BRAM).

Neither is in place today. Recommended additions alongside the channel
work:

- **`getctx rd`** — F1b single-register subop, 2 bytes. Reads the
  current context's 3-bit ID into `rd`. Trivial in hardware.
- **A C runtime entry point** that reads the context ID and dispatches
  to `main_ctxN()` if defined, falling back to `main()` for context 0
  and `halt` for others.
- **Per-context stack carve-out** in `lib/crt0_cpu4.s`: divide the
  stack region into 8 sub-stacks, each context gets one.

These are independent of channels but make the channel work practically
useful. Without per-context dispatch, all channels would have only one
participant (which is pointless).

---

## Concrete example

Producer-consumer between two contexts:

```c
#include <chan.h>

#define CHAN_PIPE   CHAN_INT(0)

void main_ctx0(void) {                   // producer (context 0)
    int i;
    for (i = 0; i < 100; i++) {
        chan_send(CHAN_PIPE, i * 2);
    }
    chan_send(CHAN_PIPE, -1);            // sentinel
}

void main_ctx1(void) {                   // consumer (context 1)
    int v;
    int sum = 0;
    while ((v = chan_recv(CHAN_PIPE)) >= 0) {
        sum += v;
    }
    putchar(sum & 0xff);
}
```

Generated code per send (assuming `CHAN_PIPE = 0`, value already in r3):

```
chsend r3, 0      ; one 2-byte F2 instruction
```

That is the entire ISA-level cost of a send. If the buffer is empty,
this instruction completes in one slot rotation (8 cycles, the same as
any other op). If full, the context's slot stalls until the consumer
drains, then the next rotation completes the send.

---

## Open questions

1. **Width of the channel data slot.** 32 bits is the natural choice
   (matches register width). Larger payloads must be passed by pointer
   into shared memory + a single chan_send carrying the pointer. Is
   this enough, or do we want 64-bit channels for double-precision
   floats and pointer-pair handoffs? CPU4 has no 64-bit GPRs so single-
   word is the natural answer; flag and move on.

2. **Closing channels.** Go has `close(chan)`; XMOS uses control tokens
   (`OUTCT`/`INCT`). For first cut, use a sentinel value (as in the
   example above). Add an explicit close opcode (`chclose chid`) only
   if real applications find the sentinel pattern awkward.

3. **`chsel` mask width.** 10 bits in F3d covers 10 channels; 64
   channels needs 64-bit masks. Options: a control register holding
   the mask (`setmask rs`), then `chsel` reads from it. Or split chsel
   into multiple instructions. Defer until v2.

4. **Inter-CPU latency budget.** A remote chan_send via SDRAM requires
   at minimum a pbus round-trip (currently 8–16 cycles). The blocking
   model handles this transparently — the sender just stalls longer.
   But it does mean inter-CPU channels have ≥10× the latency of intra-
   CPU. Worth measuring once a hardware prototype exists.

5. **Fairness on multi-waiter chans.** With at-most-one-waiter
   invariant, fairness is a non-issue inside a single channel. Across
   multiple channels (as seen by `chsel`), priority scheme is an
   implementation choice (lowest chid wins is simplest).

6. **Debugging.** With 8 contexts plus channels, deadlock becomes a
   first-class concern. The debug port (see
   `cpu4_hardware/docs/architecture.md`) already lets the host inspect
   per-context PCs; extending it to dump the channel state array would
   be ~20 lines of Verilog and turns "halt and look" into a usable
   deadlock-debug workflow. Recommend adding a `READ_CHAN` debug
   command alongside `READ_PERF`.

7. **Testing.** Channel correctness is fundamentally about concurrent
   interleavings; the existing single-context unit tests are not
   sufficient. Need a multi-context test harness in
   `cpu4_hardware/test/` that runs a producer/consumer pair against
   the Python reference simulator (which would also need a channel
   model added).

---

## Suggested rollout

1. **Spec freeze**: lock the 4-instruction core set
   (`chsend`/`chrecv`/`chprobe` + `chsendr`/`chrecvr`) and the chid
   encoding (high-bit-routing reservation). Pick F2 vs F3d.
2. **Python simulator**: add `chan_unit` to `cpu4/cpu.py` first. Run
   producer/consumer in lockstep on multiple simulated contexts to
   shake out the state machine.
3. **Assembler**: add the new mnemonics to `cpu4/assembler.py`.
4. **Compiler**: add `__builtin_chan_*` recognition in `parser.c`, IR
   opcodes in `ssa.h`, lowering in `braun.c`, emission in `emit.c`.
   Mark as side-effecting in the opt-pass guards.
5. **`getctx` + per-context dispatch**: wire up the runtime so that
   actual multi-context C programs are buildable.
6. **Hardware**: implement `chan_unit.v` in
   `cpu4_hardware/rtl/`. Verify bit-for-bit against the Python model.
7. **`chsel`**: add as a v2 once real applications motivate the
   reduction in software-poll overhead.
8. **Inter-CPU** (separate milestone): build the SDRAM-backed remote
   proxy; chid high-bit routing kicks in.

Each step is independently testable and reversible. The compiler and
hardware work can proceed in parallel once the spec is frozen, with
the Python simulator as the shared reference.

---

## Why now / why later

**Now (this doc):** the design space has well-known reference points
(XMOS, Transputer, Go) but the CPU4-specific encoding choices are
non-obvious and benefit from being written down before any code is
committed. The 8-context barrel structurally invites this feature; the
ISA encoding still has comfortable slack; the compiler's intrinsics
infrastructure already supports the recognition pattern.

**Later (implementation):** real motivation comes when more than one
CPU instance exists, or when an application emerges that meaningfully
benefits from concurrent contexts. Until then, this is theoretical
infrastructure. Worth resisting the urge to build it speculatively
just because it's elegant.
