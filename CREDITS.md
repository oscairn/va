# Credits and source provenance

Oscairn VA is licensed under the GNU Affero General Public License v3.0 or later (`LICENSE`). It builds on
the work below; vendored source is listed with its licence and pinned provenance. Existing copyright and
license notices in individual files remain authoritative.

## Vendored source

- **CLAP API headers** (`third_party/clap/include/clap/`, version 1.2.10): the CLAP contributors.
  [MIT license](third_party/clap/LICENSE); [pinned provenance](third_party/clap/upstream.json).
- **Oscairn plugin SDK headers** (`third_party/oscairn-clap/include/oscairn-clap/`: `assets.h`, `memory.h`,
  `sound-contexts.h`, `sound-contexts-plugin.hpp`, `ui.h`, `voice-budget.h`, `voice-budget-plugin.hpp`): the Oscairn
  contributors (the copyright holders named in its licence). [MIT license](third_party/oscairn-clap/LICENSE);
  [pinned provenance](third_party/oscairn-clap/upstream.json).

## Laws and data

The DSP core's laws were fitted to our own recordings of a reference instrument's audio output, and its knot
tables were written by us from those fits (`docs/design.md`). `core/params.inc` reproduces the reference
model's published parameter interface (names, raw ranges, defaults) so that patches carry its raw values.
