#!/usr/bin/env python3
#
# Copyright 2021-2026 Software Radio Systems Limited
#
# This file is part of srsRAN.
#
# srsRAN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# srsRAN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# A copy of the GNU Affero General Public License can be found in
# the LICENSE file in the top-level directory of this distribution
# and at http://www.gnu.org/licenses/.

"""Generate a static earth-fixed NTN beam table for CU-CP mobility testing.

The generated JSON is an O&M/planning input for CU-CP. It does not configure
DU/RU beamforming, SIB19, MAC scheduling or PHY compensation.
"""

import argparse
import json
import math
import sys


EARTH_RADIUS_KM = 6378.137
RING1_CHILD_OFFSETS = ((0, 0), (1, 0), (1, -1), (0, -1), (-1, 0), (-1, 1), (0, 1))


def leo_footprint_radius_km(altitude_m: float, min_elevation_deg: float) -> float:
    earth_radius_km = EARTH_RADIUS_KM
    orbit_radius_km = earth_radius_km + altitude_m / 1000.0
    elevation_rad = math.radians(min_elevation_deg)
    central_angle_rad = math.acos((earth_radius_km / orbit_radius_km) * math.cos(elevation_rad)) - elevation_rad
    return earth_radius_km * central_angle_rad


def normalize_longitude(longitude_deg: float) -> float:
    value = (longitude_deg + 180.0) % 360.0 - 180.0
    return 180.0 if value == -180.0 and longitude_deg > 0 else value


def distance_km(lat1_deg: float, lon1_deg: float, lat2_deg: float, lon2_deg: float) -> float:
    lat1 = math.radians(lat1_deg)
    lat2 = math.radians(lat2_deg)
    dlat = lat2 - lat1
    dlon = math.radians(normalize_longitude(lon2_deg - lon1_deg))
    a = math.sin(dlat / 2.0) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2.0) ** 2
    return EARTH_RADIUS_KM * 2.0 * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))


def make_nci(gnb_id: int, gnb_id_bits: int, sector_id: int) -> int:
    sector_bits = 36 - gnb_id_bits
    if sector_id >= (1 << sector_bits):
        raise ValueError(f"sector_id={sector_id} exceeds {sector_bits}-bit NR cell identity sector space")
    return (gnb_id << sector_bits) | sector_id


def offset_to_axial(row: int, col: int) -> tuple[int, int]:
    q = col - math.floor((row - (row & 1)) / 2)
    return q, row


def analog_center_lattice_points(q_values: list[int], r_values: list[int]) -> list[tuple[int, int]]:
    min_q = min(q_values) - 2
    max_q = max(q_values) + 2
    min_r = min(r_values) - 2
    max_r = max(r_values) + 2
    limit = max(abs(min_q), abs(max_q), abs(min_r), abs(max_r)) + 8
    centers: list[tuple[int, int]] = []
    for i in range(-limit, limit + 1):
        for j in range(-limit, limit + 1):
            q = 3 * i + j
            r = -i + 2 * j
            if min_q <= q <= max_q and min_r <= r <= max_r:
                centers.append((q, r))
    return centers


def generate_analog_clusters(beams: list[dict], analog_beam_id_prefix: str) -> list[dict]:
    beams_by_hex = {(beam["hex_q"], beam["hex_r"]): beam for beam in beams}
    q_values = [beam["hex_q"] for beam in beams]
    r_values = [beam["hex_r"] for beam in beams]
    clusters = []
    assigned_children: set[str] = set()
    for center_q, center_r in analog_center_lattice_points(q_values, r_values):
        children = []
        for dq, dr in RING1_CHILD_OFFSETS:
            child = beams_by_hex.get((center_q + dq, center_r + dr))
            if child is not None:
                children.append(child)
        if not children:
            continue
        children.sort(key=lambda beam: beam["sector_id"])
        for child in children:
            if child["beam_id"] in assigned_children:
                raise ValueError(f"digital beam {child['beam_id']} belongs to more than one analog cluster")
            assigned_children.add(child["beam_id"])
        center_child = beams_by_hex.get((center_q, center_r), children[0])
        clusters.append(
            {
                "center_hex_q": center_q,
                "center_hex_r": center_r,
                "center_digital_beam_id": center_child["beam_id"],
                "child_digital_beam_ids": [child["beam_id"] for child in children],
                "is_edge_partial": len(children) != 7,
                "min_sector_id": min(child["sector_id"] for child in children),
            }
        )

    missing_children = {beam["beam_id"] for beam in beams} - assigned_children
    if missing_children:
        raise ValueError(f"digital beams missing analog cluster assignment: {sorted(missing_children)[:5]}")

    clusters.sort(key=lambda cluster: cluster["min_sector_id"])
    for idx, cluster in enumerate(clusters, start=1):
        cluster["analog_beam_id"] = f"{analog_beam_id_prefix}-{idx:05d}"
        del cluster["min_sector_id"]
        for child_id in cluster["child_digital_beam_ids"]:
            for beam in beams:
                if beam["beam_id"] == child_id:
                    beam["analog_beam_id"] = cluster["analog_beam_id"]
                    break
    return clusters


