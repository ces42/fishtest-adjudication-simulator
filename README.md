# Fishtest adjudication analysis

Compare alternative draw/resignation rules against the recorded results of
Fishtest games. `reader.cpp` reads annotated PGNs, estimates the time saved by
earlier adjudication, and computes current and alternative pentanomial normalized
Elo (nElo). `plot_nelo.py` plots the reader's output and compares multiple rules.

## Requirements and build

The reader uses Linux/POSIX memory mapping and requires a C++ compiler with
C++23 support, including the `<stdfloat>` header. The plotter requires Python 3
and Matplotlib.

```sh
g++ -std=c++23 -O3 -march=native reader.cpp -o reader
python3 -m venv .venv
. .venv/bin/activate
python -m pip install matplotlib
```

The Python examples below assume that Matplotlib is installed for the `python3` being used.

## Input files

Get pgns of fishtest LTC games from [hugging face](https://huggingface.co/datasets/official-stockfish/fishtest_pgns). The reader expects pgns in this format, including engine names
starting with `New-` / `Base-`, evaluation/time comments on moves, and headers
such as:

```text
[Event "Batch 2332: wave10b vs master"]
[Round "13"]
[White "New-..."]
[Result "1/2-1/2"]
[PlyCount "120"]
[TimeControl "60+0.6"]
```

> [!CAUTION]
> The pgn parsing is **very** brittle and will break if the comments in the pgn file follow a slightly different format than those on hugging face.

## Running the reader

```text
./reader FILE.pgn DRAW_CP DRAW_PLIES WIN_CP WIN_PLIES [twosided=false|true]
```

| Argument | Meaning |
| --- | --- |
| `FILE.pgn` | Annotated, uncompressed Fishtest PGN |
| `DRAW_CP` | Maximum absolute evaluation, in centipawns, for a draw |
| `DRAW_PLIES` | Consecutive evaluated plies needed to declare a draw |
| `WIN_CP` | Evaluation magnitude, in centipawns, for a win/resignation |
| `WIN_PLIES` | Consecutive evaluated plies favoring the same side needed to declare a win |
| `twosided` | Optional, default `false`: require only the losing engine's evaluations for resignation. `true` requires both engines' evaluations, as in earlier versions. Bare `false` / `true` are also accepted. |

Note that adjudication rules are typically formulated as "eval is at least X for Y **moves**", but this uses plies to allow for more flexibility.

In one-sided mode, each qualifying losing-engine evaluation counts as two plies;
the counter is reset when that engine's evaluation no longer meets the threshold.
For example, `WIN_PLIES=6` requires three consecutive qualifying evaluations from
the losing engine. Odd counts round up to the next losing-engine observation.
Draw adjudication still checks both engines in either mode. Append `twosided=true`
to reproduce the earlier behavior. The plotter includes the mode in rule labels;
older saved output without a mode is interpreted as two-sided.

For example:

```sh
./reader fishtest_pgns/26-10-01/6abe5012c109910452a9ad63/6abe5012c109910452a9ad63.pgn \
  25 10 220 4
```

This adjudicates a draw after 10 consecutive evaluated plies within ±25 cp, or a win
after 4 consecutive evaluated plies favoring one side by at least 220 cp.

The output includes:

- The filename and adjudication parameters.
- Number of parsed games, mismatches with recorded results, and estimated time saved.
- A matrix comparing recorded and alternative outcomes from New's perspective.
- Current and alternative pentanomial counts.
- The score relative to 50%, the alternative score change, and both nElo estimates
  with approximate 95% confidence intervals.

Time statistics are based on recorded engine move times, normalized to a
60-second base time control.

## Plot reader output

Pass the output of `reader` to the stdin of the plotter:

```sh
./reader path/to/test.pgn 25 10 220 4 | python3 plot_nelo.py
```

Save an image instead of opening a window:

```sh
python3 plot_nelo.py -o adjudication.png < reader_output.txt
```

The output format is selected by the extension; for example, use `.png`, `.pdf`,
or `.svg`. For a machine without a graphical display:

```sh
MPLBACKEND=Agg python3 plot_nelo.py -o adjudication.png < reader_output.txt
```

Exclude runs with fewer than a specified number of parsed games:

```sh
python3 plot_nelo.py --min-games 100000 -o adjudication.png < reader_output.txt
```

With a positive threshold, every run must have a `Parsed N games.`
line. The plotter reports an error if all runs are filtered out.

The plotter treats a rule with a ply count `>99` as disabled when generating
labels.

## Compare multiple PGNs and alternative rules

Concatenate the **full reader output** for each file and parameter combination.
For example, with GNU Parallel installed, you can use

```sh
parallel --group -j4 './reader {} 20 99 600 6; ./reader {} 20 99 300 4' ::: fishtest_pgns/26-{10,09}-*/*/*.pgn | ./plot_nelo.py --min-games 10000
```
to compare two alternative adjudication rules.
Adjudication rules with ply counts `>= 99` are ignored by the plotter.



# LLM use
- `reader.cpp` is mostly hand-coded
- `plot_nelo.py` is fully vibe-coded
- this `README.md` is maybe 70% vibe-coded
