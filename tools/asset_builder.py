"""Build the location overrides from the user's FH3 files and install the mod."""

import argparse
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile


HERE = Path(__file__).resolve().parent
ENTITIES = "Entities/Australia/"
TRACKS = "tracks/australia/trackroutes/"
STOCK_TRIGGERS = "tracks/australia/triggerzones/tz_carmeets/car_meet_activation_triggers.tz"
TRIGGERS = "tracks/australia/triggerzones/tz_cml/car_meet_location_triggers.tz"
STOCK_ROUTE = "entities_route_offline_car_meet_airfield"
CINEMATICS = "Global/CarMeet/"
SPECTATOR_CLIP = "cml_spectators.xml"
SPECTATOR_SOURCE = "Global/E3/E3_Car_Select_Master_02.xml"


def decode_bxml(data):
    if data[:5] != b"BXML\x02":
        raise ValueError("An Airfield entity file is not FH3 BXML version 2.")
    count, string_size = struct.unpack_from("<II", data, 5)
    position = 13

    def take(size):
        nonlocal position
        end = position + size
        if end > len(data):
            raise ValueError("A source BXML file is truncated.")
        value = data[position:end]
        position = end
        return value

    def number(size):
        return int.from_bytes(take(size), "little")

    strings = [take(number(2)).decode("utf-8") for _ in range(count)]
    if position != 13 + string_size:
        raise ValueError("A source BXML string table has an unexpected length.")
    index_size = 2 if count > 255 else 1

    def read_string():
        return strings[number(index_size)]

    def element():
        flags = number(1)
        if flags not in (1, 3, 5, 7):
            raise ValueError("A source BXML element has an unsupported format.")
        node = ET.Element(read_string())
        if flags & 2:
            for _ in range(number(1)):
                key = read_string()
                node.set(key, read_string())
        if flags & 4:
            for _ in range(number(2)):
                node.append(element())
        return node

    if number(1) != 1:
        raise ValueError("An Airfield entity file does not have a single root.")
    root = element()
    if position != len(data):
        raise ValueError("Unexpected data after the source BXML document.")
    return root


def encode_bxml(root):
    strings = {}
    for node in root.iter():
        for string in (node.tag, *node.attrib.keys(), *node.attrib.values()):
            strings.setdefault(string, len(strings))
    index_size = 2 if len(strings) > 255 else 1
    table = bytearray()
    for string in strings:
        data = string.encode("utf-8")
        table.extend(struct.pack("<H", len(data)))
        table.extend(data)
    result = bytearray(b"BXML\x02" + struct.pack("<II", len(strings), len(table)) + table + b"\x01")

    def write_string(string):
        result.extend(strings[string].to_bytes(index_size, "little"))

    def element(node):
        result.append(1 | (2 if node.attrib else 0) | (4 if len(node) else 0))
        write_string(node.tag)
        if node.attrib:
            result.append(len(node.attrib))
            for key, value in node.attrib.items():
                write_string(key)
                write_string(value)
        if len(node):
            result.extend(struct.pack("<H", len(node)))
            for child in node:
                element(child)

    element(root)
    return bytes(result)


def required(parent, xpath):
    node = parent.find(xpath)
    if node is None:
        raise ValueError(f"Expected {xpath} in {parent.tag}. Check the FH3 game build.")
    return node


def property_value(parent, component, name, value):
    required(parent, f"property[@component='{component}'][@id='{name}']").set("value", str(value))


def xyz(point):
    return ",".join(f"{value:g}" for value in point)


def locator(sample, name, point, yaw):
    node = deepcopy(sample)
    required(node, "Name").set("value", name)
    transform = required(node, "SceneTransform")
    radians = math.radians(yaw)
    sine, cosine = math.sin(radians), math.cos(radians)
    for field, value in zip(("11", "13", "31", "33", "41", "42", "43"), (cosine, -sine, sine, cosine, *point)):
        transform.set(f"value._{field}", f"{value:.8f}")
    return node


