"""Idempotently author the visual identity layer for every Abyss region.

This is kept with the Abyss of Hollows template because it is an authoring tool, not a
runtime dependency: rerun it whenever a level is rebuilt to restore the
non-colliding vistas and landmarks without touching the tested collision grids.
"""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


REGIONS = {
    "home": {
        "name": "Home Hollow",
        "backgrounds": [
            ("abyss/environment/cc0_grotto_far.png", [110, 138, 176, 255], -5000, 0.55, 1.85),
            ("abyss/environment/cc0_grotto_mid.png", [144, 164, 160, 255], -3300, 0.42, 1.65),
            ("abyss/environment/cc0_grotto_back.png", [175, 142, 104, 255], -1800, 0.28, 1.50),
            ("bg_abyss_mist.png", [150, 189, 178, 210], -850, 0.32, 1.80),
        ],
        "vista": "abyss/environment/cc0_grotto_far.png",
        "landmark": "lantern_abyss.png",
        "tint": [232, 190, 112, 255],
    },
    "verdant": {
        "name": "Verdant Hollow",
        "backgrounds": [
            ("abyss/environment/cc0_leaves.png", [104, 181, 122, 255], -5000, 0.64, 2.20),
            ("abyss/environment/cc0_grotto_far.png", [102, 156, 115, 255], -3300, 0.46, 1.80),
            ("abyss/environment/cc0_grotto_mid.png", [142, 205, 138, 255], -1800, 0.34, 1.55),
            ("bg_abyss_mist.png", [139, 231, 165, 185], -850, 0.30, 1.80),
        ],
        "vista": "abyss/environment/cc0_leaves.png",
        "landmark": "abyss/environment/cc0_grotto_mid.png",
        "tint": [145, 241, 157, 255],
    },
    "crystal": {
        "name": "Crystal Hall",
        "backgrounds": [
            ("abyss/environment/cc0_grotto_back.png", [88, 140, 190, 255], -5000, 0.60, 2.05),
            ("abyss/environment/cc0_grotto_far.png", [102, 187, 223, 255], -3300, 0.48, 1.85),
            ("abyss/environment/cc0_grotto_mid.png", [143, 219, 255, 255], -1800, 0.36, 1.58),
            ("bg_abyss_mist.png", [150, 220, 255, 188], -850, 0.30, 1.82),
        ],
        "vista": "abyss/environment/cc0_grotto_mid.png",
        "landmark": "crystal_prop_abyss.png",
        "tint": [132, 232, 255, 255],
    },
    "flooded": {
        "name": "Flooded Ruins",
        "backgrounds": [
            ("abyss/environment/cc0_ruins.png", [76, 111, 160, 255], -5000, 0.65, 2.35),
            ("abyss/environment/cc0_grotto_far.png", [75, 133, 182, 255], -3300, 0.48, 1.92),
            ("abyss/environment/cc0_grotto_mid.png", [104, 172, 221, 255], -1800, 0.34, 1.62),
            ("bg_abyss_mist.png", [112, 179, 244, 190], -850, 0.36, 1.90),
        ],
        "vista": "abyss/environment/cc0_ruins.png",
        "landmark": "tile_bridge.png",
        "tint": [122, 191, 255, 255],
    },
    "deep": {
        "name": "Deep Mines",
        "backgrounds": [
            ("abyss/environment/cc0_grotto_back.png", [101, 64, 48, 255], -5000, 0.67, 2.10),
            ("abyss/environment/cc0_grotto_far.png", [155, 93, 54, 255], -3300, 0.50, 1.88),
            ("abyss/environment/cc0_grotto_mid.png", [204, 126, 70, 255], -1800, 0.35, 1.58),
            ("bg_abyss_mist.png", [255, 158, 83, 174], -850, 0.25, 1.83),
        ],
        "vista": "abyss/environment/cc0_grotto_back.png",
        "landmark": "lantern_abyss.png",
        "tint": [255, 167, 82, 255],
    },
    "ascent": {
        "name": "The Ascent",
        "backgrounds": [
            ("third_party/cc0/mattbas/raw/glax-old-platformer-assets/environment/mountain_dark.png", [128, 104, 174, 255], -5000, 0.62, 2.15),
            ("third_party/cc0/mattbas/raw/glax-old-platformer-assets/environment/mountain_blue.png", [155, 132, 220, 255], -3300, 0.44, 1.90),
            ("third_party/cc0/mattbas/raw/glax-old-platformer-assets/environment/clouds.png", [220, 181, 246, 255], -1800, 0.34, 1.72),
            ("bg_abyss_mist.png", [236, 172, 249, 170], -850, 0.24, 1.84),
        ],
        "vista": "third_party/cc0/mattbas/raw/glax-old-platformer-assets/environment/mountain_dark.png",
        "landmark": "crystal_prop_abyss.png",
        "tint": [243, 184, 255, 255],
    },
    "boss": {
        "name": "Boss Sanctum",
        "backgrounds": [
            ("abyss/environment/cc0_ruins.png", [112, 43, 58, 255], -5000, 0.70, 2.42),
            ("abyss/environment/cc0_grotto_back.png", [146, 46, 66, 255], -3300, 0.50, 1.88),
            ("abyss/environment/cc0_grotto_mid.png", [206, 67, 87, 255], -1800, 0.35, 1.60),
            ("bg_abyss_mist.png", [255, 93, 112, 190], -850, 0.30, 1.90),
        ],
        "vista": "abyss/environment/cc0_ruins.png",
        "landmark": "crystal_prop_abyss.png",
        "tint": [255, 111, 131, 255],
    },
}


