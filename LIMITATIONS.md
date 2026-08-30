# Limitations

Stated so you don't have to discover them.

- **No transport.** The session layer consumes and produces byte spans; TCP,
  reconnect, and I/O scheduling are the caller's.
- **No persistence.** Sequence numbers and the resend store are in memory.
  A restart loses them; production sessions journal both.
- **No dictionary-driven *validation*.** The dictionary is used to resolve
  group structure, not to reject unknown tags, enforce required fields, or
  check enumerated values.
- **FIX 4.4 dictionary only** is checked in. FIX 4.2 messages parse at the
  flat level; groups need a 4.2 spec run through `tools/gen_dictionary.py`.
- **Group heuristic.** Entry boundaries rely on FIX's rule that a tag is
  unique within an entry and that group members are contiguous. A message
  that violates the standard's ordering rules may be attributed wrongly;
  it will not crash.
- **Typed decoders** cover NewOrderSingle and ExecutionReport only.
- **Fixed capacity.** `Message<N>` holds N fields (default 256) and sets
  `overflow` rather than allocating. Nesting is capped at 8.
- **Benchmarks are single-message, L1-resident.** Real feeds pay for cache
  misses this benchmark does not measure.