def add_trigger(zones, sample, name, center, radius):
    node = deepcopy(sample)
    node.set("name", name)
    for axis, value in zip("xyz", center):
        required(node, "position").set(axis, str(value))
        required(node, "size").set(axis, str(radius))
    zones.append(node)


def as_xml(root):
    ET.indent(root)
    return ET.tostring(root, encoding="utf-8") + b"\n"


def loc_hash(name):
    # FH3 case-insensitive string-table hash. IDS_Airfield is 0x840B9A26.
    value = 0xFFFFFFFF
    for char in name.lower():
        value ^= ord(char)
        value = ((value << 7) | (value >> 25)) & 0xFFFFFFFF
    return value


def add_crowd_states(flow):
    states = {state.get("id"): state for state in flow.findall("state")}
    changed = {
        "load_cutscenes", "unload_cutscenes",
        "play_character_slot_cutscenes",
    }
    while True:
        ancestors = {
            name for name, state in states.items()
            if any(node.get("state") in changed for node in state.iter("node"))
        }
        expanded = changed | ancestors
        if expanded == changed:
            break
        changed = expanded
    for name, original in states.items():
        if name not in changed:
            continue
        state = deepcopy(original)
        state.set("id", "cml_" + name)
        for node in state.iter("node"):
            if node.get("state") in changed:
                node.set("state", "cml_" + node.get("state"))
        if name in ("load_cutscenes", "unload_cutscenes"):
            action = "global.load_cutscene_and_wait" if name == "load_cutscenes" else "global.unload_cutscene"
            ET.SubElement(state, "node", {"state": action, "cutscene_component_id": "crowd_cutscene_0"})
        elif name == "play_character_slot_cutscenes":
            ET.SubElement(state, "node", {
                "state": "global.set_cutscene_looping", "value": "true",
                "cutscene_component_id": "crowd_cutscene_0",
            })
            ET.SubElement(state, "node", {
                "state": "global.prepare_and_play_cutscene", "cutscene_component_id": "crowd_cutscene_0",
            })
        elif name == "enter":
            state.remove(required(state, "node[@trigger_name='FirstCarMeetAlt']"))
            brake = required(state, "node[@id='apply_parking_brake_human']")
            state.insert(list(state).index(brake), ET.Element("node", {
                "state": "global.place_player_car_at_route_locator",
                "route_locator_name": "cml_player_bay",
                "entity_id": "track_route/route",
                "is_sibling": "false",
            }))
        elif name == "initialise_car_meet":
            state.remove(required(state, "node[@state='disable_slot_spawning']"))
            start = required(state, "node[@id='enable_drivatar_spawn']")
            state.insert(list(state).index(start) + 1, ET.Element("node", {
                "state": "global.wait_for_time", "time": "1",
            }))
            intro = required(state, "node[@id='play_intro_cutscene']")
            state.insert(list(state).index(intro), ET.Element("node", {
                "state": "cml_wait_for_parked_car_models",
            }))
        flow.append(state)

    wait = ET.SubElement(flow, "state", {"id": "cml_wait_for_parked_car_models"})
    ET.SubElement(wait, "entry", {"target": "."})
    ET.SubElement(wait, "behavior", {"id": "MultiCarLoading"})
    behavior = ET.SubElement(wait, "behavior", {
        "id": "WaitForCarMeetSlotState", "wait_for_all": "true",
        "has_state": "false", "target_state": "Loading",
    })
    ET.SubElement(behavior, "transition", {"id": "onReady"})


