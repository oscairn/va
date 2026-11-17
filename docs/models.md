# Models: laws as data, behavior as code

Status: 2026-11-17. A design for the port to Rust ([plan](plan.md), sections 4 and 5). Nothing here is
built. Today the engine has one model, the reference's, with its laws compiled in as tables
(`core/laws.inc`, `core/filters.inc`) and a few formulas (`core/laws.cpp`).

A **model** is what makes an engine with this wiring and this parameter list behave like one particular
instrument. It has two halves:

- its **laws**: how each stored parameter value becomes a physical quantity. Plain data.
- its **behavior**: how things act over time (how an oscillator is band-limited, where a filter saturates,
  how an envelope segment curves, how often control values update). Code, chosen at compile time.

A sound stores its model's id. The player never sees it. An instrument with a different wiring or a
different parameter list is not a model: it is another plugin built from the same blocks.

## 1. The law set

One value of one type holds every law of a model. It is `const`, has no pointers into code, and can be
written out and read back as JSON or TOML unchanged, so a tool can check it, plot it or generate it from
measurements.

```rust
/// One law: a stored (raw) parameter value to a quantity.
pub struct Law {
    pub unit: Unit,                 // what y is: Hertz, Seconds (a time constant), Decibels, Gain, Cents, Semitones, Ratio
    pub pieces: &'static [Piece],   // in ascending raw order; they cover the parameter's whole range
}

/// A law's rule over one span of raw values. Most laws have one piece; some change rule at a threshold.
pub struct Piece {
    pub from: i32,                  // first raw value this piece answers
    pub rule: Rule,
}

pub enum Rule {
    /// Measured points, interpolated between; held or extended beyond the first and last.
    Knots { x: &'static [f32], y: &'static [f32], between: Between, beyond: Beyond },
    /// y = a * raw + b, clamped to lo..hi: for a law known to be exact arithmetic.
    Affine { a: f32, b: f32, lo: f32, hi: f32 },
    /// y = base * 2^((raw - zero) / per_octave): exact exponential laws.
    Octaves { base: f32, zero: f32, per_octave: f32 },
    /// Use another law of the set (a release time that follows the amplitude release below a threshold).
    Same(LawId),
    /// One value.
    Fixed(f32),
}

pub enum Between { Linear, LogY }          // LogY: straight in log(y), for frequencies and times
pub enum Beyond { Hold, Extend }

/// Every law of one model. A field per law: adding a law is adding a field, and every model must fill it.
pub struct LawSet {
    pub name: &'static str,
    pub osc: OscLaws,           // level, pitch offset, pulse width, the sync and shaper depths
    pub amp: AmpLaws,           // level; attack, decay, release time constants; the attack's target and stop
    pub filter: FilterLaws,     // cutoff; resonance's pass-band gain; each high-pass slope's gain; key follow; velocity
    pub filter_env: EnvLaws,    // attack, decay, release; depth
    pub pitch_env: EnvLaws,
    pub lfo: LfoLaws,           // rate; depth to pitch, filter and amplitude; delay and fade times
    pub glide: GlideLaws,       // rate, and what each time mode means
    pub unison: UnisonLaws,     // detune spread by voice count; level compensation
    pub drift: DriftLaws,       // the aging and drift amounts by setting
    pub drive: &'static [f32],  // the saturator's drive steps
    pub curves: CurveSet,       // the controller curves: one shared source grid, one row of outputs per curve
    pub control_period: u32,    // frames between control updates at the reference rate
}
```

Rules for the data:

- A measured law is `Knots` holding the measured points. It is not replaced by a fitted formula: the points
  are the evidence, and a fit adds its own error.
- A law known to be exact arithmetic is `Affine` or `Octaves`, never a table sampled from it.
- A threshold where the rule changes is a new `Piece`, not an `if` in code.
- Each law says in a comment what it was measured over. Outside that range it is `Hold` unless measured
  otherwise.

### Built tables

Laws are evaluated when a sound is prepared or a parameter changes, never per sample. At a model's first
use its law set is expanded once into flat tables indexed by raw value (one entry per raw step, as
`LawTables` is today), kept for the process. Evaluating a law afterwards is one array read. So laws being
data costs nothing in the audio loop, and the same numbers give the same result wherever they are stored.

