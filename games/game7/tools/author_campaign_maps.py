"""Author the playable Game7 campaign grids from deterministic source data.

This tool writes scene JSON only.  It deliberately has no engine/build step.
The previous generated maps reused a single top-ceiling layout and then tried
to decorate it at runtime.  This file makes terrain, room boundaries, spawn
heights, and combat placements explicit scene data instead.

Run from the engine root:
  python games/game7/tools/author_campaign_maps.py
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / "games" / "game7"
TILE = 64
NORMAL_ROWS, NORMAL_COLS = 32, 120
BOSS_ROWS, BOSS_COLS = 36, 96


REGIONS: dict[str, dict] = {
    "scene_home": {
        "name": "Home Hollow",
        "rooms": [
            ("home_lantern_refuge", "Lantern Refuge", "Learn movement and the blade"),
            ("home_old_well", "Old Well", "Read the arc and parry signs"),
            ("home_training_vault", "Training Vault", "Chain a dash through the vault"),
            ("home_rootway_gate", "Rootway Gate", "Take the path beyond the refuge"),
        ],
        "pattern": "home",
        "terrain_texture": "tile_rock.png",
    },
    "scene_verdant": {
        "name": "Verdant Hollow",
        "rooms": [
            ("verdant_mossbridge", "Mossbridge", "Follow the low root bridge"),
            ("verdant_windroot_canopy", "Windroot Canopy", "Choose the high canopy route"),
            ("verdant_thornwell", "Thornwell", "Clear the thornbound approach"),
            ("verdant_echo_shrine", "Echo Shrine", "Reach the singing root gate"),
        ],
        "pattern": "verdant",
        "terrain_texture": "tile_moss.png",
    },
    "scene_crystal": {
        "name": "Crystal Hall",
        "rooms": [
            ("crystal_shard_gallery", "Shard Gallery", "Cross the fractured gallery"),
            ("crystal_prism_wells", "Prism Wells", "Climb the mirrored wells"),
            ("crystal_resonance_shaft", "Resonance Shaft", "Use the vertical crystal route"),
            ("crystal_glass_archive", "Glass Archive", "Recover the archive route"),
        ],
        "pattern": "crystal",
        "terrain_texture": "tile_crystal.png",
    },
    "scene_flooded": {
        "name": "Flooded Ruins",
        "rooms": [
            ("flooded_tidal_vault", "Tidal Vault", "Follow the raised dryway"),
            ("flooded_sluice_maze", "Sluice Maze", "Cross the sluice platforms"),
            ("flooded_drowned_reliquary", "Drowned Reliquary", "Take the old aqueduct"),
            ("flooded_moonwell_dock", "Moonwell Dock", "Find the moonlit exit"),
        ],
        "pattern": "flooded",
        "terrain_texture": "tile_bridge.png",
    },
    "scene_deep": {
        "name": "Deep Mines",
        "rooms": [
            ("deep_ember_tram", "Ember Tram", "Follow the rail lights"),
            ("deep_oreworks", "Oreworks", "Cross the oreworks"),
            ("deep_vein_chasm", "Vein Chasm", "Descend beside the crystal vein"),
            ("deep_candle_quarry", "Candle Quarry", "Reach the quarry lift"),
        ],
        "pattern": "deep",
        "terrain_texture": "tile_rock.png",
    },
    "scene_ascent": {
        "name": "The Ascent",
        "rooms": [
            ("ascent_cloud_steps", "Cloud Steps", "Climb the first bell stairs"),
            ("ascent_bell_tower", "Bell Tower", "Thread the tower platforms"),
            ("ascent_gale_gauntlet", "Gale Gauntlet", "Master the high route"),
            ("ascent_crown_walk", "Crown Walk", "Reach the sanctum path"),
        ],
        "pattern": "ascent",
        "terrain_texture": "tile_bridge.png",
    },
    "scene_boss": {
        "name": "Boss Sanctum",
        "rooms": [
            ("sanctum_gate", "Sanctum Gate", "Prepare for the trial"),
            ("sanctum_trial_hall", "Trial Hall", "Read the Warden's tells"),
            ("sanctum_warden_arena", "Warden Arena", "Defeat the Sanctum Warden"),
            ("sanctum_afterglow_chamber", "Afterglow Chamber", "Claim the return path"),
        ],
        "pattern": "boss",
        "terrain_texture": "tile_crystal.png",
    },
}


def new_grid(rows: int, cols: int) -> list[list[int]]:
    return [[-1 for _ in range(cols)] for _ in range(rows)]


def fill(grid: list[list[int]], x0: int, y0: int, x1: int, y1: int) -> None:
    """Fill inclusive cells, clipping rather than producing malformed JSON."""
    rows, cols = len(grid), len(grid[0])
    for y in range(max(0, y0), min(rows, y1 + 1)):
        for x in range(max(0, x0), min(cols, x1 + 1)):
            grid[y][x] = 0


def platform(grid: list[list[int]], x0: int, x1: int, y: int, depth: int = 1) -> None:
    fill(grid, x0, y, x1, y + depth - 1)


def arch(grid: list[list[int]], x0: int, x1: int, floor_y: int = 25) -> None:
    """A walkable arch: open underneath, stepped and reachable above."""
    fill(grid, x0, floor_y - 3, x0 + 1, floor_y - 1)
    fill(grid, x1 - 1, floor_y - 3, x1, floor_y - 1)
    platform(grid, x0 + 2, x1 - 2, floor_y - 4)


def base_grid(rows: int, cols: int) -> list[list[int]]:
    grid = new_grid(rows, cols)
    # A continuous lower route makes every scene recoverable even when a
    # player misses an optional platform.  It is intentionally at the bottom,
    # never a giant roof over the play space.
    fill(grid, 0, rows - 7, cols - 1, rows - 1)
    fill(grid, 0, rows - 15, 1, rows - 8)
    fill(grid, cols - 2, rows - 15, cols - 1, rows - 8)
    return grid


def build_normal(pattern: str) -> list[list[int]]:
    g = base_grid(NORMAL_ROWS, NORMAL_COLS)
    floor = NORMAL_ROWS - 7
    if pattern == "home":
        # Four teaching spaces: small steps, a well arch, an arena, then a
        # root gate.  Their silhouettes deliberately differ from one another.
        for x, y in [(3, 23), (7, 21), (12, 19), (17, 20), (23, 22)]: platform(g, x, x + 3, y)
        arch(g, 34, 48, floor)
        platform(g, 39, 45, 18)
        platform(g, 54, 63, 21); platform(g, 57, 60, 17)
        platform(g, 69, 74, 22); platform(g, 78, 83, 20); platform(g, 86, 89, 18)
        fill(g, 97, 20, 100, floor - 1); platform(g, 102, 111, 21); arch(g, 112, 118, floor)
    elif pattern == "verdant":
        # Branches split and reunite above the safe root route.
        platform(g, 3, 10, 22); platform(g, 12, 17, 19); platform(g, 18, 24, 16)
        fill(g, 26, 18, 28, floor - 1); platform(g, 30, 37, 20); platform(g, 36, 43, 15)
        platform(g, 47, 53, 22); fill(g, 55, 16, 57, floor - 1); platform(g, 59, 66, 18)
        platform(g, 69, 75, 14); platform(g, 77, 83, 20); fill(g, 87, 20, 89, floor - 1)
        platform(g, 92, 99, 17); platform(g, 100, 107, 21); platform(g, 110, 116, 18)
    elif pattern == "crystal":
        # Crystal wells are vertical zig-zags rather than a copy of Verdant.
        fill(g, 5, 20, 7, floor - 1); platform(g, 8, 14, 18); fill(g, 16, 15, 18, floor - 1)
        platform(g, 20, 24, 13); platform(g, 26, 32, 18); fill(g, 34, 19, 36, floor - 1)
        platform(g, 39, 45, 15); fill(g, 47, 14, 49, floor - 1); platform(g, 51, 56, 11)
        platform(g, 59, 64, 17); fill(g, 66, 18, 68, floor - 1); platform(g, 70, 75, 14)
        platform(g, 78, 84, 19); fill(g, 86, 12, 88, floor - 1); platform(g, 90, 96, 15)
        platform(g, 99, 104, 18); platform(g, 107, 115, 13)
    elif pattern == "flooded":
        # Basins are represented by visual/open lower spaces; the dry main
        # route and bridges remain solid and safe without unsupported water.
        platform(g, 3, 13, 21); fill(g, 15, 20, 17, floor - 1); platform(g, 18, 24, 18)
        platform(g, 28, 35, 22); fill(g, 37, 17, 39, floor - 1); platform(g, 41, 48, 16)
        platform(g, 51, 58, 20); fill(g, 60, 21, 62, floor - 1); platform(g, 64, 70, 18)
        platform(g, 73, 79, 14); platform(g, 81, 87, 20); fill(g, 89, 19, 91, floor - 1)
        platform(g, 94, 101, 16); platform(g, 104, 112, 21)
    elif pattern == "deep":
        # Dense, broad mine beams and staggered shafts.  DeepRock is the one
        # collision source; bridge/crystal layers are visual only.
        fill(g, 4, 21, 10, floor - 1); platform(g, 12, 18, 18); fill(g, 20, 17, 22, floor - 1)
        platform(g, 25, 20, 32, 20); fill(g, 34, 14, 36, floor - 1); platform(g, 38, 42, 12)
        platform(g, 44, 49, 19); fill(g, 51, 21, 53, floor - 1); platform(g, 55, 61, 16)
        fill(g, 63, 14, 65, floor - 1); platform(g, 67, 73, 18); platform(g, 75, 80, 13)
        fill(g, 83, 20, 85, floor - 1); platform(g, 87, 93, 17); platform(g, 96, 103, 21)
        fill(g, 106, 16, 108, floor - 1); platform(g, 110, 116, 14)
    elif pattern == "ascent":
        # A recoverable staircase, not the previous impenetrable diagonal.
        for base, heights in [(3, [22, 20, 18, 16]), (32, [21, 18, 15, 13]),
                              (61, [22, 19, 16, 13]), (90, [20, 17, 14, 11])]:
            for i, y in enumerate(heights): platform(g, base + i * 5, base + i * 5 + 3, y)
        fill(g, 26, 19, 28, floor - 1); fill(g, 55, 17, 57, floor - 1)
        fill(g, 84, 15, 86, floor - 1); platform(g, 111, 116, 16)
    else:
        raise ValueError(f"Unknown region pattern: {pattern}")
    return g


def build_boss() -> list[list[int]]:
    g = base_grid(BOSS_ROWS, BOSS_COLS)
    # A generous open arena with limited, readable flank platforms.  There is
    # no ceiling or random obstruction between the boss and player.
    fill(g, 0, 20, 1, BOSS_ROWS - 1)
    fill(g, BOSS_COLS - 2, 20, BOSS_COLS - 1, BOSS_ROWS - 1)
    platform(g, 8, 15, 24); platform(g, 20, 26, 21)
    platform(g, 69, 75, 21); platform(g, 80, 87, 24)
    platform(g, 35, 40, 25); platform(g, 55, 60, 25)
    return g


def scene_grid(scene: str) -> list[list[int]]:
    return build_boss() if scene == "scene_boss" else build_normal(REGIONS[scene]["pattern"])


def scene_entry(scene: str) -> tuple[float, float]:
    grid = scene_grid(scene)
    x = 6.5 * TILE
    return x, safe_y(grid, x, 75.0)


def sparse_detail(terrain: list[list[int]]) -> list[list[int]]:
    """A tiny visual trim layer.  It cannot create a second collider set."""
    rows, cols = len(terrain), len(terrain[0])
    out = new_grid(rows, cols)
    for y in range(1, rows):
        for x in range(cols):
            if terrain[y][x] >= 0 and terrain[y - 1][x] < 0:
                out[y][x] = 0
    return out


def script_names(entity: dict) -> list[str]:
    return entity.get("components", {}).get("ScriptComponent", {}).get("scripts", [])


def transform(entity: dict) -> dict:
    return entity.setdefault("components", {}).setdefault("Transform", {})


def terrain_top(grid: list[list[int]], column: int) -> int:
    column = max(0, min(column, len(grid[0]) - 1))
    for y in range(1, len(grid)):
        if grid[y][column] >= 0 and grid[y - 1][column] < 0:
            return y
    return len(grid) - 7


def safe_y(grid: list[list[int]], x: float, height: float = 44.0) -> float:
    top = terrain_top(grid, int(x // TILE)) * TILE
    return top - height * 0.5 - 3.0


def room_entity(entity_id: int, scene: str, index: int, room: tuple[str, str, str], cols: int, rows: int) -> dict:
    room_id, title, objective = room
    width_cols = cols // 4
    x0 = index * width_cols * TILE
    width = width_cols * TILE
    return {
        "active": True,
        "campaign_layout_version": 2,
        "campaign_scene_key": scene,
        "room_id": room_id,
        "room_title": title,
        "room_objective": objective,
        "children": [],
        "components": {
            "Transform": {"x": x0 + width * 0.5, "y": rows * TILE * 0.5,
                          "rotation": 0.0, "scale_x": 1.0, "scale_y": 1.0},
            "BoxCollider2D": {"width": width - 12, "height": rows * TILE - 12,
                              "offset_x": 0.0, "offset_y": 0.0, "is_trigger": True,
                              "friction": 0.0, "bounciness": 0.0},
            "ScriptComponent": {"scripts": ["abyss_room"]},
        },
        "id": entity_id,
        "name": f"CampaignRoom_{index + 1}_{room_id}",
    }


def map_entity(entity_id: int, scene: str) -> dict:
    return {
        "active": True,
        "campaign_layout_version": 2,
        "campaign_scene_key": scene,
        "children": [],
        "components": {"Transform": {"x": 0.0, "y": 0.0, "rotation": 0.0,
                                         "scale_x": 1.0, "scale_y": 1.0}},
        "id": entity_id,
        "name": "CampaignLayout_v2",
    }


def nearest_support_x(grid: list[list[int]], desired_x: float) -> float:
    """Choose a nearby clear column above a top surface for a combatant."""
    base = max(2, min(len(grid[0]) - 3, int(desired_x // TILE)))
    for delta in [0, -1, 1, -2, 2, -3, 3]:
        col = max(2, min(len(grid[0]) - 3, base + delta))
        top = terrain_top(grid, col)
        if top > 2 and grid[top - 1][col] < 0:
            return col * TILE + TILE * 0.5
    return desired_x


def rewrite_scene(scene: str, spec: dict) -> None:
    path = GAME / f"{scene}.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    is_boss = scene == "scene_boss"
    rows, cols = (BOSS_ROWS, BOSS_COLS) if is_boss else (NORMAL_ROWS, NORMAL_COLS)
    terrain = scene_grid(scene)
    detail = sparse_detail(terrain)

    entities = [e for e in data.get("entities", []) if not e.get("name", "").startswith("CampaignRoom_")
                and e.get("name") != "CampaignLayout_v2"]
    highest_id = max((int(e.get("id", 0)) for e in entities), default=0)

    tilemaps = [e for e in entities if "Tilemap" in e.get("components", {})]
    if not tilemaps:
        raise RuntimeError(f"{path} has no Tilemap entity")
    primary = next((e for e in tilemaps if "rock" in e.get("name", "").lower()), tilemaps[0])
    primary["name"] = f"{spec['name'].replace(' ', '')}Terrain"
    for tilemap in tilemaps:
        tm = tilemap["components"]["Tilemap"]
        tm["tile_size"] = TILE
        tm["origin_x"] = 0
        tm["origin_y"] = 0
        tm["filter_mode"] = "point"
        tm["grid"] = copy.deepcopy(terrain if tilemap is primary else detail)
        tm["generate_colliders"] = tilemap is primary
        if tilemap is primary:
            tm["tileset"] = spec["terrain_texture"]
        tilemap.pop("_tilemap_colliders", None)
        tilemap.pop("_tilemap_debug_lines", None)

    # Rectify real gameplay entities so they stand in the new authored space
    # instead of retaining coordinates from an unrelated tile layout. The
    # legacy sample activated 18+ enemies in every short corridor; cap normal
    # regions at two readable opponents per authored room.
    active_combatants = 0
    for e in entities:
        comp = e.get("components", {})
        pos = comp.get("Transform")
        if not pos:
            continue
        scripts = script_names(e)
        x = float(pos.get("x", 0.0))
        if e.get("name") == "AbyssPlayer":
            pos["x"] = 6.5 * TILE
            pos["y"] = safe_y(terrain, pos["x"], 75.0)
            e["campaign_scene_key"] = f"{scene}.json"
        elif "abyss_checkpoint" in scripts:
            # Existing checkpoints are kept, but placed at the entrance to
            # each authored quarter instead of hanging inside old geometry.
            old_x = float(pos.get("x", 0.0))
            if old_x < cols * TILE * 0.34:
                pos["x"] = 7.0 * TILE
            elif old_x < cols * TILE * 0.70:
                pos["x"] = 57.0 * TILE
            else:
                pos["x"] = (cols - 14.0) * TILE
            pos["y"] = safe_y(terrain, pos["x"], 52.0)
        elif "abyss_portal" in scripts:
            # Ports stay region exits, never mid-air objects inside a room.
            left_exit = x < cols * TILE * 0.5
            pos["x"] = (3.5 if left_exit else cols - 4.5) * TILE
            pos["y"] = safe_y(terrain, pos["x"], 72.0)
            target = e.get("target_scene", "")
            if target in REGIONS:
                entry_x, entry_y = scene_entry(target)
                e["target_spawn_x"] = entry_x
                e["target_spawn_y"] = entry_y
                e["target_spawn_name"] = f"{REGIONS[target]['name']} Entry"
        elif any(s in scripts for s in ("abyss_crawler", "abyss_spitter", "abyss_turret", "abyss_boss")) and e.get("active", True):
            if e.get("name") == "AbyssCrawlerTemplate":
                continue
            if not is_boss and active_combatants >= 8:
                e["active"] = False
                e["campaign_disabled_reason"] = "Replaced by authored room encounter pacing"
                continue
            active_combatants += 1
            pos["x"] = nearest_support_x(terrain, x)
            collider = comp.get("BoxCollider2D", {})
            pos["y"] = safe_y(terrain, pos["x"], float(collider.get("height", 40.0)))
            e["campaign_room_index"] = min(3, int(pos["x"] / (cols * TILE / 4)))
        elif "abyss_ability_unlock" in scripts or "abyss_ability_gate" in scripts:
            pos["x"] = max(10.0 * TILE, min((cols - 10.0) * TILE, x))
            pos["y"] = safe_y(terrain, pos["x"], 64.0)

    entities.append(map_entity(highest_id + 1, scene))
    for index, room in enumerate(spec["rooms"]):
        entities.append(room_entity(highest_id + 2 + index, scene, index, room, cols, rows))

    data["campaign_layout_version"] = 2
    data["campaign_scene_key"] = scene
    data["entities"] = entities
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def validate_scene(scene: str) -> None:
    data = json.loads((GAME / f"{scene}.json").read_text(encoding="utf-8"))
    entity_by_name = {e.get("name"): e for e in data["entities"]}
    assert data.get("campaign_layout_version") == 2
    assert "CampaignLayout_v2" in entity_by_name
    rooms = [e for e in data["entities"] if e.get("name", "").startswith("CampaignRoom_")]
    assert len(rooms) == 4, (scene, len(rooms))
    terrain = next(e for e in data["entities"]
                   if e.get("components", {}).get("Tilemap", {}).get("generate_colliders", False))
    tm = terrain["components"]["Tilemap"]
    assert tm["generate_colliders"] is True
    grid = tm["grid"]
    assert all(len(row) == len(grid[0]) for row in grid)
    assert any(cell >= 0 for cell in grid[-7]), scene
    player = entity_by_name["AbyssPlayer"]["components"]["Transform"]
    assert 0 < player["x"] < len(grid[0]) * TILE
    assert 0 < player["y"] < len(grid) * TILE
    for entity in data["entities"]:
        if "abyss_portal" not in script_names(entity):
            continue
        target = entity.get("target_scene", "")
        if target not in REGIONS:
            continue
        expected_x, expected_y = scene_entry(target)
        assert entity.get("target_spawn_x") == expected_x
        assert entity.get("target_spawn_y") == expected_y


def main() -> None:
    for scene, spec in REGIONS.items():
        rewrite_scene(scene, spec)
        validate_scene(scene)
        print(f"authored {scene}")


if __name__ == "__main__":
    main()