def parked_npc_slot_flow(media):
    with zipfile.ZipFile(media / "stateflow.zip") as archive:
        slots = ET.fromstring(archive.read("car_meet_slot.xml"))
    # An entity in the active added route scopes this change without a global
    # flag. The stock Airfield continues through its original entry state.
    native = required(slots, "state[@id='enter']")
    native.set("id", "cml_native_enter")
    enter = ET.SubElement(slots, "state", {"id": "enter"})
    check = ET.SubElement(enter, "node", {
        "state": "global.does_entity_exist", "entity": "track_route/cml_meet_active",
    })
    ET.SubElement(check, "exit", {"id": "yes", "target": "check_player"})
    ET.SubElement(check, "exit", {"id": "no", "target": "native"})
    check = ET.SubElement(enter, "node", {"id": "check_player", "state": "is_local_player"})
    ET.SubElement(check, "exit", {"id": "yes", "target": "native"})
    ET.SubElement(check, "exit", {"id": "no", "target": "parked"})
    for name, state in (("native", "cml_native_enter"), ("parked", "cml_parked_npc_entry")):
        node = ET.SubElement(enter, "node", {"id": name, "state": state})
        ET.SubElement(node, "exit", {"target": ".exit"})
    parked = ET.SubElement(slots, "state", {"id": "cml_parked_npc_entry"})
    ET.SubElement(parked, "node", {"state": "enter_wait_for_car", "ugc_timeout": "0"})
    # Start the native exit recording paused at its parked first frame. Seeking
    # to the end of an entrance recording does not seek its simulated car replay.
    ET.SubElement(parked, "node", {"state": "setup_car_for_exit_cutscene_start"})
    ET.SubElement(parked, "node", {"state": "apply_handbrake"})
    ET.SubElement(parked, "node", {"state": "show_car"})
    return as_xml(slots)


def preserve_entry_time(flow):
    # Apply equally to the original Airfield and the added meets. Do not
    # change the native map entity, introduction, or progression flags.
    for name, action in (("constrain_weather", "global.stop_time"),
                         ("unconstrain_weather", "global.dummy_state")):
        state = required(flow, f"state[@id='{name}']")
        for child in list(state):
            state.remove(child)
        ET.SubElement(state, "node", {"state": action})
    enter = required(flow, "state[@id='enter']")
    enter.remove(required(enter, "node[@state='network.enable_weather_auto_change']"))
    entry = required(flow, "state[@id='enter_offline_car_meet']")
    # Freeze the incoming clock before fade/loading. Native disable_car_meet
    # retains unconstrain_time_of_day/start_time to resume free roam on exit.
    first = required(entry, "node")
    entry.insert(list(entry).index(first), ET.Element("node", {"state": "global.stop_time"}))


def add_offline_entry_state(flow):
    entry = deepcopy(required(flow, "state[@id='enter_offline_car_meet']"))
    entry.set("id", "cml_enter_offline_car_meet")
    # Only the stock entry runs the introduction and sets its progression flags.
    entry.remove(required(entry, "node[@state='network.is_in_network_session']"))
    entry.remove(required(entry, "node[@state='introduce_car_meets_if_not_already']"))
    flow.append(entry)


