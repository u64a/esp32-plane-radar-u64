#!/usr/bin/env python3
"""Build large-airport runway dataset from OurAirports (large_airport only).

Usage:
  python3 scripts/build_large_airports.py
      Downloads from pinned immutable URLs, verifies SHA-256, writes files.

  python3 scripts/build_large_airports.py --airports-csv A --runways-csv B
      Uses local CSV files instead of downloading (both or neither).

  python3 scripts/build_large_airports.py [--airports-csv A --runways-csv B] --check
      Renders in memory and compares exact LF bytes to checked-in files.
      Exits 0 if identical, non-zero otherwise. Never modifies files.

OurAirports data source (immutable pinned commit):
  Repo:    https://github.com/davidmegginson/ourairports-data
  Commit:  79efa72ec1e344d91b081160634fa042a56a21b8
  Date:    2026-06-08T01:53:13Z
  License: Public Domain / The Unlicense
           https://raw.githubusercontent.com/davidmegginson/ourairports-data/79efa72ec1e344d91b081160634fa042a56a21b8/LICENSE

This script does NOT use /main/ URLs for source data; only the pinned commit
hash URL is authoritative. There is no trust-on-first-use or auto-update mode.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "data" / "large_airports.h"
OUT_CPP = ROOT / "src" / "data" / "large_airports_data.cpp"

# --- Pinned immutable source (no /main/ anywhere) ----------------------------

_COMMIT = "79efa72ec1e344d91b081160634fa042a56a21b8"
_COMMIT_DATE = "2026-06-08T01:53:13Z"
_BASE_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/"
    + _COMMIT
)

AIRPORTS_URL = _BASE_URL + "/airports.csv"
RUNWAYS_URL = _BASE_URL + "/runways.csv"
LICENSE_URL = _BASE_URL + "/LICENSE"

# Lowercase hex SHA-256 of the raw bytes at the pinned commit.
AIRPORTS_SHA256 = "092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354"
RUNWAYS_SHA256 = "312f9ded8a5a29f8634bd615b0a7aadd4ed01e773ae63e5aab7c510629440fec"

# Expected byte lengths (verified before decode).
AIRPORTS_LENGTH = 12_651_071
RUNWAYS_LENGTH = 3_951_490

# git blob SHAs (informational, recorded in provenance comments).
AIRPORTS_BLOB = "1df8da39141e2e6adb3c8439b684579da97c19a8"
RUNWAYS_BLOB = "d8fdbbe9a1f7d47ee78eade1ec30f020e3cc9e7d"

# Filter/schema version: increment when filtering logic changes.
FILTER_SCHEMA_VERSION = 1


def _fetch_verified(url: str, expected_sha256: str, expected_length: int) -> bytes:
    """Fetch raw bytes, request identity encoding, verify length and SHA-256."""
    req = urllib.request.Request(url, headers={"Accept-Encoding": "identity"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = resp.read()
    actual_len = len(data)
    if actual_len != expected_length:
        raise ValueError(
            f"Length mismatch for {url}:\n"
            f"  expected {expected_length} bytes\n"
            f"  got      {actual_len} bytes\n"
            "Re-check the pinned commit hash or update the constants."
        )
    actual_sha = hashlib.sha256(data).hexdigest()
    if actual_sha != expected_sha256:
        raise ValueError(
            f"SHA-256 mismatch for {url}:\n"
            f"  expected {expected_sha256}\n"
            f"  got      {actual_sha}\n"
            "The file does not match the pinned commit. Do not update hashes "
            "without verifying the new commit and updating all provenance constants."
        )
    return data


def _read_local_verified(path: str, expected_sha256: str, expected_length: int) -> bytes:
    """Read local file bytes and verify length and SHA-256."""
    data = Path(path).read_bytes()
    actual_len = len(data)
    if actual_len != expected_length:
        raise ValueError(
            f"Length mismatch for {path}:\n"
            f"  expected {expected_length} bytes\n"
            f"  got      {actual_len} bytes\n"
            "Ensure this file was saved from the pinned commit "
            f"{_COMMIT[:8]} ({_COMMIT_DATE})."
        )
    actual_sha = hashlib.sha256(data).hexdigest()
    if actual_sha != expected_sha256:
        raise ValueError(
            f"SHA-256 mismatch for {path}:\n"
            f"  expected {expected_sha256}\n"
            f"  got      {actual_sha}\n"
            "This file does not match the pinned commit hash. "
            "Provide the exact bytes from the pinned commit."
        )
    return data


def _parse_csv(raw: bytes) -> list[dict[str, str]]:
    text = raw.decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def coord_e7(s: str | None) -> int | None:
    if not s or not s.strip():
        return None
    return int(round(float(s) * 1e7))


def is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    if not rest:
        return True
    if rest[0] in "-_":
        return True
    return rest.isdigit()


def is_helipad(row: dict[str, str]) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not is_h_designator(le) and not is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if is_h_designator(le) and is_h_designator(he):
        return True
    return length_ft < 2500


def build_dataset(
    airports_csv: list[dict[str, str]],
    runways_csv: list[dict[str, str]],
) -> tuple[
    list[tuple[str, int, int]],
    list[tuple[int, int, int, int, int, int]],
]:
    large_idents: dict[str, tuple[int, int]] = {}
    for a in airports_csv:
        if a.get("type") != "large_airport":
            continue
        ident = (a.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        large_idents[ident] = (lat, lon)

    airport_rows = sorted(
        (ident, lat, lon) for ident, (lat, lon) in large_idents.items()
    )
    airport_index = {ident: idx for idx, (ident, _, _) in enumerate(airport_rows)}

    segments: list[tuple[int, int, int, int, int, int]] = []
    for r in runways_csv:
        if r.get("closed") == "1":
            continue
        airport = (r.get("airport_ident") or "").strip()
        if airport not in airport_index:
            continue
        if is_helipad(r):
            continue
        try:
            length_ft = int(r.get("length_ft") or 0)
        except ValueError:
            continue
        if length_ft <= 0:
            continue
        le_lat = coord_e7(r.get("le_latitude_deg"))
        le_lon = coord_e7(r.get("le_longitude_deg"))
        he_lat = coord_e7(r.get("he_latitude_deg"))
        he_lon = coord_e7(r.get("he_longitude_deg"))
        if None in (le_lat, le_lon, he_lat, he_lon):
            continue
        length_m = int(round(length_ft * 0.3048))
        segments.append(
            (
                airport_index[airport],
                le_lat,
                le_lon,
                he_lat,
                he_lon,
                length_m,
            )
        )

    segments.sort(key=lambda row: (row[0], -row[5]))
    return airport_rows, segments


# ---------------------------------------------------------------------------
# Provenance block (stable, ASCII, no timestamp/Python-version)
# ---------------------------------------------------------------------------

def _provenance_lines() -> list[str]:
    return [
        "// Source:   https://github.com/davidmegginson/ourairports-data",
        f"// Commit:   {_COMMIT}",
        f"// Date:     {_COMMIT_DATE}",
        f"// airports.csv  SHA-256: {AIRPORTS_SHA256}",
        f"//             length:  {AIRPORTS_LENGTH}",
        f"//             git blob {AIRPORTS_BLOB}",
        f"// runways.csv   SHA-256: {RUNWAYS_SHA256}",
        f"//             length:  {RUNWAYS_LENGTH}",
        f"//             git blob {RUNWAYS_BLOB}",
        f"// License: Public Domain / The Unlicense  {LICENSE_URL}",
        f"// Filter schema version: {FILTER_SCHEMA_VERSION}",
        "// Filter: type=large_airport, 4-char ICAO ident, open runways,",
        "//         helipads excluded, coordinates and runway endpoints required.",
    ]


def render_header(airport_count: int, segment_count: int) -> str:
    lines = [
        "// Generated by scripts/build_large_airports.py -- do not edit.",
    ]
    lines += _provenance_lines()
    lines += [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace data::large_airports {",
        "",
        "struct Airport {",
        "  char ident[5];",
        "  int32_t lat_e7;",
        "  int32_t lon_e7;",
        "};",
        "",
        "struct Runway {",
        "  uint16_t airport_idx;",
        "  int32_t le_lat_e7;",
        "  int32_t le_lon_e7;",
        "  int32_t he_lat_e7;",
        "  int32_t he_lon_e7;",
        "  uint16_t length_m;",
        "};",
        "",
        f"constexpr size_t kAirportCount = {airport_count};",
        f"constexpr size_t kRunwayCount = {segment_count};",
        "",
        f"extern const Airport kAirports[kAirportCount];",
        f"extern const Runway kRunways[kRunwayCount];",
        "",
        "}  // namespace data::large_airports",
        "",
    ]
    return "\n".join(lines)


def render_cpp(
    airport_rows: list[tuple[str, int, int]],
    segments: list[tuple[int, int, int, int, int, int]],
) -> str:
    lines = [
        "// Generated by scripts/build_large_airports.py -- do not edit.",
    ]
    lines += _provenance_lines()
    lines += [
        '#include "data/large_airports.h"',
        "",
        "namespace data::large_airports {",
        "",
        "const Airport kAirports[kAirportCount] = {",
    ]
    for ident, lat, lon in airport_rows:
        lines.append(f'  {{"{ident}", {lat}, {lon}}},')
    lines += [
        "};",
        "",
        "const Runway kRunways[kRunwayCount] = {",
    ]
    for airport_idx, le_lat, le_lon, he_lat, he_lon, length_m in segments:
        lines.append(
            f"  {{{airport_idx}, {le_lat}, {le_lon}, {he_lat}, {he_lon}, {length_m}}},"
        )
    lines += [
        "};",
        "",
        "}  // namespace data::large_airports",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# --check: compare rendered bytes to checked-in files (never modifies files)
# ---------------------------------------------------------------------------

def check_mode(airports_raw: bytes, runways_raw: bytes) -> int:
    airport_rows, segments = build_dataset(
        _parse_csv(airports_raw), _parse_csv(runways_raw)
    )
    header_rendered = render_header(len(airport_rows), len(segments))
    cpp_rendered = render_cpp(airport_rows, segments)

    header_bytes = header_rendered.encode("utf-8")
    cpp_bytes = cpp_rendered.encode("utf-8")

    ok = True
    for path, rendered in ((OUT_H, header_bytes), (OUT_CPP, cpp_bytes)):
        if not path.exists():
            print(f"MISSING: {path}", file=sys.stderr)
            ok = False
            continue
        on_disk = path.read_bytes()
        if on_disk != rendered:
            print(
                f"MISMATCH: {path}\n"
                f"  on-disk {len(on_disk)} bytes, rendered {len(rendered)} bytes",
                file=sys.stderr,
            )
            ok = False
        else:
            print(f"OK: {path.name} matches rendered output ({len(rendered)} bytes)")

    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--airports-csv", metavar="PATH",
        help="Local airports.csv path (must be paired with --runways-csv)",
    )
    parser.add_argument(
        "--runways-csv", metavar="PATH",
        help="Local runways.csv path (must be paired with --airports-csv)",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="Compare rendered output to checked-in files; exit 0 if identical",
    )
    args = parser.parse_args()

    # Validate: both or neither local paths
    local_airports = args.airports_csv
    local_runways = args.runways_csv
    if bool(local_airports) != bool(local_runways):
        parser.error("--airports-csv and --runways-csv must be specified together")

    # Acquire raw bytes (verify before any parsing)
    if local_airports:
        print(f"Reading airports from {local_airports}")
        airports_raw = _read_local_verified(local_airports, AIRPORTS_SHA256, AIRPORTS_LENGTH)
        print(f"Reading runways from {local_runways}")
        runways_raw = _read_local_verified(local_runways, RUNWAYS_SHA256, RUNWAYS_LENGTH)
    else:
        print(f"Downloading airports from {AIRPORTS_URL}")
        airports_raw = _fetch_verified(AIRPORTS_URL, AIRPORTS_SHA256, AIRPORTS_LENGTH)
        print(f"Downloading runways from {RUNWAYS_URL}")
        runways_raw = _fetch_verified(RUNWAYS_URL, RUNWAYS_SHA256, RUNWAYS_LENGTH)

    if args.check:
        return check_mode(airports_raw, runways_raw)

    airport_rows, segments = build_dataset(
        _parse_csv(airports_raw), _parse_csv(runways_raw)
    )
    header = render_header(len(airport_rows), len(segments))
    cpp = render_cpp(airport_rows, segments)

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    # Write deterministic LF bytes; no platform newline translation.
    OUT_H.write_bytes(header.encode("utf-8"))
    OUT_CPP.write_bytes(cpp.encode("utf-8"))
    print(
        f"wrote {OUT_H.name} + {OUT_CPP.name} "
        f"({len(airport_rows)} airports, {len(segments)} runways)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
