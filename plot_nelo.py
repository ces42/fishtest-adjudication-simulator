#!/usr/bin/env python3
"""Plot concatenated ./reader output: ./reader ... | python3 plot_nelo.py."""

import argparse
import json
import math
from pathlib import Path
import re
import sys

import matplotlib.pyplot as plt
from matplotlib.colors import to_rgba


ESTIMATE = re.compile(
    r"^\s*(alt )?nElo estimate\s*=\s*(\S+)\s*"
    r"\[\s*([^,]+),\s*([^\]]+)\]\s*95% CI\s*$"
)
TIME_SAVED = re.compile(r"^\s*avg time saved:.*?([+-]?[\d.eE]+)%\s*$")
GAMES = re.compile(r"^\s*Parsed (\d+) games\.\s*$")
PARAMETERS = re.compile(
    r"^\s*Parameters:\s*draw\((-?\d+)cp,\s*(\d+) plies\)\s*"
    r"win\((-?\d+)cp,\s*(\d+) plies\)\s*$"
)
FILENAME = re.compile(
    r"""(?:^|[\s"'])([^\s"']*?([^/\\\s"']+)\.pgn(?:\.gz)?)(?=$|[\s"'])"""
)


def read_sprt(filename):
    if filename is None:
        return None
    path = Path(filename)
    if path.suffix == ".gz":
        path = path.with_suffix("")
    path = path.with_suffix(".json")
    if not path.is_file():
        return None
    with path.open() as stream:
        data = json.load(stream)
    sprt = data.get("args", {}).get("sprt")
    if not sprt or "elo0" not in sprt or "elo1" not in sprt:
        return None
    state = sprt.get("state", "")
    color = {"accepted": "green", "rejected": "red"}.get(state, "black")
    return f"[{sprt['elo0']:g}, {sprt['elo1']:g}]", color


