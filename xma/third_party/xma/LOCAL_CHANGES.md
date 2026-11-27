# Local changes to the vendored `xma` crate

Upstream: omonkeyz/xbox-emulator commit 27122f7dfcceb9a899555362ee0dde2d530e033c, `crates/compat/xma`
(LGPL-2.1-or-later, a Rust port of FFmpeg's libavcodec/wmaprodec.c). `UPSTREAM.sha256` holds the hashes of the files
as fetched. Every file is unchanged except:

- `src/wmapro.rs`: three methods appended to `impl WmaPro` (`state_len`, `export_state`, `import_state`) that read
  and write the inter-frame state (per channel: the overlap half of the output buffer and the previous block length).
  They are used by `port/xma` to carry decoder state between SHAPE XMA contexts the way the title copies a context's
  work buffer into the next one. No decoding code was changed.
