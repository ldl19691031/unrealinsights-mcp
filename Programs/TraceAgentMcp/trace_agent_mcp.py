#!/usr/bin/env python
from __future__ import annotations

import fnmatch
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CLI = REPO_ROOT / "Binaries" / "Win64" / "TraceAgentCli.exe"
CLI_PATH = Path(os.environ.get("TRACE_AGENT_CLI", str(DEFAULT_CLI)))
DEFAULT_TRACE_FILE = os.environ.get("TRACE_AGENT_DEFAULT_TRACE", "")
DEFAULT_PROTOCOL_VERSION = "2024-11-05"
FRAME_QUERY_LIMIT = 500000
SUMMARY_CACHE: dict[str, dict[str, Any]] = {}
FRAME_CACHE: dict[tuple[str, str], list[dict[str, Any]]] = {}


TOOLS: list[dict[str, Any]] = [
    {
        "name": "summary",
        "description": "Summarize a .utrace file and return top-level counts for tracks, timelines, counters, and bookmarks.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "list_tracks",
        "description": "List timing-related tracks available in a .utrace file, including CPU threads, GPU queues, and frame tracks.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "find_tracks",
        "description": "Find tracks by id, exact name, wildcard name pattern, kind, or group.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."},
                "track_id": {"type": "string", "description": "Exact track id to match."},
                "name": {"type": "string", "description": "Exact track name to match."},
                "name_pattern": {"type": "string", "description": "Wildcard pattern matched against track names, for example *Game*."},
                "kind": {"type": "string", "description": "Track kind, for example cpu_thread, gpu_queue, gpu_legacy, verse, or frame."},
                "group_pattern": {"type": "string", "description": "Wildcard pattern matched against the optional track group field."}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "list_timers",
        "description": "List timing timer definitions present in a .utrace file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "query_timing_events",
        "description": "Query timing events from a specific track or from all tracks in a .utrace file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."},
                "track_id": {"type": "string", "description": "Track id from list_tracks, for example cpu_thread:2."},
                "track_name": {"type": "string", "description": "Exact track name to resolve before querying."},
                "track_name_pattern": {"type": "string", "description": "Wildcard track-name match used when resolving a track automatically."},
                "track_kind": {"type": "string", "description": "Optional track kind used together with track_name or track_name_pattern."},
                "start_time": {"type": "number", "description": "Start time in seconds."},
                "end_time": {"type": "number", "description": "End time in seconds."},
                "range_start_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Start of a relative time range as a percentage of total trace duration."},
                "range_end_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "End of a relative time range as a percentage of total trace duration."},
                "head_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the first N percent of the trace."},
                "tail_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the last N percent of the trace."},
                "frame_type": {"type": "string", "enum": ["game", "rendering"], "description": "Frame type used when resolving frame-based ranges. Defaults to game."},
                "frame_number": {"type": "integer", "minimum": 1, "description": "1-based start frame number used to resolve a time window."},
                "end_frame_number": {"type": "integer", "minimum": 1, "description": "1-based inclusive end frame number used to resolve a time window."},
                "frame_count": {"type": "integer", "minimum": 1, "description": "Number of frames starting at frame_number to include."},
                "head_frame_count": {"type": "integer", "minimum": 1, "description": "Select the first N frames from the chosen frame track."},
                "tail_frame_count": {"type": "integer", "minimum": 1, "description": "Select the last N frames from the chosen frame track."},
                "limit": {"type": "integer", "minimum": 1, "description": "Maximum number of events to return."},
                "timer_ids": {
                    "type": "array",
                    "items": {"type": "integer", "minimum": 0},
                    "description": "Optional timer id filter.",
                },
                "timer_name": {"type": "string", "description": "Optional wildcard timer-name filter."},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "query_frames",
        "description": "Query frame intervals from the built-in frame tracks without needing to know the frame track id.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."},
                "frame_type": {"type": "string", "enum": ["game", "rendering"], "description": "Frame track type."},
                "start_time": {"type": "number", "description": "Start time in seconds."},
                "end_time": {"type": "number", "description": "End time in seconds."},
                "range_start_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Start of a relative time range as a percentage of total trace duration."},
                "range_end_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "End of a relative time range as a percentage of total trace duration."},
                "head_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the first N percent of the trace."},
                "tail_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the last N percent of the trace."},
                "frame_number": {"type": "integer", "minimum": 1, "description": "1-based start frame number."},
                "end_frame_number": {"type": "integer", "minimum": 1, "description": "1-based inclusive end frame number."},
                "frame_count": {"type": "integer", "minimum": 1, "description": "Number of frames starting at frame_number to include."},
                "head_frame_count": {"type": "integer", "minimum": 1, "description": "Select the first N frames."},
                "tail_frame_count": {"type": "integer", "minimum": 1, "description": "Select the last N frames."},
                "limit": {"type": "integer", "minimum": 1, "description": "Maximum number of frames to return."}
            },
            "required": ["frame_type"],
            "additionalProperties": False,
        },
    },
    {
        "name": "repo_info",
        "description": "Return server-side repository, CLI, Python, and default-trace configuration details for debugging MCP setup.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
    },
    {
        "name": "list_counters",
        "description": "List counters present in a .utrace file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "query_counter_values",
        "description": "Query counter samples or operations for a specific counter in a .utrace file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."},
                "counter_id": {"type": "integer", "minimum": 0, "description": "Counter id from list_counters."},
                "start_time": {"type": "number", "description": "Start time in seconds."},
                "end_time": {"type": "number", "description": "End time in seconds."},
                "range_start_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Start of a relative time range as a percentage of total trace duration."},
                "range_end_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "End of a relative time range as a percentage of total trace duration."},
                "head_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the first N percent of the trace."},
                "tail_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the last N percent of the trace."},
                "frame_type": {"type": "string", "enum": ["game", "rendering"], "description": "Frame type used when resolving frame-based ranges. Defaults to game."},
                "frame_number": {"type": "integer", "minimum": 1, "description": "1-based start frame number used to resolve a time window."},
                "end_frame_number": {"type": "integer", "minimum": 1, "description": "1-based inclusive end frame number used to resolve a time window."},
                "frame_count": {"type": "integer", "minimum": 1, "description": "Number of frames starting at frame_number to include."},
                "head_frame_count": {"type": "integer", "minimum": 1, "description": "Select the first N frames from the chosen frame track."},
                "tail_frame_count": {"type": "integer", "minimum": 1, "description": "Select the last N frames from the chosen frame track."},
                "limit": {"type": "integer", "minimum": 1, "description": "Maximum number of rows to return."},
                "ops": {"type": "boolean", "description": "Return raw counter operations instead of sampled values."},
            },
            "required": ["counter_id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "query_bookmarks",
        "description": "Query bookmarks from a .utrace file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "trace_file": {"type": "string", "description": "Absolute path to the .utrace file. Optional if TRACE_AGENT_DEFAULT_TRACE is configured."},
                "start_time": {"type": "number", "description": "Start time in seconds."},
                "end_time": {"type": "number", "description": "End time in seconds."},
                "range_start_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Start of a relative time range as a percentage of total trace duration."},
                "range_end_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "End of a relative time range as a percentage of total trace duration."},
                "head_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the first N percent of the trace."},
                "tail_percent": {"type": "number", "minimum": 0, "maximum": 100, "description": "Select the last N percent of the trace."},
                "frame_type": {"type": "string", "enum": ["game", "rendering"], "description": "Frame type used when resolving frame-based ranges. Defaults to game."},
                "frame_number": {"type": "integer", "minimum": 1, "description": "1-based start frame number used to resolve a time window."},
                "end_frame_number": {"type": "integer", "minimum": 1, "description": "1-based inclusive end frame number used to resolve a time window."},
                "frame_count": {"type": "integer", "minimum": 1, "description": "Number of frames starting at frame_number to include."},
                "head_frame_count": {"type": "integer", "minimum": 1, "description": "Select the first N frames from the chosen frame track."},
                "tail_frame_count": {"type": "integer", "minimum": 1, "description": "Select the last N frames from the chosen frame track."},
                "limit": {"type": "integer", "minimum": 1, "description": "Maximum number of bookmarks to return."},
                "pattern": {"type": "string", "description": "Optional wildcard pattern matched against bookmark text."},
            },
            "additionalProperties": False,
        },
    },
]