def sprite(name: str, ident: int, texture: str, x: float, y: float,
           sx: float, sy: float, tint: list[int], order: int, opacity: float = 1.0) -> dict:
    return {
        "active": True,
        "children": [],
        "id": ident,
        "name": name,
        "components": {
            "Transform": {"x": x, "y": y, "rotation": 0.0, "scale_x": sx, "scale_y": sy},
            "SpriteRenderer": {
                "texture": texture,
                "color": tint,
                "opacity": opacity,
                "layer": 0,
                "order_in_layer": order,
                "pixels_per_unit": 1.0,
                "flip_x": False,
                "flip_y": False,
                "use_source_rect": False,
                "source_x": 0,
                "source_y": 0,
                "source_w": 0,
                "source_h": 0,
            },
        },
    }


def light(name: str, ident: int, x: float, y: float, tint: list[int], radius: float) -> dict:
    return {
        "active": True,
        "children": [],
        "id": ident,
        "name": name,
        "components": {
            "Transform": {"x": x, "y": y, "rotation": 0.0, "scale_x": 1.0, "scale_y": 1.0},
            "Light2D": {"color": tint, "intensity": 0.38, "radius": radius},
        },
    }


def author_scene(key: str, config: dict) -> None:
    path = ROOT / f"scene_{key}.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    entities = [e for e in data.get("entities", [])
                if not e.get("name", "").startswith(("RegionVista_", "RegionLandmark_", "RegionLight_"))]

    # Reuse existing parallax slots but give every region a distinct actual
    # art stack.  The old scenes used the four Abyss backgrounds everywhere
    # with only tint changes, which made the journey read as one repeated map.
    parallax = [e for e in entities if "ParallaxBackground" in e.get("components", {})]
    for index, (texture, tint, depth, opacity, scale) in enumerate(config["backgrounds"]):
        if index >= len(parallax):
            continue
        bg = parallax[index]["components"]["ParallaxBackground"]
        bg.update({
            "texture": texture,
            "color": tint,
            "depth": depth,
            "opacity": opacity,
            "scale": scale,
            "tiling_x": True,
            "tiling_y": True,
            "scroll_with_camera": True,
            "speed_x": [0.012, 0.028, 0.052, 0.095][index],
        })

    next_id = max((int(e.get("id", 0)) for e in entities), default=0) + 1
    x_positions = [760.0, 2660.0, 4580.0, 6500.0]
    y_positions = [740.0, 980.0, 790.0, 1040.0]
    for index, (x, y) in enumerate(zip(x_positions, y_positions), start=1):
        entities.append(sprite(
            f"RegionVista_{key}_{index}", next_id, config["vista"], x, y,
            1.48 + (index % 2) * 0.14, 1.48 + (index % 2) * 0.14,
            [255, 255, 255, 255], -28, 0.72,
        ))
        next_id += 1
        # A foreground accent is deliberately offset from the vista and has
        # no collider: it creates a readable room silhouette while preserving
        # the designed platforming routes and enemy navigation.
        entities.append(sprite(
            f"RegionLandmark_{key}_{index}", next_id, config["landmark"], x + 330.0, y + 190.0,
            1.0 + (index % 3) * 0.18, 1.0 + (index % 3) * 0.18,
            config["tint"], -5, 0.90,
        ))
        next_id += 1
        entities.append(light(
            f"RegionLight_{key}_{index}", next_id, x + 320.0, y + 170.0, config["tint"], 210.0
        ))
        next_id += 1

    data["entities"] = entities
    data["region_art_revision"] = 3
    data["region_art_name"] = config["name"]
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)
    print(f"Authored {path.name}: {len(entities)} entities")


if __name__ == "__main__":
    for region_key, region_config in REGIONS.items():
        author_scene(region_key, region_config)
