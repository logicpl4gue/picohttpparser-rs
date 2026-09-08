# PicoHTTPParser-RS
## Agent-Ready Planning & Foundational Framework

## 1. Project Goal

Build a **drop-in Rust rewrite of PicoHTTPParser** that preserves the original library's observable behavior and C-facing API/ABI closely enough that existing consumers can use the Rust implementation without needing application-level changes.

This is **not** simply "an HTTP parser written in Rust." Rust already has strong HTTP parsing libraries such as `httparse`.

The interesting challenge is:

> Can we replace a tiny, battle-tested, allocation-free C HTTP parser with a Rust implementation while preserving compatibility, proving correctness scientifically, and potentially matching or exceeding the original's performance?

The project should be designed from the beginning so the final result can support a credible GitHub post, technical write-up, or YouTube video with reproducible evidence rather than subjective claims.

---

# 2. Core Thesis

PicoHTTPParser is a strong rewrite target because it is:

- Small enough to fully understand.
- Used in real software.
- Performance-sensitive.
- Stateless.
- Zero-copy.
- Allocation-free during parsing.
- Exposed through a tiny C API.
- Easy to benchmark against the original.
- Well suited to differential testing and fuzzing.
- Small enough that an agent-assisted rewrite can remain auditable.

The goal is not to prove that "Rust is better than C."

The goal is to conduct a controlled engineering experiment:

> Can a modern, memory-safe implementation replace the original while consumers cannot tell the difference?

If Rust loses in a benchmark or behaves differently, that result should be preserved and reported rather than hidden.

---

# 3. Primary Success Condition

The strongest possible demonstration is:

```text
Existing C Application
        |
        v
expects PicoHTTPParser API
        |
        v
Rust replacement library
        |
        v
application works normally
```

A successful rewrite should ultimately demonstrate at least one real consumer using the Rust implementation without application-level parser changes.

H2O is an obvious candidate because PicoHTTPParser originated in the H2O ecosystem.

---

# 4. Scope

## In Scope

Reimplement PicoHTTPParser's public parsing functionality, including:

- Request parsing
- Response parsing
- Header parsing
- Chunked-transfer decoding
- Chunked decoder state behavior
- Streaming/incomplete-input behavior
- Original return-value semantics
- Pointer/length output behavior
- Public structs required by the ABI
- C ABI compatibility where practical

Target functions include the PicoHTTPParser public surface such as:

```c
phr_parse_request()
phr_parse_response()
phr_parse_headers()
phr_decode_chunked()
phr_decode_chunked_is_in_data()
```

The agent must confirm the exact upstream public API from the pinned source before implementing it.

## Out of Scope Initially

Do not expand this project into:

- A web server
- HTTP/2
- HTTP/3
- TLS
- Routing
- Request handling
- Full HTTP semantic validation
- A new high-level Rust HTTP framework
- General-purpose networking abstractions

Keep the project narrow.

---

# 5. Guiding Principles

## Compatibility Before Optimization

The order is:

```text
correct
    ->
compatible
    ->
measurable
    ->
fast
```

Do not optimize parsing before differential behavior is stable.

## Original Implementation as Behavioral Oracle

The original PicoHTTPParser implementation should be treated as the reference implementation.

Where documentation and implementation disagree, record the disagreement and determine what real consumers rely on.

## Minimal Unsafe Rust

Prefer:

```text
C ABI boundary
    |
small audited unsafe layer
    |
safe Rust parsing core
```

Unsafe code is acceptable where required for ABI compatibility and raw pointer handling, but it should be:

- localized,
- documented,
- reviewable,
- measured in final project statistics.

## Preserve Zero-Copy Behavior

The parser should not unnecessarily allocate or copy request data.

Internally, prefer offsets/ranges into the original buffer.

Convert those offsets to C-compatible pointers only at the FFI boundary.

---

# 6. Suggested Repository Structure

A simple structure is preferable:

```text
picohttpparser-rs/
|
|-- README.md
|-- Cargo.toml
|-- build.rs
|
|-- src/
|   |-- lib.rs
|   |-- ffi.rs
|   |-- request.rs
|   |-- response.rs
|   |-- headers.rs
|   |-- chunked.rs
|   `-- util.rs
|
|-- reference/
|   `-- pinned upstream PicoHTTPParser
|
|-- tests/
|   |-- compatibility/
|   |-- regression/
|   |-- malformed/
|   `-- corpus/
|
|-- fuzz/
|
|-- benches/
|
|-- scripts/
|   |-- differential.*
|   `-- benchmark.*
|
|-- results/
|   |-- baseline.json
|   |-- latest.json
|   `-- README.md
|
`-- docs/
    |-- compatibility.md
    |-- divergences.md
    `-- methodology.md