def gate_stock_meet(free_roam, flow):
    stock_group = required(free_roam, "entity_group[@id='car_meets']")
    index = list(free_roam).index(stock_group)
    free_roam.remove(stock_group)
    loader = ET.Element("entity", {"id": "cml_stock_meet_progression"})
    component = ET.SubElement(loader, "component", {
        "id": "progression", "type": "GameStateMachineComponent",
    })
    ET.SubElement(component, "property", {"id": "Autostart", "value": "true"})
    ET.SubElement(component, "property", {
        "id": "TargetFlow", "value": "car_meet.cml_stock_meet_progression",
    })
    free_roam.insert(index, loader)

    gate = ET.SubElement(flow, "state", {"id": "cml_stock_meet_progression"})
    check = ET.SubElement(gate, "node", {
        "id": "check_introduction", "state": "global.is_ie_flag_set", "flag": "IntroducedCarMeets",
    })
    ET.SubElement(check, "exit", {"id": "yes", "target": "check_fans"})
    ET.SubElement(check, "exit", {"id": "no", "target": "wait"})
    fans = ET.SubElement(gate, "node", {
        "id": "check_fans", "state": "currency.has_currency", "type": "Fans", "amount": "900000",
    })
    ET.SubElement(fans, "exit", {"id": "true", "target": "load_stock"})
    ET.SubElement(fans, "exit", {"id": "false", "target": "wait"})
    wait = ET.SubElement(gate, "node", {"id": "wait", "state": "global.wait_for_time", "time": "1"})
    ET.SubElement(wait, "exit", {"target": "check_introduction"})
    load = ET.SubElement(gate, "node", {"id": "load_stock", "state": "cml_ensure_stock_meet"})
    ET.SubElement(load, "exit", {"target": ".exit"})

    ensure = ET.SubElement(flow, "state", {"id": "cml_ensure_stock_meet"})
    exists = ET.SubElement(ensure, "node", {
        "state": "global.does_entity_exist", "entity": "track_route/car_meets/car_meet_air_field",
    })
    ET.SubElement(exists, "exit", {"id": "yes", "target": ".exit"})
    ET.SubElement(exists, "exit", {"id": "no", "target": "load_stock"})
    load = ET.SubElement(ensure, "node", {
        "id": "load_stock", "state": "global.load_entity_group",
        "entity_group_file": stock_group.get("source").removesuffix(".xml"),
        "parent_group_path": "track_route", "wait_for_state_machine_exit": "false",
    })
    ET.SubElement(load, "exit", {"target": ".exit"})
    # The native introduction must have its stock map entity available before
    # initial_experience applies the feature state and starts the reveal animation.
    introduction = required(flow, "state[@id='introduce_car_meet_feature']")
    ET.SubElement(introduction, "node", {"state": "cml_ensure_stock_meet"})


def spectator_clip(media):
    with zipfile.ZipFile(media / "Cinematics.zip") as archive:
        source = ET.fromstring(archive.read(SPECTATOR_SOURCE))
        animations = {path.casefold() for path in archive.namelist()}
    with zipfile.ZipFile(media / "Cinematic_Assets.zip") as archive:
        models = {path.casefold() for path in archive.namelist()}

    original_scene = required(source, "Scene")
    original_group = required(original_scene, "Group")
    clip = ET.Element(source.tag, source.attrib)
    scene = ET.SubElement(clip, original_scene.tag, original_scene.attrib)
    group = ET.SubElement(scene, original_group.tag, original_group.attrib)
    talking = ("CrowdMale-Couple_Talking_2A_CharA_L1", "CrowdMale-Couple_Talking_2A_CharB_L1")
    selfie = ("CrowdMale-Selfie_MF", "CrowdFemale-Selfie_MF")
    # Keep the actors' offsets within each pair and stand them beyond the parked cars.
    for index, (x, z, names) in enumerate((
        (-24.2, 13.5, talking),
        (-9.8, 14.0, selfie),
        (4.9, 13.5, talking),
        (19.7, 14.0, selfie),
    )):
        first = required(original_group, f"AnimatedModel[@Name='{names[0]}']")
        for name in names:
            actor = deepcopy(required(original_group, f"AnimatedModel[@Name='{name}']"))
            model = actor.get("ModelPath").replace("\\", "/").casefold().removeprefix("cinematic_assets/")
            animation = actor.get("AnimPath").replace("\\", "/").casefold().removeprefix("cinematics/")
            if model not in models or animation not in animations:
                raise ValueError(f"The FH3 spectator assets for {name} are missing.")
            actor.set("Space", "route:car_meet_loc")
            actor.set("PosX", str(x + float(actor.get("PosX")) - float(first.get("PosX"))))
            actor.set("PosY", "0")
            actor.set("PosZ", str(z + float(actor.get("PosZ")) - float(first.get("PosZ"))))
            actor.set("Name", f"Spectator{index}_{name}")
            actor.set("ModelID", actor.get("Name"))
            actor.set("SnapToGround", "1")
            group.append(actor)
    timeline = deepcopy(required(source, "Timeline"))
    timeline.set("looping", "1")
    clip.append(timeline)
    return as_xml(clip)


