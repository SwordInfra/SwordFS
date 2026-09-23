#!/usr/bin/env python3
"""Build the stable main-branch pjdfstest status page and history."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib


def load_history(path: pathlib.Path) -> list[dict[str, object]]:
    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("history must be a JSON array")
    return payload


def percent(value: object) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2f}%"


def trend_history(history: list[dict[str, object]]) -> list[dict[str, object]]:
    metric_names = (
        "overall_support_percent",
        "supported_regression_pass_percent",
        "classified_rate_percent",
    )
    trend: list[dict[str, object]] = []
    previous_metrics: tuple[object, ...] | None = None
    for item in history:
        metrics = tuple(item.get(name) for name in metric_names)
        if any(value is None for value in metrics):
            continue
        if metrics != previous_metrics:
            trend.append(item)
            previous_metrics = metrics
    return trend


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--history", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--swordfs-commit", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--infra-failure", default="")
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    history = load_history(args.history)
    timestamp = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()

    result: dict[str, object] | None = None
    if args.result and args.result.exists():
        result = json.loads(args.result.read_text(encoding="utf-8"))

    if result:
        metadata = result.get("metadata", {})
        summary = result.get("summary", {})
        assert isinstance(metadata, dict) and isinstance(summary, dict)
        status = "PASS" if int(summary.get("blocking_count", 0)) == 0 else "FAILED"
        entry = {
            "timestamp": timestamp,
            "swordfs_commit": metadata.get("swordfs_commit", args.swordfs_commit),
            "pjdfstest_commit": metadata.get("pjdfstest_commit", "unknown"),
            "status": status,
            "overall_support_percent": summary.get("overall_support_percent"),
            "supported_regression_pass_percent": summary.get("supported_regression_pass_percent"),
            "classified_rate_percent": summary.get("classified_rate_percent"),
            "supported_set_size": summary.get("supported_set_size", 0),
            "blocking_count": summary.get("blocking_count", 0),
            "counts": summary.get("counts", {}),
            "by_category": summary.get("by_category", {}),
            "run_url": args.run_url,
        }
        latest = result
        if args.report and args.report.exists():
            current_markdown = args.report.read_text(encoding="utf-8").rstrip()
        else:
            current_markdown = f"# SwordFS POSIX conformance status\n\nLatest run: **{status}**"
    else:
        status = "INFRASTRUCTURE_FAILED"
        entry = {
            "timestamp": timestamp,
            "swordfs_commit": args.swordfs_commit,
            "pjdfstest_commit": "unknown",
            "status": status,
            "overall_support_percent": None,
            "supported_regression_pass_percent": None,
            "classified_rate_percent": None,
            "supported_set_size": 0,
            "blocking_count": None,
            "counts": {},
            "by_category": {},
            "run_url": args.run_url,
        }
        latest = {"metadata": {"swordfs_commit": args.swordfs_commit}, "status": status, "reason": args.infra_failure}
        current_markdown = "\n".join(
            [
                "# SwordFS POSIX conformance status",
                "",
                "Latest main conformance run: **INFRASTRUCTURE FAILED**",
                "",
                args.infra_failure or "The conformance job ended before a classified report was produced.",
                "",
                f"CI run: {args.run_url}",
            ]
        )

    # A rerun for the same commit/run replaces the previous entry rather than
    # producing duplicate history points.
    history = [
        item
        for item in history
        if not (
            item.get("swordfs_commit") == entry["swordfs_commit"]
            and item.get("run_url") == entry["run_url"]
        )
    ]
    history.append(entry)
    history = history[-200:]

    healthy = next((item for item in reversed(history) if item.get("status") == "PASS"), None)
    trend_lines = [
        "",
        f"Authoritative CI run: {args.run_url}",
        f"Recorded at: {timestamp}",
        "",
        "## Historical main-branch trend",
        "",
        "| Time | SwordFS | Status | Overall support | Supported gate | Classified |",
        "| --- | --- | --- | ---: | ---: | ---: |",
    ]
    for item in trend_history(history)[-30:]:
        commit = str(item.get("swordfs_commit", "unknown"))
        trend_lines.append(
            "| {timestamp} | `{commit}` | {status} | {overall} | {supported} | {classified} |".format(
                timestamp=item.get("timestamp", "unknown"),
                commit=commit[:12],
                status=item.get("status", "unknown"),
                overall=percent(item.get("overall_support_percent")),
                supported=percent(item.get("supported_regression_pass_percent")),
                classified=percent(item.get("classified_rate_percent")),
            )
        )
    if healthy:
        trend_lines.extend(
            [
                "",
                f"Last fully healthy baseline: `{str(healthy.get('swordfs_commit', 'unknown'))}`",
            ]
        )

    (args.output_dir / "status.md").write_text(current_markdown + "\n" + "\n".join(trend_lines) + "\n", encoding="utf-8")
    (args.output_dir / "history.json").write_text(json.dumps(history, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.output_dir / "latest.json").write_text(json.dumps(latest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