def read_message() -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        key, _, value = line.decode("utf-8").partition(":")
        headers[key.strip().lower()] = value.strip()

    content_length = headers.get("content-length")
    if content_length is None:
        raise RuntimeError("Missing Content-Length header.")

    payload = sys.stdin.buffer.read(int(content_length))
    return json.loads(payload.decode("utf-8"))


def write_message(message: dict[str, Any]) -> None:
    encoded = json.dumps(message, ensure_ascii=False).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(encoded)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(encoded)
    sys.stdout.buffer.flush()


def make_text_result(payload: Any, is_error: bool = False) -> dict[str, Any]:
    text = json.dumps(payload, ensure_ascii=False)
    result: dict[str, Any] = {
        "content": [
            {
                "type": "text",
                "text": text,
            }
        ]
    }
    if not is_error and isinstance(payload, dict):
        result["structuredContent"] = payload
    if is_error:
        result["isError"] = True
    return result


def resolve_trace_file(arguments: dict[str, Any]) -> str:
    trace_file = arguments.get("trace_file")
    if trace_file is None or trace_file == "":
        trace_file = DEFAULT_TRACE_FILE
    if not isinstance(trace_file, str) or not trace_file:
        raise ValueError("trace_file is required unless TRACE_AGENT_DEFAULT_TRACE is configured.")
    return trace_file