def source_files(media, sites):
    with zipfile.ZipFile(media / "EntityModel.zip") as archive:
        stock_meets = decode_bxml(archive.read(ENTITIES + "car_meets.xml"))
        free_roam = decode_bxml(archive.read(ENTITIES + "entities_offline_freeroam.xml"))
        route_sample = decode_bxml(archive.read(ENTITIES + STOCK_ROUTE + ".xml"))
    with zipfile.ZipFile(media / "stateflow.zip") as archive:
        flow = ET.fromstring(archive.read("car_meet.xml"))
    routes = ET.parse(media / TRACKS / "route0.nt").getroot()
    route_locator_sample = required(ET.parse(media / TRACKS / "route2001.nt").getroot(), "Locator")
    stock_zones = ET.parse(media / STOCK_TRIGGERS).getroot()
    zones = ET.Element(stock_zones.tag, stock_zones.attrib)
    zone_sample = required(stock_zones, "triggerzone[@name='carmeet_airfield']")
    activation_sample = required(stock_meets, "entity[@id='car_meet_air_field']")
    meet_list = ET.Element("entity_group", {"id": "cml_car_meets"})
    triggers = deepcopy(required(stock_meets, "entity[@id='triggers']"))
    required(triggers, ".//property[@id='FilePath']").set(
        "value", "tz_cml\\car_meet_location_triggers.tz")
    meet_list.append(triggers)
    ET.SubElement(free_roam, "entity_group", {
        "id": "cml_car_meets", "source": "[track]\\cml_car_meets.xml",
    })
    files = {
        "Cinematics/" + CINEMATICS + SPECTATOR_CLIP: spectator_clip(media),
        "stateflow/car_meet_slot.xml": parked_npc_slot_flow(media),
    }
    preserve_entry_time(flow)
    add_crowd_states(flow)
    add_offline_entry_state(flow)
    gate_stock_meet(free_roam, flow)
    player_x, player_z, player_yaw = struct.unpack_from("<3f", stock_parking_layout(media))

    for index, site in enumerate(sites, start=101):
        slug = "cml_" + site["id"]
        tag = "carmeet_" + slug
        meet_id = site.get("meet_id", index)
        route_id = 1950 + meet_id
        route_collection = f"Route{route_id}"
        route_name = f"entities_route_offline_car_meet_{slug}"
        state_name = f"enter_offline_car_meet_{slug}"
        name_id = f"IDS_{slug}"
        display_id = f"IDS_DisplayName_{slug}"
        lineup = site["lineup"]
        entrance = site["entrance"]
        exit_point = site["exit"]
        fast_travel = site.get("fast_travel", exit_point)
        radius = site.get("radius", 8)

        activation = deepcopy(activation_sample)
        activation.set("id", f"car_meet_{slug}")
        for component, suffix, point, yaw in (
            ("exit_locator", "_exit", exit_point[:3], exit_point[3]),
            ("map_icon_locator", "_icon", entrance, exit_point[3]),
            ("spatial_ui_locator", "_spatial", entrance, exit_point[3]),
            ("fast_travel_locator", "_fasttravel", fast_travel[:3], fast_travel[3]),
        ):
            property_value(activation, component, "LocatorName", tag + suffix)
            property_value(activation, component, "FallbackPosition", xyz(point))
            routes.append(locator(route_locator_sample, tag + suffix, point, yaw))
        for component, suffix, size in (
            ("activation_trigger", "", radius),
            ("spatial_ui_trigger", "_spatial_trigger", 130),
        ):
            property_value(activation, component, "TriggerZoneName", tag + suffix)
            property_value(activation, component, "TriggerZonePosition", xyz(entrance))
            property_value(activation, component, "TriggerZoneDimensions", xyz((size, size, size)))
            add_trigger(zones, zone_sample, tag + suffix, entrance, size)
        property_value(activation, "activation_state_machine", "TargetFlow", "car_meet." + state_name)
        property_value(activation, "activation_handler", "CarMeetId", meet_id)
        property_value(activation, "activation_handler", "RouteNameSuffix", slug)
        property_value(activation, "activation_handler", "DisplayNameStringId", "CarMeets." + display_id)
        property_value(activation, "map_context", "TitleStringID", "CarMeets." + name_id)
        # The stock feature unlock requires Xbox Live. Scope the new
        # entrances to normal festival free roam instead.
        ET.SubElement(activation, "property", {
            "component": "car_meet_feature_toggle", "id": "Feature", "value": "Festival"
        })
        meet_list.append(activation)

        site_route = deepcopy(route_sample)
        ET.SubElement(site_route, "entity", {"id": "cml_meet_active"})
        route = required(site_route, "entity[@id='route']")
        property_value(route, "route_presentation", "RouteId", route_id)
        property_value(route, "route_transforms", "CollectionName", route_collection)
        meet = required(site_route, "entity[@id='car_meet']")
        property_value(meet, "ui_view_model", "ExitLocatorName", tag + "_exit")
        property_value(meet, "ui_view_model", "RouteNameSuffix", slug)
        property_value(meet, "fast_travel_locator", "CollectionName", route_collection)
        property_value(meet, "entry", "CarMeetId", meet_id)
        property_value(meet, "entry", "LocationName", slug)
        # Populate the added meets promptly and retain their lineup instead of
        # repeatedly evicting/replacing cars after the stock one-minute timer.
        for component, key, value in (
            ("drivatar", "SpawnIntervalMin", "0"),
            ("drivatar", "SpawnIntervalMax", "0"),
            ("slot_coordinator", "SlotEvictionDelay", "86400"),
            ("slot_coordinator", "SlotMinEvictionLifetime", "86400"),
        ):
            ET.SubElement(meet, "property", {"component": component, "id": key, "value": value})
        ET.SubElement(meet, "property", {
            "component": "activation_state_machine", "id": "TargetFlow",
            "value": "car_meet.cml_root_offline",
        })
        property_value(meet, "crowd_cutscene_0", "FileName", "CarMeet\\" + SPECTATOR_CLIP)
        for index in range(1, 4):
            meet.remove(required(meet, f"property[@component='crowd_cutscene_{index}'][@id='FileName']"))
        files[f"EntityModel/{ENTITIES}{route_name}.xml"] = encode_bxml(site_route)
        route_locators = ET.Element("TrackLocators")
        route_locators.append(locator(route_locator_sample, "car_meet_loc", lineup[:3], lineup[3]))
        yaw = math.radians(lineup[3])
        sine, cosine = math.sin(yaw), math.cos(yaw)
        player_bay = (
            lineup[0] + cosine * player_x + sine * player_z,
            lineup[1],
            lineup[2] - sine * player_x + cosine * player_z,
        )
        route_locators.append(locator(route_locator_sample, "cml_player_bay", player_bay,
                                      lineup[3] + math.degrees(player_yaw)))
        files[f"{TRACKS}route{route_id}.nt"] = as_xml(route_locators)

        entry = ET.SubElement(flow, "state", {"id": state_name})
        ET.SubElement(entry, "node", {
            "state": "cml_enter_offline_car_meet",
            "entity_group_file": "[track]\\" + route_name,
            "exit_position_locator_id": tag + "_exit",
        })

    files["stateflow/car_meet.xml"] = as_xml(flow)
    files[f"EntityModel/{ENTITIES}cml_car_meets.xml"] = encode_bxml(meet_list)
    files[f"EntityModel/{ENTITIES}entities_offline_freeroam.xml"] = encode_bxml(free_roam)
    files[f"{TRACKS}route0.nt"] = as_xml(routes)
    files[TRIGGERS] = as_xml(zones)
    return files