```

Do not create unnecessary architecture before it is required.

---

# 7. Implementation Strategy

## Phase 0 — Freeze the Reference

Before writing the rewrite:

1. Select and pin an exact upstream PicoHTTPParser revision.
2. Record:
   - commit hash,
   - compiler,
   - compiler flags,
   - operating system,
   - CPU,
   - architecture.
3. Build the original implementation.
4. Run its tests.
5. Produce baseline benchmark results.
6. Archive the exact benchmark inputs.

This baseline must remain immutable.

---

## Phase 1 — ABI Shell

Implement the public Rust-exported C symbols without meaningful parsing behavior yet.

Validate:

- symbol names,
- calling convention,
- struct layout,
- integer widths,
- alignment,
- pointer types,
- return types.

Create tiny C programs that link against the Rust library.

Do not continue until this layer is understood.

---

## Phase 2 — Request Parser

Implement request parsing first.

Required behavior includes:

- HTTP method
- request path
- HTTP version
- headers
- complete message
- incomplete message
- malformed message
- header limits
- edge-case whitespace
- CRLF handling
- streaming continuation behavior

Every discovered discrepancy must become a regression test.

---

## Phase 3 — Response Parser

Implement:

- status line
- HTTP version
- status code
- reason phrase
- headers
- incomplete responses
- malformed responses

Run the same differential process used for requests.

---

## Phase 4 — Header Parser

Implement standalone header parsing and verify unusual cases such as:

- empty values,
- repeated headers,
- whitespace variations,
- invalid bytes,
- truncated headers,
- maximum header count.

Do not "improve" validation if doing so changes observable behavior without explicitly documenting the divergence.

---

## Phase 5 — Chunked Decoder

Treat the chunked decoder as a separate compatibility problem.

Test:

- normal chunks,
- multiple chunks,
- zero-length terminal chunk,
- chunk extensions,
- split boundaries,
- incomplete sizes,
- invalid hex,
- trailers,
- decoder-state transitions,
- malformed input,
- extremely large declared lengths.

Streaming behavior matters as much as complete-buffer behavior.

---

# 8. Testing Strategy

Testing is a first-class deliverable.

The project should generate evidence that can be published later.

## Layer 1 — Upstream Tests

Run the original PicoHTTPParser test suite against:

```text
A. Original C implementation
B. Rust replacement
```

Target:

```text
100% applicable upstream tests passing
```

Track:

- total tests,
- passed,
- failed,
- skipped,
- reason for every skip.

No silent exclusions.

---

## Layer 2 — Differential Testing

Feed identical inputs into both implementations.

Compare all observable outputs, including:

- return value,
- parsed method,
- path,
- version,
- status,
- reason phrase,
- header count,
- header names,
- header values,
- chunked output,
- decoder state,
- incomplete/error behavior.

Conceptually:

```text
             INPUT
            /     \
           v       v
     Original C   Rust
           \       /
            compare