def run_process(args: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=str(cwd or REPO_ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        env=env,
    )


def run_cli(command: str, arguments: dict[str, Any]) -> dict[str, Any]:
    trace_file = resolve_trace_file(arguments)
    if not CLI_PATH.exists():
        raise RuntimeError(f"TraceAgentCli not found: {CLI_PATH}")

    cli_args = [str(CLI_PATH), "--json-only", command, f"-i={trace_file}"]

    if command == "query-timing-events":
        if isinstance(arguments.get("track_id"), str) and arguments["track_id"]:
            cli_args.append(f"-track={arguments['track_id']}")
        if "start_time" in arguments:
            cli_args.append(f"-start={arguments['start_time']}")
        if "end_time" in arguments:
            cli_args.append(f"-end={arguments['end_time']}")
        if "limit" in arguments:
            cli_args.append(f"-limit={arguments['limit']}")
        if isinstance(arguments.get("timer_name"), str) and arguments["timer_name"]:
            cli_args.append(f"-timer-name={arguments['timer_name']}")
        timer_ids = arguments.get("timer_ids")
        if isinstance(timer_ids, list) and timer_ids:
            cli_args.append("-timer-ids=" + ",".join(str(int(item)) for item in timer_ids))
    elif command == "query-counter-values":
        counter_id = arguments.get("counter_id")
        if not isinstance(counter_id, int):
            raise ValueError("counter_id is required and must be an integer.")
        cli_args.append(f"-counter={counter_id}")
        if "start_time" in arguments:
            cli_args.append(f"-start={arguments['start_time']}")
        if "end_time" in arguments:
            cli_args.append(f"-end={arguments['end_time']}")
        if "limit" in arguments:
            cli_args.append(f"-limit={arguments['limit']}")
        if arguments.get("ops") is True:
            cli_args.append("-ops")
    elif command == "query-bookmarks":
        if "start_time" in arguments:
            cli_args.append(f"-start={arguments['start_time']}")
        if "end_time" in arguments:
            cli_args.append(f"-end={arguments['end_time']}")
        if "limit" in arguments:
            cli_args.append(f"-limit={arguments['limit']}")
        if isinstance(arguments.get("pattern"), str) and arguments["pattern"]:
            cli_args.append(f"-pattern={arguments['pattern']}")

    completed = run_process(cli_args, cwd=REPO_ROOT)

    stdout = completed.stdout.strip()
    stderr = completed.stderr.strip()

    if stdout:
        try:
            payload = json.loads(stdout)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"TraceAgentCli returned non-JSON stdout. returncode={completed.returncode}, stderr={stderr!r}, stdout={stdout!r}"
            ) from exc
    else:
        payload = {
            "ok": completed.returncode == 0,
            "returncode": completed.returncode,
            "stderr": stderr,
        }

    if completed.returncode != 0 and not isinstance(payload, dict):
        raise RuntimeError(f"TraceAgentCli failed. returncode={completed.returncode}, stderr={stderr!r}")

    if isinstance(payload, dict):
        payload.setdefault("cli_returncode", completed.returncode)
        if stderr:
            payload.setdefault("cli_stderr", stderr)

    return payload


