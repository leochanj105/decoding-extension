# Ownership pilot: sources

## Benchmark

- Repository: https://github.com/anirudhkhatry/CRUST-bench
- Commit: `4b4702544b800a3821feea128c5b9e413665dc83` (2026-04-27)
- The tasks are in the archive `datasets/CRUST_bench.zip`
  (sha256 `a9c3024fd5c7386e790938c6a2ec2c75c4d4582174289cb037881e04e9adc15e`).
  Paths below are inside that archive. `RBench/` holds the Rust interfaces and tests;
  `CBench/` holds the original C projects.
- CRUST-bench ships no Rust reference implementations: every function body in the
  `RBench` interfaces is `unimplemented!()`.

## Task 1: `libbase122::BitWriter::new`

Original C project: https://github.com/kevinAlbs/libbase122, commit
`e71b8a65030dcf112911eebc11c40f2ea4ce0bab` (from `CBench/libbase122/.git`).

| file (in the archive) | sha256 | used for |
|---|---|---|
| `RBench/libbase122/src/interfaces/base122.rs` | `ab4b4fd13c2baa0ac250109cf2ead63128aed3de00ee44012d9e89451eef39da` | signature (line 40); `BitWriter` (32–38); enclosing `impl` (39) |
| `RBench/libbase122/src/bin/tests.rs` | `908b82ece390e61739c70f5aaea25d45565972ebbf8bed43f87d44b92f2ec1f2` | how the tests call `new` (`test_bit_writer`, lines 74–113); not quoted in the prompt |
| `CBench/libbase122/src/util.h` | `abfb402ebf0b65ef30fca310a815ffc8fe759e387e0e8b7ea9c39431f74efddd` | `bitwriter_t` fields and comments (lines 83–89); count-only and capacity rules in `bitwriter_write` (91–108) |
| `CBench/libbase122/src/base122.c` | `3c14f1bb29a47c8648d7b50e287439594ffc044bcc7ddc84ad15807a41cd2c67` | initialisation of a writer: `out`, `len`, `curBit = 0`, `countOnly = 1` iff `out == NULL` (lines 153–165) |

The C library has no constructor; `BitWriter::new` corresponds to the field
initialisation in `base122_decode`. Both examples in the prompt (`Some`, `None`) are
derived from that code and the struct comment ("May be 0 if countOnly is true"); they
are not tests in the benchmark.

Adaptations of the Rust interface in the prompt:
- Kept verbatim: the target signature, the `BitWriter` struct with its doc comment, and
  the enclosing `impl<'a> BitWriter<'a>` line.
- Left out: `Base122Error`, the `use std::error::Error;` / `use std::fmt;` imports (unused
  by `new`), the `write` declaration, `BitReader`, `ILLEGALS`, and all free functions.
- Two requirements are phrased without naming `write`: "the writer cannot write more
  than `len * 8` bits" (from `bitwriter_write`'s capacity check) and "the writer only
  counts bits ... and never writes to a buffer" (from its count-only branch).
- No example performs writes; the benchmark's write sequence (output byte 0xDE) is not
  in the prompt.

## Task 2: `totp::hmac_sha1`

Original C project: https://github.com/sjmulder/totp, commit
`01e10a1f1f76e80c0613abd10027f9c0cda7c13b` (from `CBench/totp/.git`).

| file (in the archive) | sha256 | used for |
|---|---|---|
| `RBench/totp/src/interfaces/totp.rs` | `7b69f150ece4d6aa3f53bbc0c0e4f79960a2ff67c8445f9b0ce3877ac0b91a0d` | target signature (line 27); `sha1` signature (line 9) |
| `RBench/totp/src/bin/test.rs` | `62dff72709d7c22acbc06c03aa762ef6e322fd95431f384d3e6abebc385437a6` | `test_hmac_sha1` (lines 60–69): the RFC 2202 vector, reserved for evaluation; not in the prompt |
| `CBench/totp/totp.c` | `eb510ff49d438d894ae45b3a30700e5479d6753431335207c219d41fbbdafa52` | `hmac_sha1` (lines 69–88): `len > 64` check, 0x36 / 0x5C pads, two `sha1` calls; `sha1` padding and capacity check (23–34) |
| `CBench/totp/totp.h` | `8a6b7e4a1ef1b71def0f187623b65a2a2ae2d71086fdda5e2d40253b1bd2da58` | result codes `TOTP_OK = 0`, `TOTP_EBOUNDS = 1` (lines 3–6); `sha1` and `hmac_sha1` parameter docs (40–66) |

Adaptations of the Rust interface in the prompt:
- Kept verbatim: the target signature and the `sha1` signature (body shown as `{ ... }`).
- Left out: `use crate::std;`, the `TOTP_EXPORT` constant, every other function
  (`hotp`, `from_base32`, `unpack64`, `pack32`, `unpack32`, `totp`, `rotl`), and the
  crate's `std` module (`std.rs`: `SIZE_MAX`, `memset`, `memcpy`, `strlen`).
- The `sha1` contract is written in prose from `totp.h` (lines 40–52) and `totp.c`
  (lines 23–34); the Rust interface has no comment on it.
- The result codes are stated in prose; the Rust interface does not declare them.
- "Callers ensure `len <= data.len()`" restates the C precondition (the C code reads
  `len` bytes from `data` unchecked).
- No test vector is given; the benchmark's RFC 2202 case 3 (key 20 × 0xAA, 50 × 0xDD,
  digest `125d7342b9ac11cd91a39af48aa17b4f63f175d3`) is reserved for evaluation.

## Notes for evaluation (not part of the prompts)

- `BitWriter::new`: evaluation can inspect the four public fields of the returned
  writer directly (for `Some(..)` and `None` outputs); it does not need an
  implementation of `write`.
- `hmac_sha1`: evaluation needs a working `sha1` helper, validated on its own (e.g.
  against the benchmark's `test_sha1` vectors) before the HMAC output is judged. That
  helper's implementation is outside this step.
- `RBench/libbase122/src/lib.rs` declares `pub mod base122;` while the file is at
  `src/interfaces/base122.rs`; `Cargo.toml` says edition 2024 (totp: 2021).