```

Any mismatch becomes:

1. a stored reproduction,
2. a regression test,
3. a documented divergence until resolved.

Track:

```text
Differential cases executed
Known divergences
Resolved divergences
Unresolved divergences
```

---

# 9. Fuzzing Requirements

Differential fuzzing is one of the project's strongest proof mechanisms.

## Targets

At minimum fuzz:

- request parser
- response parser
- header parser
- chunked decoder

## Corpus

Seed the corpus with:

- valid HTTP requests,
- valid HTTP responses,
- valid chunked bodies,
- truncated messages,
- malformed headers,
- random bytes,
- real HTTP traffic samples where legally usable,
- previously discovered bugs.

## Mutation Focus

Encourage mutation around:

```text
spaces
tabs
CR
LF
colons
HTTP versions
header counts
very long tokens
NUL bytes
high-bit bytes
chunk sizes
chunk extensions
truncation points
```

## Required Reporting

Record:

- total fuzz executions,
- fuzz runtime,
- crashes in C implementation,
- crashes in Rust implementation,
- semantic mismatches,
- unique mismatch cases,
- minimized reproductions.

Do not report only "fuzzed successfully."

The exact number of executions matters.

---

# 10. Safety Testing

Run the Rust implementation under appropriate tooling where possible.

Useful approaches may include:

- Miri for Rust-side undefined behavior checks,
- sanitizers around C/reference harnesses,
- AddressSanitizer for the reference implementation,
- UndefinedBehaviorSanitizer,
- malformed-input stress tests.

Particularly scrutinize the FFI boundary.

Track:

```text
Unsafe Rust LOC
Number of unsafe blocks
FFI functions
Known sanitizer findings
Known Miri findings
```

The goal is not "zero unsafe code."

The goal is **small, deliberate, auditable unsafe code**.

---

# 11. Performance Methodology

Performance comparisons must be reproducible.

Do not benchmark one random HTTP request and declare victory.

## Benchmark Categories

At minimum:

### Tiny Request

```http
GET / HTTP/1.1
Host: example.com
```

### Typical Request

Representative browser/API request with several headers.

### Large Headers

Many headers and larger header values.

### Response Parsing

Small and medium responses.

### Chunked Decoding

Small and large chunk streams.

### Malformed Input

Inputs that cause early rejection.

### Streaming

Inputs delivered across many split positions.

---

# 12. Metrics to Track

For each implementation record where meaningful:

```text
Throughput
Requests/second
Bytes/second
ns/parse
p50 latency
p95 latency
p99 latency
CPU usage
Peak memory
Allocations per parse
Binary/library size
```

The most important metrics are likely:

```text
ns/parse
throughput
allocations
compatibility
```

---

# 13. Benchmark Rules

Use the same:

- machine,
- CPU governor/power settings,
- compiler versions,
- optimization level,
- input corpus,
- number of iterations.

Perform warmups.

Perform multiple runs.

Report:

```text
mean
median
standard deviation
```

Prefer publishing raw benchmark outputs alongside summaries.

Never use debug builds in final comparisons.

---

# 14. Performance Targets

These are project targets, not claims.

## Minimum Viable Success

The rewrite is already successful if it achieves:

```text
100% upstream test compatibility
0 unresolved known semantic divergences
0 allocations during normal parsing
successful real-consumer integration
stable fuzzing with no Rust crashes
performance within approximately 10% of C
```

A rewrite does **not** have to outperform C to be valuable.

## Strong Success

```text
All compatibility goals
+
equal or better median performance
+
safe Rust parser core
+
real application integration
```

## Exceptional Success

```text
All compatibility goals
+
measurably faster than C
+
lower or comparable memory usage
+
production consumer running unchanged
+
large differential fuzz campaign with zero unresolved divergence
```

---

# 15. Optimization Phase

Only optimize after compatibility stabilizes.

Potential investigation areas:

- `memchr`
- vectorized delimiter scanning
- SIMD
- branch reduction
- bounds-check elimination where safe
- parsing several bytes at once
- better state-machine layout
- reduced pointer conversion
- cache-friendly structures

Every optimization should follow:

```text
hypothesis
   ->
benchmark
   ->
implementation
   ->
benchmark again
   ->
keep or revert
```

Maintain before/after numbers.

Do not preserve an optimization that is merely "clever."

---

# 16. Real-World Integration Requirement

A benchmark library alone is less interesting than a successful drop-in replacement.

The project should attempt integration with at least one genuine PicoHTTPParser consumer.

Preferred target:

```text
H2O
```

Possible approach:

1. Build the consumer normally.
2. Substitute or relink the parser implementation.
3. Run its tests.
4. Serve real HTTP traffic.
5. Compare behavior.
6. Benchmark throughput and latency.

Record whether source changes were required.

The strongest result is:

```text
Application source changes required: 0
```

---

# 17. Evidence Dashboard

Maintain a machine-readable results file throughout development.

Example:

```text
Compatibility
-------------
Upstream tests:            142 / 142
Differential cases:        50,000,000
Known divergences:         0
Regression cases:          37

Fuzzing
-------
Executions:                310,000,000
Runtime:                   72 h
Rust crashes:              0
Semantic mismatches:       0 unresolved

Performance
-----------
Typical request C:         XXX ns
Typical request Rust:      XXX ns
Delta:                     X.X%

Allocations C:             0
Allocations Rust:          0

Implementation
--------------
Rust LOC:                  XXXX
Unsafe LOC:                XX
Unsafe blocks:             X
Library size:              XXX KB