## 2. The behavior

Behavior is a trait with one implementation per model. Its associated types pick the variant of each block,
so the choice is made when the plugin is compiled.

```rust
pub trait Model: 'static {
    const ID: ModelId;
    const LAWS: &'static LawSet;

    type Osc: Oscillator;        // band-limiting method, phase on a new note, sync and shaper behavior
    type Filter: FilterCore;     // topology, where it saturates, behavior near self-oscillation
    type AmpEnv: EnvShape;       // how segments curve, what a retrigger and a legato note do
    type FilterEnv: EnvShape;
    type Lfo: LfoCore;
    type Drift: DriftSource;
    type Stealing: StealPolicy;

    /// A sound's stored values to what a voice runs on: which law feeds which quantity, and how velocity,
    /// key follow and offsets combine. The part of the engine that is specific to an instrument.
    fn prepare(patch: &Patch, tables: &Built, rate: f64, out: &mut SoundParams);
    /// The controller matrix: sources and stored depths to this block's parameter offsets.
    fn modulate(sound: &SoundParams, sources: &Sources, out: &mut Offsets);
}
```

The engine is generic over the model:

```rust
pub struct Engine<M: Model> { voices: VoicePool<M>, /* ... */ }

impl<M: Model> Engine<M> {
    pub fn render(&mut self, sound: &SoundParams, out: &mut [f32]) { /* the kernels, with M's blocks inlined */ }
}
```

Each model gets its own compiled copy of the voice loop, with its blocks inlined and optimized together.
Nothing inside the loop asks which model is playing.

The plugin holds one engine per model and picks by the sound's model id once per sound per block:

```rust
match sound.model {
    ModelId::A => self.a.render(sound, out),
    ModelId::B => self.b.render(sound, out),
}
```

A model no sound uses allocates no voices.

## 3. Where things live

```
blocks/                  the shared library (its own repository once a second engine uses it)
  law.rs                 Law, Piece, Rule, the built tables, the checks
  osc/ filter/ env/ lfo/ matrix/ drift/     each block and its variants
engine/
  engine.rs              Engine<M>, the voice pool, the kernels
  model.rs               the Model trait
  models/
    mod.rs               the list of models: one line each
    a.rs                 model A: its LawSet as one const, its Model implementation
    a_laws.rs            (when a.rs gets long) the tables alone, generated or pasted from measurements
plugin/                  the plugin shell: parameters, state, the host's extensions
```

One file answers "what makes model A model A". The blocks know nothing about any model; the engine knows
only the trait.

## 4. Adding or changing a model

To change a law: edit its entry in the model's file. The checks below say what moved.

To add a model:

1. Copy a model file. Fill the `LawSet`: the compiler refuses a set with a law missing.
2. Choose each block variant in the `Model` implementation. If the instrument needs a variant that does not
   exist (another filter topology, another envelope shape), add it to the block library with its own tests.
3. Write `prepare` and `modulate`, starting from the nearest model's.
4. Add the line in `models/mod.rs`.
5. Add its test fixtures and its cost-gate cases.

To add a law to every model: add the field to `LawSet`. Each model then fails to compile until it supplies
the law, so none is forgotten.

## 5. Checks

- **The data is well formed** (a unit test over every model): knots ascending, pieces cover the parameter's
  whole raw range with no gap, every parameter in the parameter table is either read by a law or marked as
  used directly, units match what the consumer expects.
- **A model sounds as it did**: reference renders per model, compared sample for sample on the scalar path
  and within a stated tolerance on the vector path. For the first model the references are the C++
  engine's renders, so the port is proven against what ships today.
- **Cost**: the cost gate runs per model.
- **Saved sounds**: a sound with no model id loads as the first model, unchanged.

## 6. What this does not cover

- An instrument whose controls differ from this parameter list. That is another plugin on the same blocks.
- Loading a law set from a file at run time. The law set is plain data so that this stays a small step,
  but a file can never carry the behavior half, and a sound's result would then depend on more than the
  plugin's version. Not planned.
- Law values as plugin parameters. Parameters are what a player changes.

## 7. Before the port

New laws added to the C++ engine are written as knot tables with their thresholds beside them, not as
formulas or numbers in code, so the law set does not gain new special cases to untangle later.
