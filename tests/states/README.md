# Saved test states, one set per published version

`<version>/` holds states saved by that published release (macOS arm64 build), for
`tests/test_saved_states.cpp`: the current build must open every one. The sounds are only the plugin's own defaults
and plain parameter edits; no one's sounds are here.

| file | what it is |
|---|---|
| `default.state` | the plugin's default sound (its init sound) |
| `shell.state` | level 0.6, pan -0.3, polyphony limit 8 |
| `tone.state` | knob 1 at 0.25, knob 8 at 1, filter cutoff 400, filter EG depth 300 |

## Which versions

Every release in the packages repository: 0.11.1 to 0.11.7. They all write the same format (the sound-contexts snapshot,
`oscairn-va-contexts/5`; the CLAP state is `oscairn-va-state 5`), and the current loader reads it, so every one is covered.
The states of one name are byte for byte the same in every version so far; a release that changes the format adds a
directory whose files differ.

## How they were made

For each version: the release archive was taken from the packages repository and checked against the sha256 in its
record, and its macOS arm64 build extracted. Then, with that build,

    oscairn-plugin-check PLUGIN org.oscairn.va synth [--param ID=VALUE ...] --keep-archive FILE.olib

(`shell`: `--param 0=0.6 --param 1=-0.3 --param 2=8`; `tone`: `--param 3=0.25 --param 10=1 --param 18=400 --param 19=300`;
`default`: no parameters) played the plugin in a part, saved it to a library and exported it. The `.state` file is the
export's `Performances/*/nodes/node-1/state/state.clap`, the state as that plugin saved it.

To add a version: do the same with its published build, in a directory named for it, and check that the test passes.
