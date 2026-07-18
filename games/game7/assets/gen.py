import json, copy, math, random

# ── helpers ──────────────────────────────────────────────────────────────────
def make_transform(x=0, y=0, sx=1.0, sy=1.0, rot=0.0):
    return {"rotation": rot, "scale_x": sx, "scale_y": sy, "x": x, "y": y}

def make_parallax(name, eid, texture, depth, speed_x, opacity=0.95, scale=1.8, color=None):
    return {
        "active": True, "children": [], "id": eid, "name": name,
        "components": {
            "ParallaxBackground": {
                "color": color or [255,255,255,255],
                "depth": depth, "offset_x": 0.0, "offset_y": 0.0,
                "opacity": opacity, "scale": scale,
                "scroll_with_camera": True,
                "speed_x": speed_x, "speed_y": 0.0,
                "texture": texture, "tiling_x": True, "tiling_y": True
            },
            "Transform": make_transform()
        }
    }

def make_camera(eid=1000):
    return {
        "active": True, "children": [], "id": eid, "name": "Camera",
        "components": {
            "Camera2D": {"background_color": [8,6,18,255], "ortho_size": 540.0, "zoom": 1.0},
            "Transform": make_transform(620, 800)
        }
    }

def make_hud_entities(start_id=1005):
    i = start_id
    ents = []
    ents.append({"active":True,"children":[],"id":i,"name":"HUDPanel","components":{
        "Transform":make_transform(),"UICanvas":{},
        "UIPanel":{"anchor_x":0.0,"anchor_y":0.0,"color":[14,12,28,176],"height":116,"pivot_x":0.0,"pivot_y":0.0,"pos_x":12,"pos_y":12,"width":490}
    }})
    i+=1
    ents.append({"active":True,"children":[],"id":i,"name":"HudLogo","components":{
        "Transform":make_transform(),"UICanvas":{},
        "UIImage":{"anchor_x":0.0,"anchor_y":1.0,"color":[255,255,255,255],"height":76,"opacity":1.0,"pivot_x":0.0,"pivot_y":1.0,"pos_x":622,"pos_y":-535,"texture":"logo_abyss.png","width":76}
    }})
    i+=1
    for name, color, pos_y, text in [
        ("HudHealth",[160,255,180,255],-26,"HP 8 / 8"),
        ("HudEnergy",[120,200,255,255],-52,"EN 100"),
        ("HudRoom",[220,200,255,255],-78,"???"),
        ("HudHint",[255,240,160,255],-104,""),
        ("HudAmmo",[255,200,100,255],-130,"AMMO 12"),
        ("HudCombo",[255,160,80,255],-156,""),
    ]:
        ents.append({"active":True,"children":[],"id":i,"name":name,"components":{
            "Transform":make_transform(),"UICanvas":{},
            "UIText":{"align":"left","anchor_x":0.0,"anchor_y":1.0,"bold":True,"color":color,"font_size":16,"height":22,"italic":False,"pivot_x":0.0,"pivot_y":1.0,"pos_x":28,"pos_y":pos_y,"shadow":True,"text":text,"v_align":"middle","width":250}
        }})
        i+=1
    return ents

def make_player(x, y, eid=1015):
    return {
        "active": True, "children": [], "damage": 0, "hp": 8, "max_hp": 8,
        "id": eid, "name": "AbyssPlayer", "team": 1,
        "components": {
            "Animator": {
                "animations": {
                    "gun_idle":{"fps":6,"frames":[0,1,2,3],"loop":True},
                    "gun_walk":{"fps":12,"frames":[8,9,10,11,12,13,14,15],"loop":True},
                    "gun_jump":{"fps":10,"frames":[4,5],"loop":False},
                    "gun_fall":{"fps":10,"frames":[6,7],"loop":True},
                    "gun_dash":{"fps":18,"frames":[22],"loop":False},
                    "gun_shoot":{"fps":16,"frames":[16,17,18],"loop":False},
                    "gun_hurt":{"fps":16,"frames":[23,0],"loop":False,"ping_pong":True},
                    "sword_idle":{"fps":6,"frames":[0,1,2,3],"loop":True},
                    "sword_walk":{"fps":12,"frames":[8,9,10,11,12,13,14,15],"loop":True},
                    "sword_jump":{"fps":10,"frames":[4,5],"loop":False},
                    "sword_fall":{"fps":10,"frames":[6,7],"loop":True},
                    "sword_dash":{"fps":18,"frames":[22],"loop":False},
                    "sword_hurt":{"fps":16,"frames":[23,0],"loop":False,"ping_pong":True},
                    "sword_slash1":{"fps":20,"frames":[24,25,25],"loop":False},
                    "sword_slash2":{"fps":22,"frames":[24,26,26],"loop":False},
                    "sword_slash3":{"fps":24,"frames":[24,27,27,27],"loop":False},
                    "gun_reload":{"fps":5,"frames":[0,1,2,1],"loop":True,"ping_pong":True},
                    "sword_windup":{"fps":22,"frames":[24],"loop":False}
                },
                "current_animation":"gun_idle","default_fps":10,"frame_height":64,"frame_width":48,
                "loop":True,"playing":True,"sheet_columns":8,"sheet_padding":0,"sheet_rows":4,
                "sheet_spacing":0,"speed":1.0,"sprite_sheet":"player_abyss_sheet.png","use_sprite_sheet":True
            },
            "BoxCollider2D":{"bounciness":0.0,"friction":0.18,"height":42,"is_trigger":False,"offset_x":0,"offset_y":0,"width":26},
            "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":0.0,"is_kinematic":False,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},
            "Script":{"class_name":"","field_overrides":{"AbyssPlayer":{}},"scripts":["AbyssPlayer"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":10,"pixels_per_unit":1.0,"source_h":64,"source_w":48,"source_x":0,"source_y":0,"texture":"player_abyss_sheet.png","use_source_rect":True},
            "Transform":make_transform(x, y)
        }
    }

def make_template(name, eid, scripts, texture, w, h, has_animator=False, anim_data=None, team=2, extra=None):
    e = {
        "active": False, "children": [], "id": eid, "name": name, "team": team,
        "components": {
            "BoxCollider2D":{"bounciness":0.0,"friction":0.4,"height":h,"is_trigger":False,"offset_x":0,"offset_y":0,"width":w},
            "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":1.0,"is_kinematic":False,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},
            "ScriptComponent":{"scripts":scripts},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":0.95,"order_in_layer":8,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":texture,"use_source_rect":False},
            "Transform":make_transform(-5000,-5000)
        }
    }
    if has_animator and anim_data:
        e["components"]["Animator"] = anim_data
    if extra:
        e.update(extra)
    return e

def make_bolt_template(eid=1010):
    return {
        "active":False,"children":[],"id":eid,"name":"AbyssBoltTemplate","team":2,
        "components":{
            "Animator":{"animations":{"idle":{"fps":12,"frames":[0,1,2,3],"loop":True}},"current_animation":"idle","default_fps":12,"frame_height":16,"frame_width":16,"loop":True,"playing":True,"sheet_columns":4,"sheet_padding":0,"sheet_rows":1,"sheet_spacing":0,"speed":1.0,"sprite_sheet":"bolt_abyss.png","use_sprite_sheet":True},
            "BoxCollider2D":{"bounciness":0.0,"friction":0.0,"height":10,"is_trigger":True,"offset_x":0,"offset_y":0,"width":10},
            "Rigidbody2D":{"angular_drag":0.0,"drag":0.0,"freeze_rotation":True,"gravity_scale":0.0,"is_kinematic":True,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},
            "ScriptComponent":{"scripts":["abyss_bolt"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":9,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"bolt_abyss.png","use_source_rect":False},
            "Transform":make_transform(-5000,-5000)
        }
    }

def make_shard_template(eid=1011):
    return {"active":False,"children":[],"id":eid,"name":"AbyssShardTemplate","team":2,"components":{"BoxCollider2D":{"bounciness":0.3,"friction":0.3,"height":12,"is_trigger":True,"offset_x":0,"offset_y":0,"width":12},"Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":False,"gravity_scale":1.0,"is_kinematic":False,"mass":0.5,"velocity_x":0.0,"velocity_y":0.0},"ScriptComponent":{"scripts":["abyss_shard"]},"SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":9,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"shard_abyss.png","use_source_rect":False},"Transform":make_transform(-5000,-5000)}}

def make_slash_template(eid=1012):
    return {"active":False,"children":[],"id":eid,"name":"AbyssSlashTemplate","team":1,"components":{"Animator":{"animations":{"slash":{"fps":20,"frames":[0,1,2,3,4],"loop":False}},"current_animation":"slash","default_fps":20,"frame_height":64,"frame_width":64,"loop":False,"playing":False,"sheet_columns":5,"sheet_padding":0,"sheet_rows":1,"sheet_spacing":0,"speed":1.0,"sprite_sheet":"slash_abyss.png","use_sprite_sheet":True},"BoxCollider2D":{"bounciness":0.0,"friction":0.0,"height":48,"is_trigger":True,"offset_x":0,"offset_y":0,"width":64},"Rigidbody2D":{"angular_drag":0.0,"drag":0.0,"freeze_rotation":True,"gravity_scale":0.0,"is_kinematic":True,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},"ScriptComponent":{"scripts":["abyss_slash"]},"SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":11,"pixels_per_unit":1.0,"source_h":64,"source_w":64,"source_x":0,"source_y":0,"texture":"slash_abyss.png","use_source_rect":True},"Transform":make_transform(-5000,-5000)}}

