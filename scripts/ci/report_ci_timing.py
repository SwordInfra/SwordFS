#!/usr/bin/env python3
"""Summarize completed GitHub CI jobs, steps and fstests XUnit timing."""

import datetime
import json
import os
import pathlib
import statistics
import sys
import urllib.request
import xml.etree.ElementTree as ET


def parse_time(value):
    if not value:
        return None
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))


def elapsed(start, end):
    begin, finish = parse_time(start), parse_time(end)
    return max(0.0, (finish - begin).total_seconds()) if begin and finish else 0.0


def get_json(api, path):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{api}/{path}",
        headers={"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}",
                 "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def report_cases(root):
    lines = ["", "### fstests per-shard testcase times (XUnit)", "",
             "| Shard | Reported test time | Cases | Slowest cases |", "|---|---:|---:|---|"]
    for directory in sorted(root.glob("fstests-conformance-*")):
        result = directory / "raw/results/result.xml"
        if not result.is_file():
            lines.append(f"| {directory.name} | unavailable | - | missing XUnit |")
            continue
        cases = []
        for node in ET.parse(result).getroot().iter("testcase"):
            cases.append((node.attrib.get("name", ""), float(node.attrib.get("time", 0))))
        longest = sorted(cases, key=lambda x: -x[1])[:3]
        items = ", ".join(f"`{test}` {seconds:.0f}s" for test, seconds in longest)
        lines.append(f"| {directory.name.removeprefix('fstests-conformance-')} | "
                     f"{sum(seconds for _, seconds in cases):.0f}s | {len(cases)} | {items} |")
    return lines


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def historical_trends(repository, current_id):
    # A completed docs-only CI normally takes less than a minute. Require a
    # full-length run and the full-job topology to avoid mixing populations.
    response = get_json(repository, "actions/workflows/ci.yml/runs?per_page=100&status=completed")
    samples = []
    for run in response["workflow_runs"]:
        if (str(run["id"]) == current_id or run["event"] != "pull_request"
                or run["conclusion"] != "success"):
            continue
        seconds = elapsed(run.get("run_started_at"), run.get("updated_at"))
        if seconds >= 600:
            samples.append(seconds)
        if len(samples) == 12:
            break
    if len(samples) < 10:
        return ["", f"Historical complete full-CI sample is too small ({len(samples)}/10)."]

    recent, older = samples[:5], samples[5:10]
    recent_median, older_median = statistics.median(recent), statistics.median(older)
    warning = recent_median > older_median * 1.2 and recent_median - older_median > 180
    return [
        "", "### Rolling full-CI latency baseline (successful PRs, >=10m)", "",
        f"- {len(samples)} comparable full-run candidates: "
        f"P50 **{percentile(samples, 0.5) / 60:.2f}m**, "
        f"P95 **{percentile(samples, 0.95) / 60:.2f}m**.",
        f"- Last five vs preceding five medians: "
        f"**{recent_median / 60:.2f}m / {older_median / 60:.2f}m**.",
        ("- ⚠️ Sustained latency regression candidate: recent median exceeds the earlier "
         "median by >20% and >3 minutes; investigate queue/dependencies/critical path."
         if warning else "- No sustained latency regression warning from this rolling sample."),
        "- This is a *diagnostic warning*, never a flaky test-time merge gate.",
    ]


def main():
    repository = os.environ["GITHUB_REPOSITORY"]
    run_id = os.environ["GITHUB_RUN_ID"]
    run = get_json(repository, f"actions/runs/{run_id}")
    jobs = []
    for page in range(1, 10):
        result = get_json(repository, f"actions/runs/{run_id}/jobs?per_page=100&page={page}")
        jobs.extend(result["jobs"])
        if len(result["jobs"]) < 100:
            break

    # Only completed steps contribute to timings. The reporting job itself
    # is still running, so the aggregate total is explicitly a lower bound.
    finished = [job for job in jobs if job.get("completed_at") and job.get("started_at")]
    runner_seconds = sum(elapsed(job["started_at"], job["completed_at"]) for job in finished)
    end_times = [parse_time(job["completed_at"]) for job in finished]
    first_start = parse_time(run.get("run_started_at"))
    wall = max(((last - first_start).total_seconds() for last in end_times), default=0) if first_start else 0
    queue = elapsed(run.get("created_at"), run.get("run_started_at"))
    ranked = sorted(finished, key=lambda job: -elapsed(job["started_at"], job["completed_at"]))

    lines = [
        "## CI duration diagnostics", "",
        f"- Workflow: [{run_id}]({run['html_url']})",
        f"- Run queue delay: **{queue:.0f}s**",
        f"- Elapsed since workflow start (before this summary completes): **{wall / 60:.1f}m**",
        f"- Completed hosted runner time (lower bound): **{runner_seconds / 60:.1f} runner-minutes**",
        "", "### Longest completed jobs", "",
        "| Job | Duration | Longest steps |", "|---|---:|---|",
    ]
    for job in ranked[:18]:
        steps = sorted(job.get("steps", []),
                       key=lambda step: -elapsed(step.get("started_at"), step.get("completed_at")))[:3]
        details = ", ".join(f"{step['name']}: {elapsed(step.get('started_at'), step.get('completed_at')):.0f}s"
                            for step in steps)
        lines.append(f"| {job['name']} | {elapsed(job['started_at'], job['completed_at']):.0f}s | {details} |")

    producer = next((job for job in finished if job["name"] == "build-release-core"), None)
    if producer:
        consumers = [job for job in finished if job["name"].startswith("fstests-shard (")
                     or job["name"] == "pjdfstest-conformance (Release)"]
        waits = [elapsed(producer["completed_at"], job["started_at"]) for job in consumers]
        lines.extend(["", f"- Release producer to conformance job-start gap: "
                      f"min {min(waits):.0f}s / max {max(waits):.0f}s" if waits
                      else "- Release producer to conformance job-start gap unavailable"])

    artifact = get_json(repository, f"actions/runs/{run_id}/artifacts?per_page=100")
    release = next((item for item in artifact.get("artifacts", [])
                    if item["name"] == "swordfs-release-core"), None)
    if release:
        lines.append(f"- Release artifact size (uploaded compressed bytes): **{release['size_in_bytes']:,}**")
    lines.extend(report_cases(pathlib.Path(sys.argv[1])))
    lines.extend(historical_trends(repository, run_id))
    output = "\n".join(lines) + "\n"
    print(output)
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
        stream.write(output)


if __name__ == "__main__":
    main()