def generate_beams(args: argparse.Namespace) -> list[dict]:
    beam_radius_km = args.beam_radius_m / 1000.0
    region_radius_km = args.region_radius_km
    if region_radius_km is None:
        region_radius_km = leo_footprint_radius_km(args.satellite_height_m, args.min_elevation_deg)

    lat_spacing_deg = (1.5 * beam_radius_km) / EARTH_RADIUS_KM * 180.0 / math.pi
    max_rows = int(math.ceil((region_radius_km + beam_radius_km) / (1.5 * beam_radius_km)))

    beams = []
    sector_id = args.first_sector_id
    for row in range(-max_rows, max_rows + 1):
        lat_deg = args.center_lat_deg + row * lat_spacing_deg
        if lat_deg < -90.0 or lat_deg > 90.0:
            continue

        row_offset_km = abs(row) * 1.5 * beam_radius_km
        half_width_km = math.sqrt(max(0.0, (region_radius_km + beam_radius_km) ** 2 - row_offset_km ** 2))
        lon_spacing_km = math.sqrt(3.0) * beam_radius_km
        cos_lat = max(0.05, math.cos(math.radians(lat_deg)))
        lon_spacing_deg = lon_spacing_km / (EARTH_RADIUS_KM * cos_lat) * 180.0 / math.pi
        max_cols = int(math.ceil(half_width_km / lon_spacing_km))
        row_lon_offset_deg = 0.5 * lon_spacing_deg if row % 2 else 0.0

        for col in range(-max_cols, max_cols + 1):
            lon_deg = normalize_longitude(args.center_lon_deg + col * lon_spacing_deg + row_lon_offset_deg)
            if (
                distance_km(args.center_lat_deg, args.center_lon_deg, lat_deg, lon_deg)
                > region_radius_km + beam_radius_km
            ):
                continue

            hex_q, hex_r = offset_to_axial(row, col)
            nci = make_nci(args.gnb_id, args.gnb_id_bits, sector_id)
            beams.append(
                {
                    "beam_id": f"{args.beam_id_prefix}-{sector_id:05d}",
                    "sector_id": sector_id,
                    "hex_q": hex_q,
                    "hex_r": hex_r,
                    "nci": f"0x{nci:x}",
                    "center_latitude_deg": round(lat_deg, 7),
                    "center_longitude_deg": round(lon_deg, 7),
                    "coverage_radius_m": args.beam_radius_m,
                    "enabled": True,
                }
            )
            sector_id += 1
    return beams


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--center-lat-deg", type=float, required=True)
    parser.add_argument("--center-lon-deg", type=float, required=True)
    parser.add_argument("--region-radius-km", type=float, default=None)
    parser.add_argument("--satellite-height-m", type=float, default=500000.0)
    parser.add_argument("--min-elevation-deg", type=float, default=50.0)
    parser.add_argument("--beam-radius-m", type=float, default=15000.0)
    parser.add_argument("--gnb-id", type=lambda value: int(value, 0), default=0x19B)
    parser.add_argument("--gnb-id-bits", type=int, default=22)
    parser.add_argument("--first-sector-id", type=int, default=1)
    parser.add_argument("--beam-id-prefix", default="LEO500-DIGI")
    parser.add_argument("--analog-beam-id-prefix", default="LEO500-ANALOG")
    parser.add_argument("--region", default="leo-500km-generated")
    parser.add_argument("--output", default="-")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not (-90.0 <= args.center_lat_deg <= 90.0 and -180.0 <= args.center_lon_deg <= 180.0):
        raise ValueError("center latitude/longitude are outside valid ranges")
    if args.beam_radius_m <= 0.0 or args.satellite_height_m <= 0.0:
        raise ValueError("beam radius and satellite height must be positive")
    if not (1 <= args.gnb_id_bits <= 32):
        raise ValueError("gnb-id-bits must be within [1, 32]")

    beams = generate_beams(args)
    analog_beams = generate_analog_clusters(beams, args.analog_beam_id_prefix)
    for beam in beams:
        del beam["sector_id"]
    table = {
        "version": 1,
        "region": args.region,
        "satellite_height_m": args.satellite_height_m,
        "analog_beams": analog_beams,
        "beams": beams,
    }
    text = json.dumps(table, indent=2)
    if args.output == "-":
        print(text)
    else:
        with open(args.output, "w", encoding="utf-8") as output:
            output.write(text)
            output.write("\n")
    nof_full_analog_beams = sum(1 for beam in analog_beams if not beam["is_edge_partial"])
    print(
        f"generated {len(beams)} digital beams and {len(analog_beams)} analog beams "
        f"({nof_full_analog_beams} full, {len(analog_beams) - nof_full_analog_beams} partial)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