def get_summary_payload(arguments: dict[str, Any]) -> dict[str, Any]:
    trace_file = resolve_trace_file(arguments)
    cached = SUMMARY_CACHE.get(trace_file)
    if cached is not None:
        return cached
    payload = run_cli("summary", {"trace_file": trace_file})
    if not payload.get("ok", False):
        raise RuntimeError(payload.get("error", "summary failed"))
    SUMMARY_CACHE[trace_file] = payload
    return payload


def get_trace_duration(arguments: dict[str, Any]) -> float:
    payload = get_summary_payload(arguments)
    duration = payload.get("duration_seconds")
    if not isinstance(duration, (int, float)):
        raise RuntimeError("summary did not return duration_seconds")
    return float(duration)


def get_frames_for_type(arguments: dict[str, Any], frame_type: str) -> list[dict[str, Any]]:
    trace_file = resolve_trace_file(arguments)
    cache_key = (trace_file, frame_type)
    cached = FRAME_CACHE.get(cache_key)
    if cached is not None:
        return cached

    payload = run_cli(
        "query-timing-events",
        {
            "trace_file": trace_file,
            "track_id": f"frame:{frame_type}",
            "limit": FRAME_QUERY_LIMIT,
        },
    )
    if not payload.get("ok", False):
        raise RuntimeError(payload.get("error", "query frame track failed"))
    events = payload.get("events")
    if not isinstance(events, list):
        raise RuntimeError("frame query did not return an events array")
    FRAME_CACHE[cache_key] = events
    return events


def _has_any(arguments: dict[str, Any], keys: tuple[str, ...]) -> bool:
    return any(key in arguments and arguments[key] not in (None, "") for key in keys)