def make_crawler_template(eid=1013):
    return {**make_template("AbyssCrawlerTemplate",eid,["abyss_crawler"],"crawler_abyss.png",48,34,
        True,{"animations":{"idle":{"fps":8,"frames":[0,1,2,3],"loop":True}},"current_animation":"idle","default_fps":8,"frame_height":64,"frame_width":64,"loop":True,"playing":True,"sheet_columns":4,"sheet_padding":0,"sheet_rows":1,"sheet_spacing":0,"speed":1.0,"sprite_sheet":"crawler_abyss.png","use_sprite_sheet":True},
        extra={"alert_range":560.0,"damage":1,"hp":3,"jump_force":640.0,"patrol_range":180.0,"speed":95.0})}

def make_boundary(name, eid, x, y, w, h, sx=1.0, sy=1.0):
    return {"active":True,"children":[],"id":eid,"name":name,"components":{"BoxCollider2D":{"bounciness":0.0,"friction":0.7,"height":h,"is_trigger":False,"offset_x":0,"offset_y":0,"width":w},"SpriteRenderer":{"color":[0,0,0,0],"flip_x":False,"flip_y":False,"layer":0,"opacity":0.0,"order_in_layer":0,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"","use_source_rect":False},"Transform":make_transform(x,y,sx,sy)}}

def make_tilemap(name, eid, tileset, grid, tile_size=64, gen_colliders=True):
    return {
        "active": True, "children": [], "id": eid, "name": name,
        "components": {
            "Transform": make_transform(),
            "Tilemap": {
                "filter_mode": "point",
                "generate_colliders": gen_colliders,
                "grid": grid,
                "tile_size": tile_size,
                "tileset": tileset
            }
        }
    }

def make_checkpoint(name, eid, x, y, label, color=[120,220,255,255]):
    return {
        "active":True,"children":[],"checkpoint_name":label,"id":eid,"name":name,
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.0,"height":52,"is_trigger":True,"offset_x":0,"offset_y":0,"width":36},
            "Light2D":{"color":color,"intensity":0.9,"radius":220.0},
            "ScriptComponent":{"scripts":["abyss_checkpoint"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":7,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"checkpoint_abyss.png","use_source_rect":False},
            "Transform":make_transform(x,y)
        }
    }

def make_portal(name, eid, x, y, target_scene, target_spawn_name, tx, ty, color=[120,220,255,255], require_boss=False, sx=1.5, sy=1.5):
    return {
        "active":True,"children":[],"id":eid,"name":name,
        "target_scene":target_scene,"target_spawn_name":target_spawn_name,
        "target_spawn_x":float(tx),"target_spawn_y":float(ty),
        "require_boss_defeated":1 if require_boss else 0,
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.0,"height":80,"is_trigger":True,"offset_x":0,"offset_y":0,"width":50},
            "Light2D":{"color":color,"intensity":1.1,"radius":220.0},
            "ScriptComponent":{"field_overrides":{"abyss_portal":{}},"scripts":["abyss_portal"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":11,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"portal_abyss.png","use_source_rect":False},
            "Transform":make_transform(x,y,sx,sy)
        }
    }

