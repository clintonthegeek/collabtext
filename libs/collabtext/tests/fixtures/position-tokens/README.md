# Supported position-token golden bytes (CTPT/1)

`ctpt-v1-supported.json` holds the canonical bytes for the position-token
format version that roadmap task **E07b** declared supported for consumer
persistence. `libs/collabtext/tests/tst_position_token_reader.cpp` is the reader
fixture that keeps them working.

## What "supported" means here, and what it does not

**Supported** means these bytes, in this layout, keep resolving for a consumer who
stored them, from this release forward. A later supported release either keeps
this reader or ships a checked translation from it. It does *not* mean the token
is a pin, that it survives reclamation, that it survives an epoch change, or that
it survives a store recovery — none of those exist yet, and the exact scope is
stated in `docs/releases/integration-alpha.md` and the installed capability
manifest.

**Not supported**, and not covered by this fixture: any pre-vNext or experimental
token a consumer happened to store earlier. There is no production data, so there
is nothing to convert and no reader obligation for those bytes.

## Why the bytes are canonical

Every case was minted through the installed public surface, one fresh actor
counter per case, so a case is reproducible from its own scenario rather than from
the order cases happened to run in. An earlier draft shared one counter across
cases and was therefore *not* independently reproducible; the reader fixture's
replay check is what caught that, which is the reason the replay check exists.

`byte_length` is recorded rather than trusted: the fixture recomputes the length
the §4 layout in `docs/specs/vnext-boundary-semantics.md` implies from each
case's declared logical fields, and compares. It also contains an independent
literal reader that parses the bytes using only that specification — no codec is
called — and checks the fence tags, identity ranges, distinct fence identities,
strictly increasing frontier, frontier coverage, and exact end of input.

A length check alone is not sufficient here, and that was measured: injecting an
extra 24-byte field into one golden left the total length consistent with "two
item fences" and passed a length-only assertion. The literal reader rejects it,
because an unrecognised fence tag or an unconsumed tail does not parse.

## Coverage

Sixteen cases spanning all four token kinds (`TextBoundary`, `TextAttachment`,
`ListBoundary`, `ListElement`), both affinities, both attachment selections,
empty and populated text, interior and end boundaries, a four-byte non-BMP
scalar, a two-actor mint frontier built by transferring peer operations over the
wire codec, an empty list, interior and end list boundaries, and two distinct
list elements.

Negative coverage is exhaustive per case: every proper truncation and all 256
one-byte trailing suffixes, a bumped version field (`unsupported_version`, not
`malformed`), and a typed read of the wrong kind (`wrong_primitive`).