def resolve_time_window(arguments: dict[str, Any], *, default_frame_type: str | None = "game") -> dict[str, Any]:
    absolute_keys = ("start_time", "end_time")
    percent_keys = ("range_start_percent", "range_end_percent", "head_percent", "tail_percent")
    frame_keys = ("frame_number", "end_frame_number", "frame_count", "head_frame_count", "tail_frame_count")

    has_absolute = _has_any(arguments, absolute_keys)
    has_percent = _has_any(arguments, percent_keys)
    has_frame = _has_any(arguments, frame_keys)

    selector_group_count = sum(1 for enabled in (has_absolute, has_percent, has_frame) if enabled)
    if selector_group_count > 1:
        raise ValueError("Use only one time selector style at once: absolute time, percentage range, or frame range.")

    if has_absolute:
        window: dict[str, Any] = {"source": "absolute"}
        if "start_time" in arguments:
            window["start_time"] = float(arguments["start_time"])
        if "end_time" in arguments:
            window["end_time"] = float(arguments["end_time"])
        return window

    if has_percent:
        duration = get_trace_duration(arguments)
        head_percent = arguments.get("head_percent")
        tail_percent = arguments.get("tail_percent")
        range_start_percent = arguments.get("range_start_percent")
        range_end_percent = arguments.get("range_end_percent")

        if head_percent not in (None, "") and tail_percent not in (None, ""):
            raise ValueError("head_percent and tail_percent cannot be used together.")
        if (head_percent not in (None, "") or tail_percent not in (None, "")) and (
            range_start_percent not in (None, "") or range_end_percent not in (None, "")
        ):
            raise ValueError("head_percent or tail_percent cannot be combined with range_start_percent/range_end_percent.")

        if head_percent not in (None, ""):
            start_percent = 0.0
            end_percent = float(head_percent)
            source = "head_percent"
        elif tail_percent not in (None, ""):
            start_percent = 100.0 - float(tail_percent)
            end_percent = 100.0
            source = "tail_percent"
        else:
            if range_start_percent in (None, "") or range_end_percent in (None, ""):
                raise ValueError("range_start_percent and range_end_percent must be provided together.")
            start_percent = float(range_start_percent)
            end_percent = float(range_end_percent)
            source = "percent_range"

        if not (0.0 <= start_percent <= 100.0 and 0.0 <= end_percent <= 100.0):
            raise ValueError("Percentage values must be between 0 and 100.")
        if end_percent < start_percent:
            raise ValueError("End percentage must be greater than or equal to start percentage.")

        return {
            "source": source,
            "start_time": duration * start_percent / 100.0,
            "end_time": duration * end_percent / 100.0,
            "duration_seconds": duration,
            "start_percent": start_percent,
            "end_percent": end_percent,
        }

    if has_frame:
        frame_type = arguments.get("frame_type") or default_frame_type
        if frame_type not in {"game", "rendering"}:
            raise ValueError("frame_type must be either 'game' or 'rendering'.")

        frames = get_frames_for_type(arguments, frame_type)
        if not frames:
            raise RuntimeError(f"No frames found for frame_type={frame_type}.")

        frame_number = arguments.get("frame_number")
        end_frame_number = arguments.get("end_frame_number")
        frame_count = arguments.get("frame_count")
        head_frame_count = arguments.get("head_frame_count")
        tail_frame_count = arguments.get("tail_frame_count")

        if head_frame_count not in (None, "") and tail_frame_count not in (None, ""):
            raise ValueError("head_frame_count and tail_frame_count cannot be used together.")
        if (head_frame_count not in (None, "") or tail_frame_count not in (None, "")) and (
            frame_number not in (None, "") or end_frame_number not in (None, "") or frame_count not in (None, "")
        ):
            raise ValueError("head_frame_count/tail_frame_count cannot be combined with frame_number/end_frame_number/frame_count.")

        selected_frames: list[dict[str, Any]]
        source: str
        if head_frame_count not in (None, ""):
            selected_frames = frames[: int(head_frame_count)]
            source = "head_frame_count"
        elif tail_frame_count not in (None, ""):
            count = int(tail_frame_count)
            selected_frames = frames[-count:] if count > 0 else []
            source = "tail_frame_count"
        else:
            if frame_number in (None, ""):
                raise ValueError("frame_number is required when selecting a specific frame or frame span.")
            start_index = int(frame_number) - 1
            if start_index < 0:
                raise ValueError("frame_number must be at least 1.")

            if end_frame_number not in (None, "") and frame_count not in (None, ""):
                raise ValueError("Use either end_frame_number or frame_count, not both.")

            if end_frame_number not in (None, ""):
                end_index = int(end_frame_number) - 1
                source = "frame_number_range"
            elif frame_count not in (None, ""):
                end_index = start_index + int(frame_count) - 1
                source = "frame_number_count"
            else:
                end_index = start_index
                source = "single_frame"

            if end_index < start_index:
                raise ValueError("end_frame_number must be greater than or equal to frame_number.")

            selected_frames = [frame for frame in frames if start_index <= int(frame.get("frame_index", -1)) <= end_index]

        if not selected_frames:
            raise RuntimeError("No frames matched the supplied frame selector.")

        first_frame = selected_frames[0]
        last_frame = selected_frames[-1]
        return {
            "source": source,
            "frame_type": frame_type,
            "start_time": float(first_frame["start_time"]),
            "end_time": float(last_frame["end_time"]),
            "start_frame_index": int(first_frame["frame_index"]),
            "end_frame_index": int(last_frame["frame_index"]),
            "frame_count": len(selected_frames),
        }

    return {"source": "full_trace"}