def make_enemy(name, eid, x, y, enemy_type, hp=3, speed=95, patrol=180, alert=500, jump=640, color=None, sx=1.0, sy=1.0):
    if enemy_type == "crawler":
        return {
            "active":True,"children":[],"id":eid,"name":name,"team":2,
            "hp":hp,"damage":1,"speed":speed,"patrol_range":float(patrol),"alert_range":float(alert),"jump_force":float(jump),
            "components":{
                "Animator":{"animations":{"idle":{"fps":8,"frames":[0,1,2,3],"loop":True}},"current_animation":"idle","default_fps":8,"frame_height":64,"frame_width":64,"loop":True,"playing":True,"sheet_columns":4,"sheet_padding":0,"sheet_rows":1,"sheet_spacing":0,"speed":1.0,"sprite_sheet":"crawler_abyss.png","use_sprite_sheet":True},
                "BoxCollider2D":{"bounciness":0.0,"friction":0.4,"height":34,"is_trigger":False,"offset_x":0,"offset_y":0,"width":48},
                "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":1.0,"is_kinematic":False,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},
                "ScriptComponent":{"scripts":["abyss_crawler"]},
                "SpriteRenderer":{"color":color or [255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":0.95,"order_in_layer":8,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"crawler_abyss.png","use_source_rect":False},
                "Transform":make_transform(x,y,sx,sy)
            }
        }
    elif enemy_type == "spitter":
        return {
            "active":True,"children":[],"id":eid,"name":name,"team":2,
            "hp":hp,"damage":1,"speed":speed,"fire_rate":1.8,"fire_range":float(alert),"alert_range":float(alert),
            "components":{
                "Animator":{"animations":{"idle":{"fps":6,"frames":[0,1,2,3],"loop":True}},"current_animation":"idle","default_fps":6,"frame_height":32,"frame_width":32,"loop":True,"playing":True,"sheet_columns":4,"sheet_padding":0,"sheet_rows":1,"sheet_spacing":0,"speed":1.0,"sprite_sheet":"spitter_abyss.png","use_sprite_sheet":True},
                "BoxCollider2D":{"bounciness":0.0,"friction":0.4,"height":28,"is_trigger":False,"offset_x":0,"offset_y":0,"width":28},
                "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":1.0,"is_kinematic":False,"mass":1.0,"velocity_x":0.0,"velocity_y":0.0},
                "ScriptComponent":{"scripts":["abyss_spitter"]},
                "SpriteRenderer":{"color":color or [255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":0.95,"order_in_layer":8,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"spitter_abyss.png","use_source_rect":False},
                "Transform":make_transform(x,y,sx,sy)
            }
        }
    elif enemy_type == "turret":
        return {
            "active":True,"children":[],"id":eid,"name":name,"team":2,
            "hp":hp,"damage":1,"fire_rate":1.2,"fire_range":float(alert),
            "components":{
                "BoxCollider2D":{"bounciness":0.0,"friction":0.4,"height":36,"is_trigger":False,"offset_x":0,"offset_y":0,"width":36},
                "ScriptComponent":{"scripts":["abyss_turret"]},
                "SpriteRenderer":{"color":color or [255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":0.95,"order_in_layer":8,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"turret_abyss.png","use_source_rect":False},
                "Transform":make_transform(x,y,sx,sy)
            }
        }

def make_lantern(name, eid, x, y, color=[200,180,100,255], radius=180.0, intensity=0.8):
    return {
        "active":True,"children":[],"id":eid,"name":name,
        "components":{
            "Light2D":{"color":color,"intensity":intensity,"radius":radius},
            "ScriptComponent":{"scripts":["abyss_lantern"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":6,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"lantern_abyss.png","use_source_rect":False},
            "Transform":make_transform(x,y)
        }
    }

def make_spike(name, eid, x, y, w=64, h=32):
    return {
        "active":True,"children":[],"id":eid,"name":name,"team":0,
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.0,"height":h,"is_trigger":True,"offset_x":0,"offset_y":0,"width":w},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":5,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"spike_abyss.png","use_source_rect":False},
            "Transform":make_transform(x,y)
        }
    }

def make_crystal_prop(name, eid, x, y, sx=1.0, sy=1.0):
    return {"active":True,"children":[],"id":eid,"name":name,"components":{"SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":1,"opacity":1.0,"order_in_layer":3,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"crystal_prop_abyss.png","use_source_rect":False},"Transform":make_transform(x,y,sx,sy)}}

def make_rubble_prop(name, eid, x, y, sx=1.0, sy=1.0):
    return {"active":True,"children":[],"id":eid,"name":name,"components":{"SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":1,"opacity":0.9,"order_in_layer":3,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"rubble_prop_abyss.png","use_source_rect":False},"Transform":make_transform(x,y,sx,sy)}}

def make_lift(name, eid, x, y, travel_dist=200, speed=80):
    return {
        "active":True,"children":[],"id":eid,"name":name,
        "travel_distance":float(travel_dist),"lift_speed":float(speed),
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.8,"height":20,"is_trigger":False,"offset_x":0,"offset_y":0,"width":120},
            "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":0.0,"is_kinematic":True,"mass":10.0,"velocity_x":0.0,"velocity_y":0.0},
            "ScriptComponent":{"scripts":["abyss_lift"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":5,"pixels_per_unit":1.0,"source_h":0,"source_w":0,"source_x":0,"source_y":0,"texture":"tile_bridge.png","use_source_rect":False},
            "Transform":make_transform(x,y)
        }
    }

SORTING_LAYERS = [{"id":0,"name":"Background"},{"id":1,"name":"Terrain"},{"id":2,"name":"Default"},{"id":3,"name":"Effects"},{"id":4,"name":"UI"}]

def empty_grid(rows, cols, fill=-1):
    return [[fill]*cols for _ in range(rows)]

# ═══════════════════════════════════════════════════════════════════════════════
# SCENE 1: scene_home.json  — "Hollow Spire" (hub / starting area)
# A moody vertical hub with platforms, leading to two portals.
# Tileset: rock main, moss accent. Vertical layout, lots of height.
# 72 cols x 36 rows = 4608 x 2304 world units
# ═══════════════════════════════════════════════════════════════════════════════
def build_scene_home():
    ROWS, COLS = 36, 72
    rock = empty_grid(ROWS, COLS)
    moss = empty_grid(ROWS, COLS)
    # Solid ground band at bottom
    for r in range(28, ROWS):
        for c in range(COLS):
            rock[r][c] = 0
    # Left wall cliff (tall)
    for r in range(6, 28):
        for c in range(0, 3):
            rock[r][c] = 0
    # Right wall cliff
    for r in range(10, 28):
        for c in range(69, 72):
            rock[r][c] = 0
    # Floating platforms — varied heights forming ascent
    platforms = [
        (22, 8, 14),   # (row, col_start, col_end)
        (18, 18, 26),
        (14, 32, 42),
        (20, 46, 54),
        (16, 56, 64),
        (11, 28, 36),
        (8,  14, 22),
        (7,  50, 60),
    ]
    for row, cs, ce in platforms:
        for c in range(cs, ce):
            rock[row][c] = 0
        # top of platform gets moss
        for c in range(cs, ce):
            moss[row][c] = 0
    # Ceiling partial overhang left
    for r in range(0, 4):
        for c in range(0, 16):
            rock[r][c] = 0
    # Ceiling partial overhang right
    for r in range(0, 4):
        for c in range(58, 72):
            rock[r][c] = 0
    # Moss on ground surface
    for c in range(COLS):
        moss[27][c] = 0
    # Some moss on left wall top
    for c in range(0,3):
        moss[6][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1
    # parallax
    for name,tex,depth,spd in [
        ("AbyssSkyDeep","bg_abyss_deep.png",-4500,0.018),
        ("AbyssSkyMid","bg_abyss_mid.png",-3000,0.04),
        ("AbyssSkyFront","bg_abyss_front.png",-1500,0.08),
        ("AbyssMist","bg_abyss_mist.png",-800,0.15),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd)); eid+=1
    # tilemaps
    entities.append(make_tilemap("HomeRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("HomeMossDetail",eid,"tile_moss.png",moss,gen_colliders=False)); eid+=1
    # HUD
    for e in make_hud_entities(eid): entities.append(e); eid+=1
    # Player spawns bottom left safe spot
    entities.append(make_player(380, 1680, eid)); eid+=1
    # Templates
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1
    # Boundaries
    entities.append(make_boundary("LeftBoundary",eid,-80,1152,120,2304)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,4688,1152,120,2304)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,2304,0,4608,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,2304,2350,4608,80)); eid+=1
    # Checkpoints
    entities.append(make_checkpoint("Checkpoint_Home",eid,380,1600,"Hollow Spire",[100,255,180,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Spire_High",eid,960,448,"Spire Summit",[140,200,255,255])); eid+=1
    # Portals: to Ascent and to Crystal Hall
    entities.append(make_portal("Portal_To_Ascent",eid,4400,1536,"scene_ascent.json","Abyssal Ascent",620,1000,[120,220,255,255])); eid+=1
    entities.append(make_portal("Portal_To_Crystal",eid,1450,448,"scene_crystal.json","Crystal Hall",620,900,[180,100,255,255])); eid+=1
    # Lanterns
    for lx, ly, c in [
        (380,1520,[200,160,80,255]),(960,1520,[160,220,255,255]),(2200,1728,[200,160,80,255]),
        (3200,1440,[200,160,80,255]),(4250,1600,[120,200,255,255]),(960,380,[200,100,255,255])
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,c)); eid+=1
    # A few crawlers guarding platforms
    for ex,ey in [(1280,1728),(2240,1728),(3200,1728),(1150,1216),(2050,1024)]:
        entities.append(make_enemy(f"Crawler_{eid}",eid,ex,ey,"crawler",hp=3,speed=90)); eid+=1
    # Rubble props
    for px, py in [(700,1712),(1500,1712),(2500,1712),(3100,1712),(600,1136),(1700,1136)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1
    # Crystal props near portals
    for px,py in [(4300,1520),(4480,1536),(1300,384),(1600,384)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ═══════════════════════════════════════════════════════════════════════════════
# SCENE 2: scene_boss.json  — "The Abyssal Maw" (boss arena)
# Wide rectangular arena, pillars, floor of spikes at edges, dramatic lighting
# Tileset: rock. 72 cols x 28 rows = 4608 x 1792 world units
# ═══════════════════════════════════════════════════════════════════════════════
def build_scene_boss():
    ROWS, COLS = 28, 72
    rock = empty_grid(ROWS, COLS)
    crystal = empty_grid(ROWS, COLS)

    # Floor (thick)
    for r in range(22, ROWS):
        for c in range(COLS):
            rock[r][c] = 0
    # Ceiling
    for r in range(0, 3):
        for c in range(COLS):
            rock[r][c] = 0
    # Left wall
    for r in range(3, 22):
        for c in range(0, 3):
            rock[r][c] = 0
    # Right wall
    for r in range(3, 22):
        for c in range(69, 72):
            rock[r][c] = 0
    # Pillars — 4 dramatic columns
    for col_start in [12, 22, 46, 56]:
        for r in range(14, 22):
            for c in range(col_start, col_start+3):
                rock[r][c] = 0
        for c in range(col_start, col_start+3):
            crystal[13][c] = 0  # crystal caps
    # Balcony ledges mid-height
    for c in range(3, 12):
        rock[14][c] = 0
    for c in range(59, 69):
        rock[14][c] = 0
    # Crystal accent on floor surface
    for c in range(3, 69):
        crystal[21][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1
    for name,tex,depth,spd in [
        ("AbyssSkyDeep","bg_abyss_deep.png",-4500,0.010),
        ("AbyssSkyMid","bg_abyss_mid.png",-3000,0.025),
        ("AbyssSkyFront","bg_abyss_front.png",-1500,0.05),
        ("AbyssMist","bg_abyss_mist.png",-800,0.10),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=[220,200,255,255])); eid+=1
    entities.append(make_tilemap("BossArenaRock",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("BossArenaCrystal",eid,"tile_crystal.png",crystal,gen_colliders=False)); eid+=1
    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(380, 1280, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1
    entities.append(make_boundary("LeftBoundary",eid,-80,896,120,1792)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,4688,896,120,1792)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,2304,0,4608,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,2304,1850,4608,80)); eid+=1
    entities.append(make_checkpoint("Checkpoint_BossEntry",eid,380,1200,"The Abyssal Maw",[255,80,80,255])); eid+=1
    # Portal back to home (requires boss defeated)
    entities.append(make_portal("Portal_To_Home",eid,4200,1200,"scene_home.json","Hollow Spire",380,1680,[255,80,80,255],require_boss=True)); eid+=1
    # Boss entity
    entities.append({
        "active":True,"children":[],"id":eid,"name":"AbyssBoss","team":2,
        "hp":60,"max_hp":60,"damage":2,"speed":80.0,"phase":1,
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.3,"height":80,"is_trigger":False,"offset_x":0,"offset_y":0,"width":80},
            "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":1.0,"is_kinematic":False,"mass":5.0,"velocity_x":0.0,"velocity_y":0.0},
            "ScriptComponent":{"scripts":["abyss_boss"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":9,"pixels_per_unit":1.0,"source_h":64,"source_w":64,"source_x":0,"source_y":0,"texture":"boss_abyss.png","use_source_rect":True},
            "Transform":make_transform(2304,1260,2.0,2.0)
        }
    }); eid+=1
    # Spikes flanking center
    for sx in [500, 700, 900, 3400, 3600, 3800]:
        entities.append(make_spike(f"Spike_{eid}",eid,sx,1380)); eid+=1
    # Dramatic lanterns — red/purple boss atmosphere
    for lx, ly, c in [
        (380,1120,[255,60,60,255]),(800,880,[200,60,255,255]),
        (1800,960,[255,60,60,255]),(2300,256,[200,60,255,255]),
        (2900,960,[255,60,60,255]),(3800,880,[200,60,255,255]),
        (4200,1120,[255,60,60,255])
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,c,radius=280,intensity=1.2)); eid+=1
    # Crystal props around pillars
    for px,py in [(740,1340),(1390,1340),(2940,1340),(3580,1340)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.3,1.3)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ═══════════════════════════════════════════════════════════════════════════════
# SCENE 3: scene.json  — "The Sunken Gate" (menu / title screen)
# Original scene.json was the menu. Keep it as the title scene (just parallax + UI).
# ═══════════════════════════════════════════════════════════════════════════════
def build_scene_title():
    """Load and preserve the original title scene logic from the ascent file,
    but don't touch entities — we rebuild with proper menu entity names."""
    eid = 1000
    entities = []
    entities.append({"active":True,"children":[],"id":eid,"name":"Camera","components":{"Camera2D":{"background_color":[4,3,12,255],"ortho_size":540.0,"zoom":1.0},"Transform":make_transform(0,0)}}); eid+=1
    for name,tex,depth,spd,op in [
        ("Menu_Backdrop","bg_abyss_deep.png",-4500,0.025,0.95),
        ("Menu_BackdropMid","bg_abyss_mid.png",-3000,0.045,0.90),
        ("Menu_BackdropFront","bg_abyss_front.png",-1500,0.07,0.85),
        ("Menu_Mist","bg_abyss_mist.png",-800,0.12,0.75),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,op)); eid+=1
    # Menu controller
    entities.append({"active":True,"children":[],"id":eid,"name":"MenuController","components":{"ScriptComponent":{"scripts":["abyss_menu_controller"]},"Transform":make_transform()}}); eid+=1
    # HUD elements for menu
    for e in make_hud_entities(eid): entities.append(e); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ═══════════════════════════════════════════════════════════════════════════════
# SCENE 4: scene_verdant.json  — "Verdant Hollow" (rework — lush underground)
# Organic winding cavern, heavy moss/bridge mix, lifts, lots of enemies.
# 72 cols x 32 rows
# ═══════════════════════════════════════════════════════════════════════════════
def build_scene_verdant():
    ROWS, COLS = 32, 72
    rock = empty_grid(ROWS, COLS)
    moss = empty_grid(ROWS, COLS)
    bridge = empty_grid(ROWS, COLS)

    # Bottom solid ground (thicker, uneven)
    ground_heights = [25]*72
    # Wave the ground surface
    for c in range(COLS):
        base = 25
        if 15 <= c <= 25: base = 23  # dip
        if 40 <= c <= 50: base = 24  # dip
        ground_heights[c] = base
    for c in range(COLS):
        for r in range(ground_heights[c], ROWS):
            rock[r][c] = 0
    # Moss on ground surface
    for c in range(COLS):
        moss[ground_heights[c]][c] = 0

    # Ceiling — organic, not flat
    ceil_heights = [3]*72
    for c in range(COLS):
        if 10 <= c <= 20: ceil_heights[c] = 4
        if 30 <= c <= 40: ceil_heights[c] = 5
        if 50 <= c <= 62: ceil_heights[c] = 4
    for c in range(COLS):
        for r in range(0, ceil_heights[c]):
            rock[r][c] = 0

    # Left wall
    for r in range(ceil_heights[0], ground_heights[0]):
        for c in range(0, 2):
            rock[r][c] = 0
    # Right wall
    for r in range(ceil_heights[71], ground_heights[71]):
        for c in range(70, 72):
            rock[r][c] = 0

    # Mid-air platforms (mixed rock/bridge)
    plat_defs = [
        ("rock", 20, 6, 14),
        ("bridge", 17, 16, 24),
        ("moss", 15, 28, 36),
        ("bridge", 19, 38, 46),
        ("rock", 13, 48, 56),
        ("bridge", 18, 58, 66),
        ("rock", 11, 20, 30),
        ("bridge", 9, 38, 50),
        ("rock", 16, 4, 10),
    ]
    for tileset_name, row, cs, ce in plat_defs:
        for c in range(cs, ce):
            if tileset_name == "bridge":
                bridge[row][c] = 0
            else:
                rock[row][c] = 0
                moss[row][c] = 0

    # Hanging stalactites from ceiling
    for c in range(5, 68, 8):
        h = 3 + ((c*7) % 4)
        for r in range(ceil_heights[c], ceil_heights[c]+h):
            rock[r][c] = 0
            rock[r][c+1] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1
    for name,tex,depth,spd,color in [
        ("AbyssSkyDeep","bg_abyss_deep.png",-4500,0.020,[200,255,200,255]),
        ("AbyssSkyMid","bg_abyss_mid.png",-3000,0.042,[180,240,180,255]),
        ("AbyssSkyFront","bg_abyss_front.png",-1500,0.078,[160,220,160,255]),
        ("AbyssMist","bg_abyss_mist.png",-800,0.15,[180,255,180,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=color)); eid+=1
    entities.append(make_tilemap("VerdantRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("VerdantMossDetail",eid,"tile_moss.png",moss,gen_colliders=False)); eid+=1
    entities.append(make_tilemap("VerdantBridgeTerrain",eid,"tile_bridge.png",bridge)); eid+=1
    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(280, 1440, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1
    entities.append(make_boundary("LeftBoundary",eid,-80,1024,120,2048)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,4688,1024,120,2048)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,2304,0,4608,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,2304,2100,4608,80)); eid+=1
    entities.append(make_checkpoint("Checkpoint_Verdant",eid,280,1360,"Verdant Hollow",[80,255,120,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_VerdantDeep",eid,2880,960,"Deep Roots",[80,200,100,255])); eid+=1
    entities.append(make_portal("Portal_To_Home",eid,160,1392,"scene_home.json","Hollow Spire",380,1680,[100,255,140,255])); eid+=1
    entities.append(make_portal("Portal_To_Flooded",eid,4420,1424,"scene_flooded.json","Flooded Ruins",620,1020,[60,180,255,255])); eid+=1
    # Moving lifts
    for lx, ly, dist, spd in [(896,1360,160,70),(1920,1280,200,80),(3072,1200,240,90)]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1
    # Enemies — mix
    for ex,ey,etype,hp,spd in [
        (640,1488,"crawler",3,90),(1100,1488,"spitter",2,0),
        (1700,928,"crawler",3,100),(2200,1312,"turret",2,0),
        (2600,1488,"crawler",4,110),(2900,928,"spitter",2,0),
        (3400,832,"crawler",3,95),(3800,1488,"turret",2,0),
        (4100,1488,"crawler",4,100),
    ]:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd)); eid+=1
    # Lanterns — green tinted
    for lx,ly in [(280,1280),(900,1100),(1600,800),(2200,1200),(2900,800),(3500,1200),(4100,1300)]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,[100,220,100,255],radius=200,intensity=0.85)); eid+=1
    # Spikes in dip areas
    for sx in [1000,1064,1128, 2560,2624,2688]:
        entities.append(make_spike(f"Spike_{eid}",eid,sx,1520)); eid+=1
    for px,py in [(500,1472),(1400,1472),(2100,1312),(3000,1472),(3700,1472)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1
    for px,py in [(1600,752),(2900,752),(3500,768)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ═══════════════════════════════════════════════════════════════════════════════
# SCENE 5: scene_flooded.json  — "Flooded Ruins" (rework — submerged depths)
# Wide horizontal dungeon, sunken pillars, more turrets/spitters, dramatic blues
# 72 cols x 30 rows
# ═══════════════════════════════════════════════════════════════════════════════
def build_scene_flooded():
    ROWS, COLS = 30, 72
    rock = empty_grid(ROWS, COLS)
    crystal = empty_grid(ROWS, COLS)

    # Ground — irregular, sunken ruin style
    for r in range(24, ROWS):
        for c in range(COLS):
            rock[r][c] = 0
    # Sunken columns / ruins (thick pillars)
    pillar_cols = [8, 18, 30, 44, 56, 66]
    for pc in pillar_cols:
        for r in range(18, 24):
            for c in range(pc, pc+3):
                rock[r][c] = 0
    # Sub-platform ledges (ruins of upper floor)
    ruin_ledges = [
        (20, 3, 8), (19, 12, 18), (20, 22, 29), (18, 33, 41),
        (21, 47, 54), (19, 58, 65),
        (15, 5, 14), (14, 24, 34), (15, 46, 56), (13, 34, 42),
    ]
    for row, cs, ce in ruin_ledges:
        for c in range(cs, ce):
            rock[row][c] = 0
    # Crystal accents on platforms
    for row, cs, ce in ruin_ledges[::2]:
        for c in range(cs, ce):
            crystal[row][c] = 0
    # Ceiling — crumbling, has gaps
    for r in range(0,3):
        for c in range(COLS):
            if not (22 <= c <= 26 or 45 <= c <= 49):
                rock[r][c] = 0
    # Left wall
    for r in range(3, 24):
        for c in range(0, 2):
            rock[r][c] = 0
    # Right wall
    for r in range(3, 24):
        for c in range(70, 72):
            rock[r][c] = 0
    # Crystal on floor surface
    for c in range(2, 70):
        crystal[23][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1
    for name,tex,depth,spd,color in [
        ("AbyssSkyDeep","bg_abyss_deep.png",-4500,0.015,[100,140,220,255]),
        ("AbyssSkyMid","bg_abyss_mid.png",-3000,0.035,[80,120,200,255]),
        ("AbyssSkyFront","bg_abyss_front.png",-1500,0.065,[60,100,180,255]),
        ("AbyssMist","bg_abyss_mist.png",-800,0.12,[80,160,240,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=color)); eid+=1
    entities.append(make_tilemap("FloodedRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("FloodedCrystalAccent",eid,"tile_crystal.png",crystal,gen_colliders=False)); eid+=1
    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(200, 1380, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1
    entities.append(make_boundary("LeftBoundary",eid,-80,960,120,1920)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,4688,960,120,1920)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,2304,0,4608,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,2304,1980,4608,80)); eid+=1
    entities.append(make_checkpoint("Checkpoint_Flooded",eid,200,1300,"Flooded Ruins",[60,160,255,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Flooded_Deep",eid,2900,1220,"Drowned Archive",[60,120,220,255])); eid+=1
    entities.append(make_portal("Portal_To_Verdant",eid,100,1280,"scene_verdant.json","Verdant Hollow",280,1440,[80,220,120,255])); eid+=1
    entities.append(make_portal("Portal_To_Boss",eid,4430,1300,"scene_boss.json","The Abyssal Maw",380,1280,[255,80,80,255],require_boss=False)); eid+=1
    # Dense enemy gauntlet — fits flooded ruin feel
    for ex,ey,etype,hp,spd in [
        (500,1540,"turret",2,0),(900,1540,"spitter",3,80),
        (1300,1220,"crawler",3,90),(1500,1540,"turret",2,0),
        (1900,1220,"spitter",3,0),(2200,1220,"crawler",4,100),
        (2500,1220,"turret",3,0),(2800,1540,"spitter",3,80),
        (3200,1220,"crawler",4,110),(3500,1540,"turret",3,0),
        (3900,1220,"spitter",3,0),(4100,1540,"crawler",4,100),
    ]:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd,color=[100,140,255,255])); eid+=1
    # Moving lifts crossing water sections
    for lx,ly,dist,spd in [(700,1500,200,60),(2100,1400,240,65),(3600,1500,200,70)]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1
    # Spikes on floor at gaps
    for sx in [400,464,528, 2000,2064, 3300,3364,3428]:
        entities.append(make_spike(f"Spike_{eid}",eid,sx,1560)); eid+=1
    # Blue-tinted lanterns
    for lx,ly in [(200,1220),(750,1060),(1400,860),(2100,860),(2750,1060),(3400,860),(4100,1060)]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,[60,140,255,255],radius=220,intensity=0.9)); eid+=1
    for px,py in [(600,1540),(1200,1540),(2200,1540),(3200,1540),(4000,1540)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1
    for px,py in [(1400,820),(2100,820),(2750,1020),(3400,820)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.2,1.2)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ── write all scenes ──────────────────────────────────────────────────────────
scenes = {
    "scene.json":         build_scene_title(),
    "scene_home.json":    build_scene_home(),
    "scene_boss.json":    build_scene_boss(),
    "scene_verdant.json": build_scene_verdant(),
    "scene_flooded.json": build_scene_flooded(),
}

# ═══════════════════════════════════════════════════════════════════════════════
# NOVA EPIC MAPS — Silksong-scale adventures
# Each map is much larger, denser, more detailed, more enemies, more atmosphere.
# ═══════════════════════════════════════════════════════════════════════════════

import math

def noise2d(x, y, seed=0):
    """Simple deterministic smooth noise, no deps needed."""
    n = int(x) + int(y)*57 + seed*131
    n = (n << 13) ^ n
    return 1.0 - ((n * (n*n*15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0

def smooth_noise(x, y, seed=0):
    corners = (noise2d(x-1,y-1,seed)+noise2d(x+1,y-1,seed)+noise2d(x-1,y+1,seed)+noise2d(x+1,y+1,seed))/16.0
    sides   = (noise2d(x-1,y,seed)+noise2d(x+1,y,seed)+noise2d(x,y-1,seed)+noise2d(x,y+1,seed))/8.0
    center  = noise2d(x,y,seed)/4.0
    return corners + sides + center

def fractal_noise(x, y, octaves=4, persistence=0.5, seed=0):
    total, amp, freq, max_v = 0.0, 1.0, 1.0, 0.0
    for _ in range(octaves):
        total += smooth_noise(x*freq, y*freq, seed) * amp
        max_v += amp; amp *= persistence; freq *= 2
    return total / max_v

# ─── Scene: scene_ascent.json ─────────────────────────────────────────────────
# "The Shattered Spire" — epic vertical climb, 100 cols x 64 rows (6400 x 4096)
# Winding vertical shaft with overhangs, secret ledges, mixed biomes climbing.
# ─────────────────────────────────────────────────────────────────────────────
def build_scene_ascent():
    ROWS, COLS = 64, 100
    rock   = empty_grid(ROWS, COLS)
    moss   = empty_grid(ROWS, COLS)
    bridge = empty_grid(ROWS, COLS)

    # ── Outer shell: left/right walls with varied thickness ──
    for r in range(ROWS):
        lw = 3 + int(abs(smooth_noise(0, r*0.18, 7)) * 4)
        rw = 3 + int(abs(smooth_noise(1, r*0.18, 11)) * 4)
        for c in range(lw): rock[r][c] = 0
        for c in range(COLS-rw, COLS): rock[r][c] = 0

    # ── Floor (thick) ──
    for r in range(56, ROWS):
        for c in range(COLS): rock[r][c] = 0

    # ── Ceiling ──
    for r in range(0, 3):
        for c in range(COLS): rock[r][c] = 0

    # ── Winding interior walls carving vertical corridor ──
    # Left interior protrusions every ~8 rows
    protrude_left = [
        (8,  20, 38),   # (row, col_start, col_end) — rock block
        (16, 15, 34),
        (24, 22, 40),
        (32, 12, 30),
        (40, 18, 36),
        (48, 20, 42),
    ]
    for row, cs, ce in protrude_left:
        for r in range(row, row+4):
            for c in range(cs, ce):
                rock[r][c] = 0

    protrude_right = [
        (12, 62, 82),
        (20, 65, 85),
        (28, 60, 78),
        (36, 64, 84),
        (44, 62, 80),
        (52, 65, 83),
    ]
    for row, cs, ce in protrude_right:
        for r in range(row, row+4):
            for c in range(cs, ce):
                rock[r][c] = 0

    # ── Floating platforms — staggered ascending path ──
    ascent_plats = [
        ("bridge", 53, 40, 58),
        ("rock",   50, 18, 32),
        ("bridge", 47, 62, 76),
        ("moss",   44, 30, 48),
        ("bridge", 41, 55, 70),
        ("rock",   38, 20, 38),
        ("bridge", 35, 65, 80),
        ("moss",   32, 35, 52),
        ("bridge", 29, 15, 30),
        ("rock",   26, 60, 78),
        ("bridge", 23, 28, 44),
        ("moss",   20, 52, 66),
        ("bridge", 17, 22, 36),
        ("rock",   14, 58, 74),
        ("bridge", 11, 38, 54),
        ("moss",    8, 25, 40),
        ("rock",    6, 52, 68),
    ]
    for kind, row, cs, ce in ascent_plats:
        for c in range(cs, ce):
            if kind == "bridge": bridge[row][c] = 0
            elif kind == "moss": rock[row][c] = 0; moss[row][c] = 0
            else: rock[row][c] = 0
        # Thin pillar support under each platform (rock only)
        mid = (cs+ce)//2
        for r in range(row+1, min(row+5, ROWS)):
            if rock[r][mid] == -1: rock[r][mid] = 0

    # ── Hanging stalactites from protrusions ──
    for (row, cs, ce) in protrude_left + protrude_right:
        for c in range(cs, ce, 3):
            for r in range(row+4, min(row+4+3, ROWS)):
                if rock[r][c] == -1: rock[r][c] = 0

    # ── Moss top of rock platforms ──
    for r in range(4, ROWS-1):
        for c in range(COLS):
            if rock[r][c] == 0 and rock[r-1][c] == -1:
                moss[r][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1

    # Parallax — purple-blue climbing atmosphere
    for name,tex,depth,spd,color in [
        ("AscentSkyDeep","bg_abyss_deep.png",-5000,0.012,[120,80,200,255]),
        ("AscentSkyMid","bg_abyss_mid.png",-3200,0.030,[100,60,180,255]),
        ("AscentSkyFront","bg_abyss_front.png",-1600,0.060,[80,40,160,255]),
        ("AscentMist","bg_abyss_mist.png",-900,0.12,[140,80,220,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=color)); eid+=1

    entities.append(make_tilemap("AscentRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("AscentMossDetail",eid,"tile_moss.png",moss,gen_colliders=False)); eid+=1
    entities.append(make_tilemap("AscentBridgeTerrain",eid,"tile_bridge.png",bridge)); eid+=1

    for e in make_hud_entities(eid): entities.append(e); eid+=1

    # Player spawns at bottom center
    entities.append(make_player(3200, 3456, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1

    W, H = COLS*64, ROWS*64
    entities.append(make_boundary("LeftBoundary",eid,-80,H//2,120,H)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,W+80,H//2,120,H)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,W//2,-40,W,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,W//2,H+40,W,80)); eid+=1

    # Checkpoints — one per zone of ascent
    entities.append(make_checkpoint("Checkpoint_Ascent_Base",eid,3200,3360,"Shattered Spire Base",[120,80,255,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Ascent_Mid",eid,2800,2048,"Midspire Ledge",[140,100,255,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Ascent_Top",eid,3400,640,"Spire Summit",[180,140,255,255])); eid+=1

    # Portals: bottom from Home, top to Deep
    entities.append(make_portal("Portal_From_Home",eid,200,3456,"scene_home.json","Hollow Spire",4400,1536,[120,80,255,255])); eid+=1
    entities.append(make_portal("Portal_To_Deep",eid,3200,256,"scene_deep.json","The Sunken Deep",3200,3800,[60,60,200,255])); eid+=1

    # Enemies — get harder as you ascend
    enemy_defs = [
        # bottom zone (easy)
        (2560,3392,"crawler",3,85,160), (3840,3392,"crawler",3,90,180),
        (1600,3200,"spitter",2,0,400),  (4200,3200,"spitter",2,0,380),
        # mid zone
        (1800,2240,"crawler",4,100,200),(3200,2240,"turret",3,0,500),
        (4000,2176,"crawler",4,105,200),(2400,2048,"spitter",3,0,420),
        (1200,1920,"crawler",5,110,220),(4600,1920,"turret",3,0,500),
        # upper zone (hard)
        (2000,1280,"crawler",5,115,240),(3600,1280,"spitter",4,0,460),
        (2800,1088,"turret",4,0,520),   (1400,960,"crawler",6,120,260),
        (4200,896,"crawler",6,125,260), (3000,704,"turret",4,0,540),
        (2200,576,"spitter",5,0,480),   (3800,512,"crawler",7,130,280),
    ]
    for ex,ey,etype,hp,spd,alert in enemy_defs:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd,alert=alert)); eid+=1

    # Lifts on some platforms to help vertical traversal
    for lx,ly,dist,spd in [
        (2560,3360,240,75),(3840,3328,280,80),(1800,2240,320,85),
        (4000,2112,320,85),(1200,1856,360,90),(4600,1856,360,90),
    ]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1

    # Spikes in tight corridors
    spike_spots = [
        (1700,3456),(1764,3456),(1828,3456),
        (4300,3456),(4364,3456),(4428,3456),
        (2000,2368),(2064,2368),(1200,1088),(1264,1088),(1328,1088),
        (4500,1024),(4564,1024),(4628,1024),
    ]
    for sx,sy in spike_spots:
        entities.append(make_spike(f"Spike_{eid}",eid,sx,sy)); eid+=1

    # Lanterns — ascending purple→blue gradient
    lantern_defs = [
        (3200,3320,[180,120,255,255],200,0.9),(1200,3200,[160,100,240,255],180,0.85),
        (4800,3200,[160,100,240,255],180,0.85),(2000,2560,[140,80,220,255],200,0.9),
        (4200,2560,[140,80,220,255],200,0.9),(3200,2048,[120,80,200,255],220,0.95),
        (1800,1600,[100,80,200,255],220,0.95),(4400,1600,[100,80,200,255],220,0.95),
        (2600,1152,[80,60,180,255],240,1.0),(3600,1088,[80,60,180,255],240,1.0),
        (3200,640,[60,40,160,255],260,1.1),(2200,512,[60,40,160,255],260,1.1),
    ]
    for lx,ly,c,r,i in lantern_defs:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,c,r,i)); eid+=1

    # Props
    for px,py in [(2400,3456),(3600,3456),(1600,3264),(4400,3264),(2800,2240),(3200,2048)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1
    for px,py in [(2800,1984),(3600,1088),(2600,576),(3800,448)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.2,1.2)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ─── Scene: scene_crystal.json ────────────────────────────────────────────────
# "Crystal Labyrinth" — 96 cols x 40 rows, sprawling horizontal with vertical pockets
# Dense crystal formations, maze-like dead ends, secret rooms, hard enemies.
# ─────────────────────────────────────────────────────────────────────────────
def build_scene_crystal():
    ROWS, COLS = 40, 96
    rock    = empty_grid(ROWS, COLS)
    crystal = empty_grid(ROWS, COLS)
    moss    = empty_grid(ROWS, COLS)

    # ── Solid outer shell ──
    for r in range(0, 3):
        for c in range(COLS): rock[r][c] = 0
    for r in range(33, ROWS):
        for c in range(COLS): rock[r][c] = 0
    for r in range(3, 33):
        for c in range(0, 3): rock[r][c] = 0
        for c in range(93, 96): rock[r][c] = 0

    # ── Interior rock masses — create maze-like rooms ──
    # Left section: descending cavern
    for r in range(8, 20):
        for c in range(18, 28): rock[r][c] = 0
    for r in range(14, 26):
        for c in range(6, 16): rock[r][c] = 0

    # Mid section: crystal tower
    for r in range(6, 28):
        for c in range(42, 52): rock[r][c] = 0
    for r in range(12, 22):
        for c in range(52, 58): rock[r][c] = 0

    # Right section: upper gallery
    for r in range(4, 16):
        for c in range(70, 82): rock[r][c] = 0
    for r in range(18, 28):
        for c in range(78, 90): rock[r][c] = 0

    # ── Platforms carved out of masses ──
    crystal_plats = [
        (20, 3, 18),    (22, 28, 42),   (20, 58, 70),
        (24, 72, 82),   (17, 5, 15),    (16, 30, 42),
        (14, 62, 72),   (12, 22, 34),   (11, 54, 66),
        (9,  18, 30),   (8,  64, 76),   (7,  36, 50),
        (6,  80, 92),   (5,  10, 22),
    ]
    for row, cs, ce in crystal_plats:
        for c in range(cs, ce):
            crystal[row][c] = 0

    # ── Ground floor irregularity ──
    for c in range(3, 93):
        h = 32 + int(abs(smooth_noise(c*0.15, 0, 42)) * 1)
        for r in range(h, 33): pass  # already solid
        crystal[32][c] = 0  # crystal on floor

    # ── Crystal stalactites from ceiling ──
    for c in range(5, 91, 5):
        drop = 2 + int(abs(fractal_noise(c*0.12, 0, 3, 0.5, 99)) * 5)
        for r in range(3, 3+drop):
            rock[r][c] = 0
            crystal[r][c] = 0

    # ── Crystal stalagmites from floor ──
    for c in range(7, 91, 6):
        if rock[32][c] == -1: continue
        rise = 1 + int(abs(fractal_noise(c*0.10, 1, 3, 0.5, 77)) * 4)
        for r in range(33-rise, 33):
            crystal[r][c] = 0

    # ── Moss on top of rock platforms ──
    for r in range(1, ROWS-1):
        for c in range(COLS):
            if rock[r][c] == 0 and rock[r-1][c] == -1 and crystal[r-1][c] == -1:
                moss[r][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1

    # Cyan-purple crystalline atmosphere
    for name,tex,depth,spd,color in [
        ("CrystalSkyDeep","bg_abyss_deep.png",-4800,0.014,[40,180,200,255]),
        ("CrystalSkyMid","bg_abyss_mid.png",-3000,0.032,[30,160,180,255]),
        ("CrystalSkyFront","bg_abyss_front.png",-1500,0.060,[20,140,160,255]),
        ("CrystalMist","bg_abyss_mist.png",-800,0.11,[60,200,220,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=color)); eid+=1

    entities.append(make_tilemap("CrystalRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("CrystalAccentTerrain",eid,"tile_crystal.png",crystal,gen_colliders=False)); eid+=1
    entities.append(make_tilemap("CrystalMossDetail",eid,"tile_moss.png",moss,gen_colliders=False)); eid+=1

    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(200, 2048, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1

    W, H = COLS*64, ROWS*64
    entities.append(make_boundary("LeftBoundary",eid,-80,H//2,120,H)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,W+80,H//2,120,H)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,W//2,-40,W,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,W//2,H+40,W,80)); eid+=1

    entities.append(make_checkpoint("Checkpoint_Crystal_Entry",eid,200,1984,"Crystal Labyrinth Entry",[80,200,220,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Crystal_Mid",eid,2752,1152,"Crystal Core",[60,180,200,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Crystal_Vault",eid,5500,448,"Crystal Vault",[40,160,180,255])); eid+=1

    entities.append(make_portal("Portal_To_Home",eid,100,2048,"scene_home.json","Hollow Spire",1450,448,[80,200,220,255])); eid+=1
    entities.append(make_portal("Portal_To_Deep",eid,5900,2048,"scene_deep.json","The Sunken Deep",200,3800,[40,80,200,255])); eid+=1

    # Enemies — crystalline guardian types
    crystal_color = [140,220,255,255]
    enemy_defs = [
        (600,2048,"turret",3,0,480,crystal_color),
        (1200,2048,"spitter",3,80,420,crystal_color),
        (1900,1280,"crawler",4,100,260,crystal_color),
        (2400,1344,"turret",3,0,500,crystal_color),
        (2800,704,"crawler",4,105,280,[180,255,255,255]),
        (3200,1280,"spitter",4,0,460,crystal_color),
        (3600,1344,"turret",4,0,520,crystal_color),
        (4000,640,"crawler",5,110,300,[180,255,255,255]),
        (4400,1280,"crawler",5,115,300,crystal_color),
        (4800,768,"turret",4,0,520,crystal_color),
        (5200,448,"spitter",5,0,500,crystal_color),
        (5500,1280,"crawler",6,120,320,[180,255,255,255]),
        (5800,448,"turret",5,0,540,[220,255,255,255]),
        (1600,576,"spitter",4,0,480,crystal_color),
        (3000,512,"turret",4,0,520,crystal_color),
    ]
    for ex,ey,etype,hp,spd,alert,c in enemy_defs:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd,alert=alert,color=c)); eid+=1

    # Lifts in narrow crystal shafts
    for lx,ly,dist,spd in [
        (900,1984,280,65),(1900,1216,320,70),(3800,640,360,75),(5200,384,280,65)
    ]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1

    # Crystal spikes — dense
    for sx in range(320,5800,128):
        if sx % 384 < 192:
            entities.append(make_spike(f"Spike_{eid}",eid,sx,2112)); eid+=1

    # Cyan lanterns
    for lx,ly,r2,i in [
        (200,1920,180,0.9),(900,1280,200,0.95),(1600,512,200,0.95),
        (2200,640,200,1.0),(2800,640,220,1.0),(3400,1280,200,1.0),
        (4000,576,220,1.05),(4600,1280,200,1.0),(5200,320,220,1.05),
        (5600,1280,200,1.0),(3200,1920,180,0.9),(4400,1984,180,0.9),
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,[60,200,220,255],r2,i)); eid+=1

    for px,py in [(500,2048),(1200,2048),(2200,2048),(3500,2048),(4800,2048)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.3,1.3)); eid+=1
    for px,py in [(800,2048),(2000,1280),(3800,1280),(5500,448)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ─── Scene: scene_deep.json ───────────────────────────────────────────────────
# "The Sunken Deep" — 96 cols x 52 rows, massive oppressive underground.
# Extreme pressure aesthetic: thick walls, very tight corridors, crushing ceilings,
# narrow escape paths. Hardest non-boss area.
# ─────────────────────────────────────────────────────────────────────────────
def build_scene_deep():
    ROWS, COLS = 52, 96
    rock    = empty_grid(ROWS, COLS)
    crystal = empty_grid(ROWS, COLS)
    bridge  = empty_grid(ROWS, COLS)

    # ── Completely fill, then carve corridors ──
    for r in range(ROWS):
        for c in range(COLS):
            rock[r][c] = 0

    # ── Main horizontal corridor system ──
    # Ground level corridor (rows 40-48)
    for r in range(40, 48):
        for c in range(3, 93): rock[r][c] = -1

    # Upper corridor (rows 28-36)
    for r in range(28, 36):
        for c in range(3, 93): rock[r][c] = -1

    # Top corridor (rows 16-24)
    for r in range(16, 24):
        for c in range(3, 93): rock[r][c] = -1

    # Near-ceiling corridor (rows 5-12)
    for r in range(5, 12):
        for c in range(3, 93): rock[r][c] = -1

    # ── Vertical shafts connecting corridors ──
    shaft_xs = [10, 22, 34, 46, 58, 70, 82]
    for sx in shaft_xs:
        for r in range(12, 40): 
            for c in range(sx, sx+6): rock[r][c] = -1

    # ── Wide chambers at shaft intersections ──
    chambers = [
        (20, 18, 8, 10),  # (row, col, height, width)
        (20, 42, 8, 10),
        (20, 66, 8, 10),
        (32, 30, 6, 12),
        (32, 54, 6, 12),
        (8,  20, 5, 14),
        (8,  52, 5, 14),
        (8,  74, 5, 14),
    ]
    for cr, cc, ch, cw in chambers:
        for r in range(cr, cr+ch):
            for c in range(cc, cc+cw):
                rock[r][c] = -1

    # ── Re-add floor at very bottom ──
    for r in range(48, ROWS):
        for c in range(COLS): rock[r][c] = 0

    # ── Crystal veins in walls ──
    for r in range(ROWS):
        for c in range(COLS):
            if rock[r][c] == 0:
                v = abs(fractal_noise(c*0.08, r*0.08, 4, 0.5, 333))
                if v > 0.55: crystal[r][c] = 0

    # ── Bridge platforms in wide corridors ──
    bridge_plats = [
        (44, 12, 22), (44, 36, 46), (44, 60, 70), (44, 78, 88),
        (32, 8, 18),  (32, 40, 52), (32, 68, 80),
        (20, 24, 36), (20, 50, 62), (20, 76, 88),
        (9,  10, 22), (9,  34, 46), (9,  60, 72), (9,  80, 90),
    ]
    for row, cs, ce in bridge_plats:
        for c in range(cs, ce):
            if rock[row][c] == -1:
                bridge[row][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1

    # Dark, oppressive blue-black atmosphere
    for name,tex,depth,spd,color in [
        ("DeepSkyDeep","bg_abyss_deep.png",-5000,0.008,[20,30,60,255]),
        ("DeepSkyMid","bg_abyss_mid.png",-3200,0.020,[15,25,50,255]),
        ("DeepSkyFront","bg_abyss_front.png",-1600,0.040,[10,20,40,255]),
        ("DeepMist","bg_abyss_mist.png",-900,0.080,[30,40,80,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=color)); eid+=1

    entities.append(make_tilemap("DeepRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("DeepCrystalVeins",eid,"tile_crystal.png",crystal,gen_colliders=False)); eid+=1
    entities.append(make_tilemap("DeepBridgePlatforms",eid,"tile_bridge.png",bridge)); eid+=1

    for e in make_hud_entities(eid): entities.append(e); eid+=1

    W, H = COLS*64, ROWS*64
    # Player spawns in bottom-left ground corridor
    entities.append(make_player(320, 2496, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1

    entities.append(make_boundary("LeftBoundary",eid,-80,H//2,120,H)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,W+80,H//2,120,H)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,W//2,-40,W,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,W//2,H+40,W,80)); eid+=1

    entities.append(make_checkpoint("Checkpoint_Deep_Entry",eid,320,2432,"The Sunken Deep",[40,60,160,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Deep_Mid",eid,2880,1792,"Pressure Core",[30,50,140,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Deep_Top",eid,4800,512,"The Abyssal Eye",[20,40,120,255])); eid+=1

    entities.append(make_portal("Portal_To_Ascent",eid,200,2432,"scene_ascent.json","Shattered Spire",3200,256,[60,80,200,255])); eid+=1
    entities.append(make_portal("Portal_To_Crystal",eid,5700,2432,"scene_crystal.json","Crystal Labyrinth",5900,2048,[40,160,200,255])); eid+=1
    entities.append(make_portal("Portal_To_Boss",eid,4800,448,"scene_boss.json","The Abyssal Maw",380,1280,[200,40,60,255],require_boss=False)); eid+=1

    # The hardest enemies — dark variants
    deep_color = [60,80,160,255]
    elite_color = [100,120,220,255]
    enemy_defs = [
        # Ground corridor
        (640,2496,"crawler",6,120,300,deep_color),(1280,2496,"turret",5,0,560,deep_color),
        (1920,2496,"spitter",5,0,520,deep_color),(2560,2496,"crawler",6,125,320,elite_color),
        (3200,2496,"turret",5,0,560,deep_color),(3840,2496,"spitter",5,0,520,deep_color),
        (4480,2496,"crawler",7,130,340,elite_color),(5120,2496,"turret",6,0,580,elite_color),
        (5760,2496,"crawler",7,135,340,deep_color),
        # Mid corridors
        (960,1792,"crawler",6,120,300,deep_color),(2240,1792,"turret",5,0,560,deep_color),
        (3520,1792,"spitter",6,0,540,elite_color),(4800,1792,"crawler",7,130,340,elite_color),
        (960,1152,"turret",6,0,580,elite_color),(2240,1152,"crawler",7,130,340,elite_color),
        (3520,1152,"spitter",6,0,540,elite_color),(4800,1152,"turret",6,0,580,elite_color),
        # Top corridor near boss portal
        (1600,512,"crawler",8,140,360,[140,160,255,255]),
        (3200,512,"turret",7,0,600,[140,160,255,255]),
        (4800,448,"spitter",7,0,560,[140,160,255,255]),
    ]
    for ex,ey,etype,hp,spd,alert,c in enemy_defs:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd,alert=alert,color=c)); eid+=1

    # Lifts in shafts
    for lx,ly,dist,spd in [
        (672,2432,480,90),(1344,2432,480,90),(2016,1728,400,85),
        (2688,1728,400,85),(3360,1088,320,80),(4032,1088,320,80),
        (4704,448,280,75),(5376,448,280,75),
    ]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1

    # Dense spike traps
    for sx in range(448,5800,192):
        entities.append(make_spike(f"Spike_{eid}",eid,sx,2560)); eid+=1
    for sx in range(576,5800,256):
        if (sx//256)%2==0:
            entities.append(make_spike(f"SpikeC_{eid}",eid,sx,1856)); eid+=1

    # Dim, oppressive lanterns
    for lx,ly in [
        (320,2432),(1280,2432),(2560,2432),(3840,2432),(5120,2432),
        (960,1792),(2880,1792),(4800,1792),
        (1920,1152),(3840,1152),
        (2880,512),(4800,448),
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,[40,60,140,255],radius=160,intensity=0.7)); eid+=1

    # Crystal props in chambers
    for px,py in [
        (640,2432),(1920,2432),(3200,2432),(4480,2432),
        (1600,1792),(3200,1792),(4800,1152),(2880,448),
    ]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.4,1.4)); eid+=1
    for px,py in [
        (1280,2432),(2560,2432),(3840,2432),(5120,2432),
    ]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ─── Scene: scene_home.json EPIC REWORK ──────────────────────────────────────
# "Hollow Spire" — 96 cols x 48 rows (was 72x36) — much larger, richer hub
# ─────────────────────────────────────────────────────────────────────────────
def build_scene_home_epic():
    ROWS, COLS = 48, 96
    rock = empty_grid(ROWS, COLS)
    moss = empty_grid(ROWS, COLS)

    # ── Ground band — jagged ──
    for c in range(COLS):
        gbase = 36 + int(abs(smooth_noise(c*0.12, 0, 5)) * 2)
        for r in range(gbase, ROWS): rock[r][c] = 0

    # ── Left wall + right wall ──
    for r in range(6, 36):
        for c in range(0, 4): rock[r][c] = 0
        for c in range(92, 96): rock[r][c] = 0

    # ── Overhanging ceiling cliffs ──
    for r in range(0, 5):
        for c in range(0, 28): rock[r][c] = 0
        for c in range(70, 96): rock[r][c] = 0

    # ── Epic floating platforms — wide, many levels ──
    home_plats = [
        (30, 8,  20),  (28, 24, 38),  (31, 42, 56),  (29, 60, 74),  (30, 78, 90),
        (24, 4,  16),  (22, 20, 34),  (25, 38, 52),  (23, 56, 70),  (24, 74, 88),
        (18, 8,  22),  (16, 28, 44),  (19, 50, 64),  (17, 68, 84),
        (12, 12, 26),  (10, 36, 52),  (13, 58, 74),  (11, 78, 92),
        (7,  18, 32),  (6,  50, 66),  (8,  72, 86),
        (4,  26, 42),  (5,  60, 76),
    ]
    for row, cs, ce in home_plats:
        for c in range(cs, ce): rock[row][c] = 0
        for c in range(cs, ce): moss[row][c] = 0

    # ── Moss on ground surface ──
    for c in range(COLS):
        for r in range(2, ROWS-1):
            if rock[r][c] == 0 and rock[r-1][c] == -1: moss[r][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1
    for name,tex,depth,spd in [
        ("AbyssSkyDeep","bg_abyss_deep.png",-4800,0.016),
        ("AbyssSkyMid","bg_abyss_mid.png",-3000,0.038),
        ("AbyssSkyFront","bg_abyss_front.png",-1500,0.072),
        ("AbyssMist","bg_abyss_mist.png",-800,0.14),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd)); eid+=1

    entities.append(make_tilemap("HomeRockTerrain",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("HomeMossDetail",eid,"tile_moss.png",moss,gen_colliders=False)); eid+=1

    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(380, 2176, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1

    W, H = COLS*64, ROWS*64
    entities.append(make_boundary("LeftBoundary",eid,-80,H//2,120,H)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,W+80,H//2,120,H)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,W//2,-40,W,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,W//2,H+40,W,80)); eid+=1

    entities.append(make_checkpoint("Checkpoint_Home_Base",eid,380,2112,"Hollow Spire",[100,255,180,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Home_Mid",eid,3072,1280,"Spire Heart",[120,230,160,255])); eid+=1
    entities.append(make_checkpoint("Checkpoint_Home_Top",eid,2688,320,"Spire Crown",[140,200,255,255])); eid+=1

    # Portals to all areas
    entities.append(make_portal("Portal_To_Ascent",eid,5900,2240,"scene_ascent.json","Shattered Spire",3200,3456,[120,80,255,255])); eid+=1
    entities.append(make_portal("Portal_To_Crystal",eid,1700,576,"scene_crystal.json","Crystal Labyrinth",200,2048,[80,200,220,255])); eid+=1
    entities.append(make_portal("Portal_To_Verdant",eid,160,2112,"scene_verdant.json","Verdant Hollow",280,1440,[80,220,120,255])); eid+=1

    # Enemies across all platform levels
    for ex,ey,etype,hp,spd in [
        (700,2240,"crawler",3,88),(1300,2240,"crawler",3,90),
        (2000,2240,"spitter",2,0),(2800,2240,"crawler",3,92),
        (3600,2240,"crawler",3,88),(4400,2240,"spitter",2,0),
        (5000,2240,"crawler",3,90),(5600,2240,"crawler",4,95),
        (1000,1472,"crawler",4,95),(2200,1344,"spitter",3,0),
        (3400,1472,"crawler",4,98),(4600,1344,"spitter",3,0),
        (1600,832,"crawler",5,102),(3072,832,"crawler",5,105),
        (4500,832,"turret",3,0),(2300,448,"crawler",5,105),
        (3700,384,"spitter",4,0),
    ]:
        entities.append(make_enemy(f"{etype.capitalize()}_{eid}",eid,ex,ey,etype,hp=hp,speed=spd)); eid+=1

    # Lifts on some mid platforms
    for lx,ly,dist,spd in [(1500,2240,180,70),(3200,1536,220,75),(5200,1344,200,72)]:
        entities.append(make_lift(f"Lift_{eid}",eid,lx,ly,dist,spd)); eid+=1

    # Lanterns
    for lx,ly,c in [
        (380,2048,[200,160,80,255]),(1200,2048,[160,220,255,255]),(2400,2048,[200,160,80,255]),
        (3600,2048,[200,160,80,255]),(4800,2048,[160,220,255,255]),(5700,2048,[200,160,80,255]),
        (1000,1280,[140,200,255,255]),(2800,1280,[140,200,255,255]),(4600,1280,[140,200,255,255]),
        (1800,640,[120,180,255,255]),(3800,640,[120,180,255,255]),
        (2700,256,[100,160,255,255]),
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,c)); eid+=1

    # Spikes in gaps
    for sx in range(1000,5700,320):
        entities.append(make_spike(f"Spike_{eid}",eid,sx,2304)); eid+=1

    for px,py in [(600,2240),(1400,2240),(2600,2240),(4000,2240),(5300,2240)]:
        entities.append(make_rubble_prop(f"Rubble_{eid}",eid,px,py)); eid+=1
    for px,py in [(1600,512),(2600,256),(3800,384),(5000,448)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,1.2,1.2)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ─── Scene: scene_boss.json EPIC REWORK ──────────────────────────────────────
# "The Abyssal Maw" — 96 cols x 36 rows — largest boss arena ever
# Multiple phases reflected in arena geometry: pillars collapse in phase 2 (fake),
# spike fields, lava pits (spike proxies), and dramatic crystal lighting.
# ─────────────────────────────────────────────────────────────────────────────
def build_scene_boss_epic():
    ROWS, COLS = 36, 96
    rock    = empty_grid(ROWS, COLS)
    crystal = empty_grid(ROWS, COLS)

    # ── Floor ──
    for r in range(28, ROWS):
        for c in range(COLS): rock[r][c] = 0

    # ── Ceiling ──
    for r in range(0, 4):
        for c in range(COLS): rock[r][c] = 0

    # ── Side walls ──
    for r in range(4, 28):
        for c in range(0, 4): rock[r][c] = 0
        for c in range(92, 96): rock[r][c] = 0

    # ── 6 dramatic boss arena pillars ──
    pillar_xs = [12, 22, 36, 58, 72, 82]
    for px in pillar_xs:
        for r in range(18, 28):
            for c in range(px, px+4): rock[r][c] = 0
        for c in range(px, px+4): crystal[17][c] = 0  # crystal cap

    # ── Balcony ledges left/right ──
    for c in range(4, 12):  rock[18][c] = 0
    for c in range(84, 92): rock[18][c] = 0

    # ── Mid platform (breakable looking — same tile) ──
    for c in range(38, 58): rock[16][c] = 0
    # Crystal surface on mid platform
    for c in range(38, 58): crystal[15][c] = 0

    # ── Crystal surface on floor ──
    for c in range(4, 92): crystal[27][c] = 0

    eid = 1000
    entities = []
    entities.append(make_camera(eid)); eid+=1

    for name,tex,depth,spd,c in [
        ("BossSkyDeep","bg_abyss_deep.png",-5000,0.008,[160,40,40,255]),
        ("BossSkyMid","bg_abyss_mid.png",-3200,0.020,[140,30,60,255]),
        ("BossSkyFront","bg_abyss_front.png",-1600,0.040,[120,20,80,255]),
        ("BossMist","bg_abyss_mist.png",-900,0.08,[200,40,80,255]),
    ]:
        entities.append(make_parallax(name,eid,tex,depth,spd,color=c)); eid+=1

    entities.append(make_tilemap("BossArenaRock",eid,"tile_rock.png",rock)); eid+=1
    entities.append(make_tilemap("BossArenaCrystal",eid,"tile_crystal.png",crystal,gen_colliders=False)); eid+=1

    for e in make_hud_entities(eid): entities.append(e); eid+=1
    entities.append(make_player(380, 1664, eid)); eid+=1
    entities.append(make_bolt_template(eid)); eid+=1
    entities.append(make_shard_template(eid)); eid+=1
    entities.append(make_slash_template(eid)); eid+=1
    entities.append(make_crawler_template(eid)); eid+=1

    W, H = COLS*64, ROWS*64
    entities.append(make_boundary("LeftBoundary",eid,-80,H//2,120,H)); eid+=1
    entities.append(make_boundary("RightBoundary",eid,W+80,H//2,120,H)); eid+=1
    entities.append(make_boundary("UpperCeiling",eid,W//2,-40,W,80)); eid+=1
    entities.append(make_boundary("DeepFloor",eid,W//2,H+40,W,80)); eid+=1

    entities.append(make_checkpoint("Checkpoint_BossEntry",eid,380,1600,"The Abyssal Maw",[255,80,80,255])); eid+=1
    entities.append(make_portal("Portal_To_Home",eid,5700,1600,"scene_home.json","Hollow Spire",380,2176,[255,100,100,255],require_boss=True)); eid+=1

    # The Boss — massive scale
    entities.append({
        "active":True,"children":[],"id":eid,"name":"AbyssBoss","team":2,
        "hp":100,"max_hp":100,"damage":2,"speed":100.0,"phase":1,
        "components":{
            "BoxCollider2D":{"bounciness":0.0,"friction":0.3,"height":96,"is_trigger":False,"offset_x":0,"offset_y":0,"width":96},
            "Rigidbody2D":{"angular_drag":0.05,"drag":0.0,"freeze_rotation":True,"gravity_scale":1.0,"is_kinematic":False,"mass":8.0,"velocity_x":0.0,"velocity_y":0.0},
            "ScriptComponent":{"scripts":["abyss_boss"]},
            "SpriteRenderer":{"color":[255,255,255,255],"flip_x":False,"flip_y":False,"layer":2,"opacity":1.0,"order_in_layer":9,"pixels_per_unit":1.0,"source_h":64,"source_w":64,"source_x":0,"source_y":0,"texture":"boss_abyss.png","use_source_rect":True},
            "Transform":make_transform(3072,1600,2.5,2.5)
        }
    }); eid+=1

    # Mini-boss adds for phase 2 (inactive, activated by boss script)
    for ax,ay in [(1000,1600),(5144,1600)]:
        entities.append(make_enemy(f"BossMinion_{eid}",eid,ax,ay,"crawler",hp=8,speed=140,alert=9999,color=[200,60,80,255])); eid+=1

    # Spike fields (edge lava pits)
    for sx in range(256,800,64):   entities.append(make_spike(f"Spike_{eid}",eid,sx,1792)); eid+=1
    for sx in range(5440,5940,64): entities.append(make_spike(f"Spike_{eid}",eid,sx,1792)); eid+=1
    # Center pit
    for sx in range(2624,3584,64): entities.append(make_spike(f"Spike_{eid}",eid,sx,1792)); eid+=1

    # Boss atmosphere lanterns — very dramatic
    for lx,ly,c,r2,i in [
        (380,1536,[255,40,40,255],320,1.4),(1000,1152,[200,40,255,255],300,1.3),
        (2000,960,[255,40,40,255],320,1.4),(3072,256,[200,40,255,255],360,1.5),
        (4144,960,[255,40,40,255],320,1.4),(5144,1152,[200,40,255,255],300,1.3),
        (5764,1536,[255,40,40,255],320,1.4),(3072,1600,[255,255,80,255],400,1.6),
    ]:
        entities.append(make_lantern(f"Lantern_{eid}",eid,lx,ly,c,r2,i)); eid+=1

    for px,py,sx,sy in [(740,1728,1.5,1.5),(1388,1728,1.5,1.5),(4656,1728,1.5,1.5),(5240,1728,1.5,1.5)]:
        entities.append(make_crystal_prop(f"CrystalProp_{eid}",eid,px,py,sx,sy)); eid+=1

    return {"entities": entities, "sorting_layers": SORTING_LAYERS}


# ── Write all epic scenes ──────────────────────────────────────────────────────
import os
os.makedirs("/home/claude/game5_epic_out", exist_ok=True)
scenes = {
    "scene.json":          build_scene_title(),      # kept from original
    "scene_home.json":     build_scene_home_epic(),  # enlarged hub
    "scene_boss.json":     build_scene_boss_epic(),  # epic boss arena
    "scene_verdant.json":  build_scene_verdant(),    # kept rich original
    "scene_flooded.json":  build_scene_flooded(),    # kept rich original
    "scene_ascent.json":   build_scene_ascent(),     # NEW — vertical climb
    "scene_crystal.json":  build_scene_crystal(),    # NEW — crystal labyrinth
    "scene_deep.json":     build_scene_deep(),       # NEW — sunken deep
}
for fname, data in scenes.items():
    with open(f"/home/claude/game5_epic_out/{fname}", "w") as f:
        json.dump(data, f, indent=2)
    tiles=sum(1 for e in data["entities"] if "Tilemap" in e.get("components",{}))
    print(f"{fname}: {len(data['entities'])} entities, {tiles} tilemaps")
import os
os.makedirs("/home/claude/game5_out", exist_ok=True)
for fname, data in scenes.items():
    with open(f"/home/claude/game5_out/{fname}", "w") as f:
        json.dump(data, f, indent=2)
    print(f"Wrote {fname}: {len(data['entities'])} entities")