def read_estimates(lines):
    pairs = []
    main = None
    time_saved = None
    run_id = None
    filename = None
    games = None
    rule = "Unspecified adjudication"
    for line_number, line in enumerate(lines, 1):
        filename_match = FILENAME.search(line)
        if filename_match:
            filename, run_id = filename_match.groups()
        parameters_match = PARAMETERS.match(line)
        if parameters_match:
            draw_cp, draw_plies, win_cp, win_plies = map(int, parameters_match.groups())
            enabled = [
                f"{kind}: {cp}cp over {plies} plies"
                for kind, cp, plies in (
                    ("draw", draw_cp, draw_plies), ("win", win_cp, win_plies)
                )
                if plies < 99
            ]
            rule = ", ".join(enabled) or "Adjudication disabled"
            time_saved = None
            games = None
        games_match = GAMES.match(line)
        if games_match:
            games = int(games_match.group(1))
            continue
        saved_match = TIME_SAVED.match(line)
        if saved_match:
            time_saved = float(saved_match.group(1))
            if not math.isfinite(time_saved):
                raise ValueError(f"line {line_number}: non-finite time saved")
            continue
        match = ESTIMATE.match(line)
        if not match:
            continue
        estimate, low, high = map(float, match.group(2, 3, 4))
        if not all(map(math.isfinite, (estimate, low, high))):
            raise ValueError(f"line {line_number}: non-finite estimate or interval")
        if not low <= estimate <= high:
            raise ValueError(f"line {line_number}: estimate outside its interval")
        result = (estimate, low, high)
        if match.group(1):
            if main is None:
                raise ValueError(f"line {line_number}: alternative without main estimate")
            pairs.append((main, result, time_saved, run_id, read_sprt(filename), rule, games))
            main = None
            time_saved = None
            run_id = None
            filename = None
            games = None
            rule = "Unspecified adjudication"
        else:
            if main is not None:
                raise ValueError(f"line {line_number}: previous alternative is missing")
            main = result
    if main is not None:
        raise ValueError("final alternative estimate is missing")
    if not pairs:
        raise ValueError("no paired nElo estimates found on stdin")
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", help="save the plot instead of opening a window")
    parser.add_argument("--min-games", type=int, default=0, metavar="N",
                        help="exclude runs with fewer than N parsed games (default: 0)")
    args = parser.parse_args()
    if args.min_games < 0:
        parser.error("--min-games must be nonnegative")
    try:
        pairs = read_estimates(sys.stdin)
    except (ValueError, OSError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    if args.min_games:
        if any(pair[6] is None for pair in pairs):
            parser.exit(1, f"{parser.prog}: --min-games requires a 'Parsed N games.' line for every run\n")
        pairs = [pair for pair in pairs if pair[6] >= args.min_games]
        if not pairs:
            parser.exit(1, f"{parser.prog}: no runs meet --min-games {args.min_games}\n")

    # Preserve rule encounter order, independently of sorting the PGNs by nElo.
    rules = list(dict.fromkeys(pair[5] for pair in pairs))
    palette = ["tab:orange", "tab:purple", "tab:red", "tab:cyan", "tab:olive",
               "tab:brown", "tab:pink", "tab:green", "tab:gray"]
    colors = {
        rule: palette[i] if i < len(palette)
        else plt.get_cmap("hsv")(((i - len(palette)) * 0.61803398875) % 1)
        for i, rule in enumerate(rules)
    }
    grouped = {}
    for i, pair in enumerate(pairs):
        key = ("id", pair[3]) if pair[3] else ("run", i)
        grouped.setdefault(key, []).append(pair)
    groups = sorted(grouped.values(), key=lambda group: group[0][0][0])
    labeled_runs = any(group[0][4] is not None for group in groups)
    fig, ax = plt.subplots(
        figsize=(max(8, min(20, len(groups) * 0.5)), 5 + 0.22 * len(rules))
    )
    spacing = min(0.16, 0.65 / len(rules))
    main_offset = -spacing * len(rules) / 2
    offsets = {rule: main_offset + spacing * (i + 1) for i, rule in enumerate(rules)}
    saved = [
        (i + (offsets[pair[5]] if len(rules) > 1 else 0), pair[2], colors[pair[5]])
        for i, group in enumerate(groups, 1) for pair in group if pair[2] is not None
    ]
    saved_bars = None
    if saved:
        saved_ax = ax.twinx()
        saved_ax.set_ylim(0, 100)
        saved_ax.set_ylabel("Time saved (%)")
        saved_bars = saved_ax.bar(
            [i for i, percent, color in saved], [percent for i, percent, color in saved],
            width=0.65 if len(rules) == 1 else spacing * 0.85,
            color="tab:green" if len(rules) == 1 else [color for i, percent, color in saved],
            alpha=0.2, label="Time saved",
        )
        # Keep the estimates above the bars while letting the bars show through.
        ax.set_zorder(saved_ax.get_zorder() + 1)
        ax.patch.set_visible(False)
    series = [("current adjudication", "tab:blue", main_offset,
               [(i, group[0][0]) for i, group in enumerate(groups, 1)])]
    for rule in rules:
        series.append((rule, colors[rule], offsets[rule], [
            (i, pair[1]) for i, group in enumerate(groups, 1)
            for pair in group if pair[5] == rule
        ]))
    for label, color, offset, points in series:
        values = [value for i, value in points]
        positions = [i + offset for i, value in points]
        lower_sigma = [(value - low) / 1.959963984540054 for value, low, high in values]
        upper_sigma = [(high - value) / 1.959963984540054 for value, low, high in values]
        boxes = ax.bxp(
            [
                dict(med=value, q1=value - lower, q3=value + upper,
                     whislo=low, whishi=high)
                for (value, low, high), lower, upper
                in zip(values, lower_sigma, upper_sigma)
            ],
            positions=positions, widths=spacing * 0.65,
            patch_artist=True, showfliers=False, manage_ticks=False,
            boxprops=dict(facecolor=to_rgba(color, 0.25), edgecolor=color, linewidth=1.2),
            medianprops=dict(color=color, linewidth=1.5),
            whiskerprops=dict(color=color, linewidth=1.2),
            capprops=dict(color=color, linewidth=1.2), zorder=3,
        )
        boxes["boxes"][0].set_label(label)
    ax.axhline(0, color="0.5", linewidth=0.8, linestyle="--")
    ax.set_xticks(
        range(1, len(groups) + 1),
        [
            (group[0][3][:6] if group[0][3] else str(i))
            + ("\n" + group[0][4][0] if group[0][4] else "")
            for i, group in enumerate(groups, 1)
        ],
        rotation=45,
        ha="right",
    )
    for tick, group in zip(ax.get_xticklabels(), groups):
        tick.set_color(group[0][4][1] if group[0][4] else "black")
    ax.set_xlabel("SPRT bounds (sorted by main nElo)" if labeled_runs
                  else "Reader run (sorted by main nElo)")
    ax.set_ylabel("Normalized Elo")
    ax.set_title("nElo estimates: boxes ±1σ, whiskers 95% CI")
    ax.grid(axis="y", alpha=0.25)
    handles, labels = ax.get_legend_handles_labels()
    if saved_bars is not None:
        handles.append(saved_bars)
        labels.append("Time saved")
    ax.legend(handles, labels, loc="upper left", fontsize=8)
    fig.tight_layout()
    if args.output:
        fig.savefig(args.output, dpi=180)
    else:
        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    main()