def stock_parking_layout(media):
    def point(node):
        return float(node.attrib["PosX"]), float(node.attrib["PosZ"])

    layout = []
    with zipfile.ZipFile(media / "Cinematics.zip") as archive:
        camera = ET.fromstring(archive.read(CINEMATICS + "UI_Panel_Cameras_01.xml"))
        for slot in range(1, 13):
            clip = ET.fromstring(archive.read(CINEMATICS + f"CarMeet_Car_{slot:02d}_Exit.xml"))
            parked = required(clip, ".//CarRecording")
            layout.extend((*point(parked), float(parked.attrib["Yaw"])))
            for tag, name in (
                ("Camera", f"CarMeet_Cam_{slot}"),
                ("UIPanel", f"Car_UI_Panel_{slot:03d}"),
            ):
                layout.extend(point(required(camera, f".//{tag}[@name='{name}']")))
    return struct.pack("<84f", *layout)


def pack(sites, files, media):
    def number(value):
        return struct.pack("<I", value)

    payload = bytearray(b"CML9" + stock_parking_layout(media) + number(len(sites)))
    for index, site in enumerate(sites, start=101):
        payload += number(site.get("meet_id", index))
        payload += struct.pack("<4f", *site["lineup"])
        slug = "cml_" + site["id"]
        for key, label in (
            (f"IDS_{slug}", site["name"].upper() + " CAR MEET"),
            (f"IDS_DisplayName_{slug}", site["name"]),
        ):
            data = label.encode("utf-8")
            if len(data) > 128:
                raise ValueError(f"The map label for {site['name']} is too long.")
            payload += number(loc_hash(key)) + number(len(data)) + data
    payload += number(len(files))
    for path, contents in files.items():
        name = path.encode("ascii")
        payload += number(len(name)) + number(len(contents)) + name + contents
    return payload