def get_tracks(arguments: dict[str, Any]) -> list[dict[str, Any]]:
    payload = run_cli("list-tracks", arguments)
    if not payload.get("ok", False):
        raise RuntimeError(payload.get("error", "list-tracks failed"))
    tracks = payload.get("tracks")
    if not isinstance(tracks, list):
        raise RuntimeError("list-tracks returned no tracks array")
    return tracks


def match_pattern(value: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(value.lower(), pattern.lower())


def filter_tracks(tracks: list[dict[str, Any]], arguments: dict[str, Any]) -> list[dict[str, Any]]:
    track_id = arguments.get("track_id")
    exact_name = arguments.get("name")
    name_pattern = arguments.get("name_pattern")
    kind = arguments.get("kind")
    group_pattern = arguments.get("group_pattern")

    matched: list[dict[str, Any]] = []
    for track in tracks:
        if isinstance(track_id, str) and track.get("track_id") != track_id:
            continue
        if isinstance(exact_name, str) and track.get("name") != exact_name:
            continue
        if isinstance(name_pattern, str) and not match_pattern(str(track.get("name", "")), name_pattern):
            continue
        if isinstance(kind, str) and str(track.get("kind", "")).lower() != kind.lower():
            continue
        if isinstance(group_pattern, str) and not match_pattern(str(track.get("group", "")), group_pattern):
            continue
        matched.append(track)
    return matched


def resolve_single_track(arguments: dict[str, Any]) -> dict[str, Any]:
    tracks = get_tracks(arguments)
    criteria = {
        "track_id": arguments.get("track_id"),
        "name": arguments.get("track_name"),
        "name_pattern": arguments.get("track_name_pattern"),
        "kind": arguments.get("track_kind"),
    }
    filtered = filter_tracks(tracks, criteria)
    if not filtered:
        raise RuntimeError("No track matched the supplied track selector.")
    if len(filtered) > 1:
        raise RuntimeError(
            "Track selector matched multiple tracks: "
            + ", ".join(str(track.get("track_id", "")) for track in filtered[:8])
        )
    return filtered[0]


def handle_find_tracks(arguments: dict[str, Any]) -> dict[str, Any]:
    tracks = get_tracks(arguments)
    filtered = filter_tracks(tracks, arguments)
    return {
        "ok": True,
        "command": "find-tracks",
        "trace_file": resolve_trace_file(arguments),
        "tracks": filtered,
        "returned": len(filtered),
    }


def handle_query_frames(arguments: dict[str, Any]) -> dict[str, Any]:
    frame_type = arguments.get("frame_type")
    if not isinstance(frame_type, str) or frame_type not in {"game", "rendering"}:
        raise ValueError("frame_type must be either 'game' or 'rendering'.")

    query_args = {
        "trace_file": resolve_trace_file(arguments),
        "track_id": f"frame:{frame_type}",
    }
    resolved_window = resolve_time_window(arguments, default_frame_type=frame_type)
    for key in ("start_time", "end_time"):
        if key in resolved_window:
            query_args[key] = resolved_window[key]
    if "limit" in arguments:
        query_args["limit"] = arguments["limit"]
    payload = run_cli("query-timing-events", query_args)
    payload["resolved_window"] = resolved_window
    return payload


def handle_query_timing_events(arguments: dict[str, Any]) -> dict[str, Any]:
    query_args = dict(arguments)
    query_args["trace_file"] = resolve_trace_file(arguments)

    if not isinstance(query_args.get("track_id"), str) and (
        isinstance(query_args.get("track_name"), str) or isinstance(query_args.get("track_name_pattern"), str)
    ):
        track = resolve_single_track(arguments)
        query_args["track_id"] = track["track_id"]
        query_args["resolved_track"] = track

    resolved_window = resolve_time_window(arguments, default_frame_type="game")
    for key in ("start_time", "end_time"):
        if key in resolved_window:
            query_args[key] = resolved_window[key]

    payload = run_cli("query-timing-events", query_args)
    payload["resolved_window"] = resolved_window
    if "resolved_track" in query_args:
        payload["resolved_track"] = query_args["resolved_track"]
    return payload


def handle_query_counter_values(arguments: dict[str, Any]) -> dict[str, Any]:
    query_args = dict(arguments)
    query_args["trace_file"] = resolve_trace_file(arguments)
    resolved_window = resolve_time_window(arguments, default_frame_type="game")
    for key in ("start_time", "end_time"):
        if key in resolved_window:
            query_args[key] = resolved_window[key]
    payload = run_cli("query-counter-values", query_args)
    payload["resolved_window"] = resolved_window
    return payload


def handle_query_bookmarks(arguments: dict[str, Any]) -> dict[str, Any]:
    query_args = dict(arguments)
    query_args["trace_file"] = resolve_trace_file(arguments)
    resolved_window = resolve_time_window(arguments, default_frame_type="game")
    for key in ("start_time", "end_time"):
        if key in resolved_window:
            query_args[key] = resolved_window[key]
    payload = run_cli("query-bookmarks", query_args)
    payload["resolved_window"] = resolved_window
    return payload


def handle_repo_info(_: dict[str, Any]) -> dict[str, Any]:
    return {
        "ok": True,
        "repo_root": str(REPO_ROOT),
        "cli_path": str(CLI_PATH),
        "cli_exists": CLI_PATH.exists(),
        "default_trace_file": DEFAULT_TRACE_FILE,
        "python": sys.executable,
    }


def handle_request(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    request_id = message.get("id")

    if method == "notifications/initialized":
        return None

    if method == "initialize":
        client_protocol = message.get("params", {}).get("protocolVersion", DEFAULT_PROTOCOL_VERSION)
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "protocolVersion": client_protocol,
                "capabilities": {
                    "tools": {
                        "listChanged": False,
                    }
                },
                "serverInfo": {
                    "name": "trace-agent-mcp",
                    "version": "0.1.0",
                },
            },
        }

    if method == "ping":
        return {"jsonrpc": "2.0", "id": request_id, "result": {}}

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": request_id, "result": {"tools": TOOLS}}

    if method == "tools/call":
        params = message.get("params", {})
        tool_name = params.get("name")
        arguments = params.get("arguments", {})
        if not isinstance(arguments, dict):
            arguments = {}

        command_map = {
            "summary": "summary",
            "list_tracks": "list-tracks",
            "find_tracks": "__find_tracks__",
            "list_timers": "list-timers",
            "query_timing_events": "__query_timing_events__",
            "query_frames": "__query_frames__",
            "list_counters": "list-counters",
            "query_counter_values": "__query_counter_values__",
            "query_bookmarks": "__query_bookmarks__",
            "repo_info": "__repo_info__",
        }

        if tool_name not in command_map:
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": make_text_result({"ok": False, "error": f"Unknown tool: {tool_name}"}, is_error=True),
            }

        try:
            command = command_map[tool_name]
            if command == "__find_tracks__":
                payload = handle_find_tracks(arguments)
            elif command == "__query_timing_events__":
                payload = handle_query_timing_events(arguments)
            elif command == "__query_frames__":
                payload = handle_query_frames(arguments)
            elif command == "__query_counter_values__":
                payload = handle_query_counter_values(arguments)
            elif command == "__query_bookmarks__":
                payload = handle_query_bookmarks(arguments)
            elif command == "__repo_info__":
                payload = handle_repo_info(arguments)
            else:
                payload = run_cli(command, arguments)
            is_error = isinstance(payload, dict) and payload.get("ok") is False
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": make_text_result(payload, is_error=is_error),
            }
        except Exception as exc:  # noqa: BLE001
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": make_text_result({"ok": False, "error": str(exc)}, is_error=True),
            }

    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


def main() -> int:
    while True:
        message = read_message()
        if message is None:
            return 0
        response = handle_request(message)
        if response is not None:
            write_message(response)


if __name__ == "__main__":
    raise SystemExit(main())