Integration
-----------
H2O links:                 PASS
H2O tests:                 PASS
Application changes:       0
```

Numbers above are illustrative only.

Never pre-fill success metrics with fake values.

---

# 18. Scientific Process

For every meaningful claim, retain enough information for reproduction.

Each result should answer:

```text
What was tested?
Against what version?
On what hardware?
With what compiler?
Using what command?
With what corpus?
How many iterations?
What result was produced?
```

Keep raw results under:

```text
/results/
```

Prefer generated result files over manually copied numbers.

---

# 19. Divergence Policy

Never hide compatibility problems.

Maintain:

```text
docs/divergences.md
```

For each disagreement record:

```text
Input
C result
Rust result
Expected resolution
Status
Date discovered
Regression test
```

Possible statuses:

```text
OPEN
FIXED
INTENTIONAL
REFERENCE BUG
UNRESOLVED
```

If the original appears buggy, preserve compatibility first unless there is a compelling safety reason not to.

A safer divergent mode can be introduced later as an optional feature.

---

# 20. Agent Development Workflow

The implementation agent should work incrementally.

Recommended loop:

```text
inspect target behavior
        |
implement smallest missing behavior
        |
run unit tests
        |
run differential tests
        |
run regression corpus
        |
commit
        |
repeat
```

Avoid large speculative rewrites.

Prefer small milestones that can be independently verified.

---

# 21. Milestone Plan

## Milestone 0 — Baseline

Deliver:

- pinned PicoHTTPParser source,
- original tests passing,
- benchmark harness,
- recorded baseline.

## Milestone 1 — Rust ABI

Deliver:

- shared/static library,
- correct exported symbols,
- C smoke-test links successfully.

## Milestone 2 — Requests

Deliver:

- request parsing,
- request differential tests,
- compatibility report.

## Milestone 3 — Responses + Headers

Deliver:

- response parsing,
- standalone headers,
- growing regression corpus.

## Milestone 4 — Chunked

Deliver:

- compatible chunk decoder,
- streaming tests.

## Milestone 5 — Compatibility Gate

Target:

```text
100% applicable upstream tests
0 unresolved known divergences
```

Do not begin major optimization before this gate.

## Milestone 6 — Fuzz Campaign

Run sustained differential fuzzing.

Convert every discovered mismatch into a permanent regression test.

## Milestone 7 — Real Consumer

Integrate with H2O or another genuine consumer.

Prove actual traffic works.

## Milestone 8 — Performance

Create reproducible C vs Rust benchmark suite.

## Milestone 9 — Optimization

Profile first.

Optimize only measured bottlenecks.

## Milestone 10 — Final Evidence

Generate:

- final benchmark tables,
- compatibility totals,
- fuzz statistics,
- unsafe-code totals,
- integration evidence,
- known limitations.

---

# 22. Definition of Done

The project should not be described as a successful rewrite until:

- The pinned upstream test suite passes.
- Known semantic divergences have been resolved or explicitly documented.
- Differential testing has exercised a substantial corpus.
- Fuzzing has been performed and quantified.
- Normal parsing remains zero-allocation.
- ABI behavior has been tested.
- At least one real consumer has been integrated.
- Benchmarks are reproducible.
- Results include losses as well as wins.
- The unsafe Rust surface has been audited and measured.
- All significant claims can be independently reproduced.

---

# 23. Final Public Story

The eventual public story should not be:

> "AI rewrote a C parser in Rust."

It should be:

> "I used AI-assisted development to see whether a modern Rust implementation could become a drop-in replacement for a tiny production C HTTP parser. We treated the original as a behavioral oracle, ran differential fuzzing, measured every disagreement, integrated the rewrite into real software, and benchmarked both implementations under identical conditions."

That framing gives the project value regardless of which implementation wins.

Possible final results are all interesting:

```text
Rust wins performance.
C wins performance.
They tie.
Rust exposes bugs.
Fuzzing exposes bugs in both.
Compatibility proves unexpectedly difficult.
The rewrite works but increases binary size.
SIMD changes the result.
A real server runs without realizing its parser was replaced.
```

The experiment is the product.

---

# 24. Immediate Agent Instruction

Begin with **Milestone 0 only**.

Do not start implementing the Rust parser until the following exist:

1. Exact upstream revision pinned.
2. Original library builds reproducibly.
3. Original tests pass locally.
4. Baseline benchmark harness exists.
5. Public API and struct layouts have been documented.
6. Repository structure has been created.
7. Initial methodology/results files exist.

Once the baseline is reproducible, begin the ABI shell and request parser.

Keep every result measurable from the first commit.
