#!/usr/bin/env python3
"""
Performance analysis tooling for the llcpp exchange system.

Parses RDTSC and TTT log entries, generates PDF figures and a LaTeX
summary table.

Usage:
    python3 notebooks/perf_analysis.py \
        --log-dir . --cpu-ghz 3.29 \
        --fig-dir perf_output/figures --table-dir perf_output/tables
"""

import argparse
import glob
import os
import re
import sys

import matplotlib
matplotlib.use('Agg')  # non-interactive backend
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def parse_log_files(log_dir: str) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Parse all log files for RDTSC and TTT entries."""
    pattern = re.compile(
        r'(?:^|\s)(\d{2}:\d{2}:\d{2}\.\d+)\s+(RDTSC|TTT)\s+(\S+)\s+(\d+)'
    )

    rdtsc_rows = []
    ttt_rows = []

    log_patterns = ['*.log']
    log_files = []
    for pat in log_patterns:
        log_files.extend(glob.glob(os.path.join(log_dir, pat)))
        log_files.extend(glob.glob(os.path.join(log_dir, '**', pat), recursive=True))

    # Deduplicate
    log_files = sorted(set(log_files))

    for fpath in log_files:
        # Skip build directories
        if '/build' in fpath or '/cmake-build' in fpath:
            continue
        try:
            with open(fpath, 'r', errors='replace') as f:
                for line in f:
                    m = pattern.search(line)
                    if not m:
                        continue
                    time_str, kind, tag, value = m.groups()
                    value = int(value)
                    row = {
                        'time': time_str,
                        'tag': tag,
                        'value': value,
                        'file': os.path.basename(fpath),
                    }
                    if kind == 'RDTSC':
                        rdtsc_rows.append(row)
                    else:
                        ttt_rows.append(row)
        except (OSError, UnicodeDecodeError):
            continue

    rdtsc_df = pd.DataFrame(rdtsc_rows) if rdtsc_rows else pd.DataFrame(
        columns=['time', 'tag', 'value', 'file'])
    ttt_df = pd.DataFrame(ttt_rows) if ttt_rows else pd.DataFrame(
        columns=['time', 'tag', 'value', 'file'])

    return rdtsc_df, ttt_df


def generate_rdtsc_figures(rdtsc_df: pd.DataFrame, cpu_ghz: float,
                           fig_dir: str) -> None:
    """Generate a histogram PDF for each unique RDTSC tag."""
    if rdtsc_df.empty:
        print("  No RDTSC data to plot.")
        return

    rdtsc_df = rdtsc_df.copy()
    rdtsc_df['ns'] = rdtsc_df['value'] / cpu_ghz

    for tag, group in rdtsc_df.groupby('tag'):
        ns_values = group['ns'].values
        fig, ax = plt.subplots(figsize=(6, 4))

        if len(ns_values) >= 5:
            ax.hist(ns_values, bins=min(30, len(ns_values)), edgecolor='black',
                    alpha=0.7, color='steelblue')
        else:
            ax.bar(range(len(ns_values)), ns_values, color='steelblue',
                   edgecolor='black', alpha=0.7)
            ax.set_xlabel('Sample')

        ax.set_title(tag.replace('_', r'\_'), fontsize=10)
        ax.set_ylabel('Nanoseconds' if len(ns_values) < 5 else 'Count')
        if len(ns_values) >= 5:
            ax.set_xlabel('Latency (ns)')

        # Add stats annotation
        stats_text = (f'n={len(ns_values)}\n'
                      f'mean={np.mean(ns_values):.0f} ns\n'
                      f'p50={np.median(ns_values):.0f} ns')
        ax.annotate(stats_text, xy=(0.97, 0.97), xycoords='axes fraction',
                    ha='right', va='top', fontsize=8,
                    bbox=dict(boxstyle='round,pad=0.3', facecolor='wheat',
                              alpha=0.7))

        plt.tight_layout()
        safe_tag = tag.replace('/', '_').replace(' ', '_')
        fpath = os.path.join(fig_dir, f'fig_rdtsc_{safe_tag}.pdf')
        fig.savefig(fpath, bbox_inches='tight')
        plt.close(fig)
        print(f"  Saved: {fpath}")


def generate_ttt_figures(ttt_df: pd.DataFrame, fig_dir: str) -> None:
    """Generate scatter plots for TTT inter-hop transit times."""
    if ttt_df.empty:
        print("  No TTT data to plot.")
        return

    # Group by file, sort by time, compute inter-tag deltas
    for fname, group in ttt_df.groupby('file'):
        group = group.sort_values('time').reset_index(drop=True)
        if len(group) < 2:
            continue

        tags = group['tag'].values
        nanos = group['value'].values

        # Find adjacent pairs with different tags
        pairs = []
        for i in range(len(group) - 1):
            delta = int(nanos[i + 1]) - int(nanos[i])
            if delta > 0 and delta < 1_000_000_000:  # < 1 second
                pairs.append({
                    'from': tags[i],
                    'to': tags[i + 1],
                    'delta_ns': delta,
                    'idx': i,
                })

        if not pairs:
            continue

        pairs_df = pd.DataFrame(pairs)
        for (from_tag, to_tag), pgroup in pairs_df.groupby(['from', 'to']):
            deltas = pgroup['delta_ns'].values
            fig, ax = plt.subplots(figsize=(6, 4))
            ax.scatter(range(len(deltas)), deltas, s=10, alpha=0.6,
                       color='steelblue')

            if len(deltas) >= 10:
                window = max(3, len(deltas) // 10)
                rolling = pd.Series(deltas).rolling(window, min_periods=1).mean()
                ax.plot(range(len(deltas)), rolling, color='red', linewidth=1.5,
                        label=f'Rolling mean (w={window})')
                ax.legend(fontsize=8)

            safe_from = from_tag.replace('/', '_')
            safe_to = to_tag.replace('/', '_')
            ax.set_title(f'{safe_from} -> {safe_to}', fontsize=9)
            ax.set_ylabel('Transit time (ns)')
            ax.set_xlabel('Sample')

            plt.tight_layout()
            fpath = os.path.join(fig_dir,
                                 f'fig_hop_{safe_from}_to_{safe_to}.pdf')
            fig.savefig(fpath, bbox_inches='tight')
            plt.close(fig)
            print(f"  Saved: {fpath}")


def generate_summary_table(rdtsc_df: pd.DataFrame, cpu_ghz: float,
                           table_dir: str) -> None:
    """Generate a booktabs LaTeX summary table."""
    if rdtsc_df.empty:
        print("  No RDTSC data for table.")
        return

    rdtsc_df = rdtsc_df.copy()
    rdtsc_df['ns'] = rdtsc_df['value'] / cpu_ghz

    rows = []
    for tag, group in rdtsc_df.groupby('tag'):
        ns = group['ns'].values
        n = len(ns)
        sorted_ns = np.sort(ns)
        rows.append({
            'tag': tag,
            'count': n,
            'mean': np.mean(ns),
            'p50': np.median(ns),
            'p99': sorted_ns[min(int(n * 0.99), n - 1)],
            'p999': sorted_ns[min(int(n * 0.999), n - 1)],
            'max': np.max(ns),
        })

    rows.sort(key=lambda r: r['mean'], reverse=True)

    lines = [
        r'\begin{tabular}{@{}lrrrrrr@{}}',
        r'\toprule',
        r'Tag & Count & Mean (ns) & P50 (ns) & P99 (ns) & P99.9 (ns) & Max (ns) \\',
        r'\midrule',
    ]

    for r in rows:
        safe_tag = r['tag'].replace('_', r'\_')
        lines.append(
            f"\\texttt{{{safe_tag}}} & {r['count']} & "
            f"{r['mean']:.0f} & {r['p50']:.0f} & {r['p99']:.0f} & "
            f"{r['p999']:.0f} & {r['max']:.0f} \\\\"
        )

    lines.append(r'\bottomrule')
    lines.append(r'\end{tabular}')

    fpath = os.path.join(table_dir, 'latency_summary.tex')
    with open(fpath, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"  Saved: {fpath}")


def main():
    parser = argparse.ArgumentParser(description='Performance analysis tooling')
    parser.add_argument('--log-dir', default='.', help='Directory with log files')
    parser.add_argument('--cpu-ghz', type=float, default=3.29,
                        help='CPU frequency in GHz for cycle->ns conversion')
    parser.add_argument('--fig-dir', default='perf_output/figures',
                        help='Output directory for PDF figures')
    parser.add_argument('--table-dir', default='perf_output/tables',
                        help='Output directory for LaTeX tables')
    args = parser.parse_args()

    os.makedirs(args.fig_dir, exist_ok=True)
    os.makedirs(args.table_dir, exist_ok=True)

    print(f"Parsing logs from: {args.log_dir}")
    rdtsc_df, ttt_df = parse_log_files(args.log_dir)
    print(f"  RDTSC entries: {len(rdtsc_df)}")
    print(f"  TTT entries:   {len(ttt_df)}")

    print("\nGenerating RDTSC figures...")
    generate_rdtsc_figures(rdtsc_df, args.cpu_ghz, args.fig_dir)

    print("\nGenerating TTT figures...")
    generate_ttt_figures(ttt_df, args.fig_dir)

    print("\nGenerating summary table...")
    generate_summary_table(rdtsc_df, args.cpu_ghz, args.table_dir)

    print("\nDone.")


if __name__ == '__main__':
    main()