def verify_build(mount):
    with (mount / "forza_durango_release_final.exe").open("rb") as executable:
        executable.seek(0x3C)
        offset = struct.unpack("<I", executable.read(4))[0]
        executable.seek(offset)
        magic, machine, _, timestamp = struct.unpack("<4sHHI", executable.read(12))
    if magic != b"PE\0\0" or machine != 0x8664 or timestamp != 0x5B16ECCA:
        raise ValueError("This mod requires the Azimuth Xbox One FH3 build 1.0.125.0.")


def validate_sites(sites):
    if not isinstance(sites, list) or not 1 <= len(sites) <= 32:
        raise ValueError("locations.json must contain 1 to 32 sites.")
    names = set()
    meet_ids = set()
    for index, site in enumerate(sites, start=101):
        slug = site["id"]
        if not re.fullmatch(r"[a-z0-9_]{1,25}", slug) or slug in names:
            raise ValueError(f"Invalid or duplicate location ID: {slug}")
        if slug.replace("_", "") == "airfield":
            raise ValueError("Airfield is reserved for the base game.")
        names.add(slug)
        meet_id = site.get("meet_id", index)
        if type(meet_id) is not int or not 101 <= meet_id <= 132 or meet_id in meet_ids:
            raise ValueError(f"Invalid or duplicate meet ID for {slug}: {meet_id}")
        meet_ids.add(meet_id)
        for key, count in (("lineup", 4), ("entrance", 3), ("exit", 4), ("fast_travel", 4)):
            if key == "fast_travel" and key not in site:
                continue
            point = site[key]
            if len(point) != count or not all(math.isfinite(value) for value in point):
                raise ValueError(f"{slug} has an invalid {key} coordinate.")
        radius = site.get("radius", 8)
        if not 3 <= radius <= 30 or math.dist(site["entrance"], site["exit"][:3]) <= radius + 3:
            raise ValueError(f"{slug} needs an exit clear of its entrance trigger.")
        if "fast_travel" in site and math.dist(site["entrance"], site["fast_travel"][:3]) <= radius + 3:
            raise ValueError(f"{slug} needs a fast-travel point clear of its entrance trigger.")


