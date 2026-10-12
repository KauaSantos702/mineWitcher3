"""Mechanical part of the SkyCraft -> MineWitcher3 Fabric port.

Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.

Copies SkyCraft's fabric/ tree (upstream commit bfcaf17) into this folder, renaming packages,
classes, identifiers, protocol names and log prefixes, and adds the MIT attribution header to every
Java file. Files that MineWitcher3 drops are skipped. The hand-made changes on top (Proto.java,
W3Combat, W3Client, the entrypoints, mixin lists, fabric.mod.json, build files) are NOT redone by
this script: run it into an empty folder and diff against this one to pull in upstream changes.

    python tools/port_from_skycraft.py <skycraft checkout>/fabric <output folder>
"""

import os
import re
import shutil
import sys

HEADER = "// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.\n"

# Files left out of the port (Skyrim-only, or tied to SkyCraft's own services).
DROP = {
    "src/main/java/dev/skycraft/mixin/SmithingMixin.java",          # trains Skyrim's Smithing skill
    "src/client/java/dev/skycraft/client/DiscordPresence.java",     # SkyCraft's Discord application
    "src/client/java/dev/skycraft/client/mixin/ChatComponentMixin.java",  # only feeds DiscordPresence
}

# Class renames (file names follow). Longest first so prefixes don't eat longer names.
CLASSES = [
    ("SkyCraftClient", "MineWitcher3Client"),
    ("SkyrimActorEntity", "WitcherActorEntity"),
    ("SkyrimHitResult", "GameHitResult"),
    ("SkyDigClient", "W3DigClient"),
    ("SkyDigBlast", "W3DigBlast"),
    ("SkyCollision", "W3Collision"),
    ("SkyCollider", "W3Collider"),
    ("SkyRayTest", "W3RayTest"),
    ("SkyCombat", "W3Combat"),
    ("SkyClient", "W3Client"),
    ("SkyAtlas", "W3Atlas"),
    ("SkyWater", "W3Water"),
    ("SkyLink", "WitcherLink"),
    ("SkyClip", "W3Clip"),
    ("SkyState", "GameState"),
    ("SkyNet", "W3Net"),
    ("SkyDig", "W3Dig"),
    ("SkyRay", "W3Ray"),
    ("SkyTri", "W3Tri"),
    ("SkyCraft", "MineWitcher3"),
]

IDENTS = [
    (r"\bskycraft_protocol\.h\b", "mw3_protocol.h"),
    (r"skycraft\$", "mw3$"),
    (r"\breadSkyState\b", "readGameState"),
    (r"\bskyStateSeq\b", "gameStateSeq"),
    (r"\bOFF_SKY_STATE\b", "OFF_GAME_STATE"),
    (r"\bSKY_IN_GAME\b", "GAME_IN_GAME"),
    (r"\bSKY_MENU_OPEN\b", "GAME_MENU_OPEN"),
    (r"\bSKY_LOADING\b", "GAME_LOADING"),
    (r"\bSS_", "GS_"),
    (r"\bH_SKYRIM_PID\b", "H_GAME_PID"),
    (r"\bH_SKYRIM_HEARTBEAT\b", "H_GAME_HEARTBEAT"),
    (r"\bHURT_BLOCKED_IN_SKYRIM\b", "HURT_BLOCKED_IN_GAME"),
    (r"\bHURT_POWER_ATTACK\b", "HURT_HEAVY_ATTACK"),
    (r"\bSKYRIM_ACTOR_KEY\b", "WITCHER_ACTOR_KEY"),
    (r"\bSKYRIM_ACTOR\b", "WITCHER_ACTOR"),
    (r"\bSKYRIM_TO_MC_DAMAGE\b", "GAME_TO_MC_DAMAGE"),
    (r"\bskyrim_actor\b", "witcher_actor"),
    (r"\bquitWithSkyrim\b", "quitWithGame"),
    (r"\bQUIT_WITH_SKYRIM\b", "QUIT_WITH_GAME"),
    (r"formId", "actorId"),
    (r"FormId", "ActorId"),
    (r"SKYCRAFT\$", "MW3$"),
    (r"SKYCRAFT_", "MW3_"),
    (r"\bDovahkiin\b", "Geralt"),
    (r"Local\\\\SkyCraft_", r"Local\\\\MineWitcher3_"),
    (r"Local\\SkyCraft_", r"Local\\MineWitcher3_"),
]


def sentence_start(text, i):
    j = i - 1
    while j >= 0 and text[j] in " \t":
        j -= 1
    return j < 0 or text[j] in "\n.!?\"*/(:" or text[j - 1:j + 1] == "//"


def translate(text):
    for old, new in CLASSES:
        text = re.sub(r"\b%s\b" % old, new, text)
    for pat, rep in IDENTS:
        text = re.sub(pat, rep, text)
    # Identifiers that still contain Skyrim (fields, locals, mixin handler names): -> game.
    text = re.sub(r"(?<=[A-Za-z0-9_$])Skyrim|Skyrim(?=[A-Za-z0-9_])", "Game", text)
    text = re.sub(r"(?<=[A-Za-z0-9_$])skyrim|skyrim(?=[A-Z0-9_])", "game", text)
    text = re.sub(r"(?<=[A-Za-z0-9_$])SKYRIM|SKYRIM(?=[A-Z0-9_])", "GAME", text)
    # Prose (comments, log messages): "Skyrim" -> "the game".
    out, last = [], 0
    for m in re.finditer(r"\bSkyrim\b", text):
        out.append(text[last:m.start()])
        out.append("The game" if sentence_start(text, m.start()) else "the game")
        last = m.end()
    out.append(text[last:])
    text = "".join(out)
    text = re.sub(r"\ba the game\b", "the game", text)
    text = re.sub(r"\bthe the game\b", "the game", text)
    text = re.sub(r"\b(own|stand-in|that|their|The|A|a) the game\b", r"\1 game", text)
    text = text.replace("SKSE plugin", "native plugin (mw3.asi)").replace("SKSE side", "native (mw3.asi) side")
    text = text.replace("worldspace or interior cell", "W3 world, by name hash")
    text = text.replace("The game destruction", "World destruction")
    # Packages, mod id, resource names, system properties.
    text = text.replace("dev.skycraft", "dev.minewitcher3").replace("dev/skycraft", "dev/minewitcher3")
    text = re.sub(r"\bskycraft\b", "minewitcher3", text)
    text = text.replace("skycraft.", "minewitcher3.").replace("skycraft_", "minewitcher3_")
    return text


def out_path(rel):
    rel = rel.replace("dev/skycraft", "dev/minewitcher3").replace("assets/skycraft", "assets/minewitcher3")
    rel = rel.replace("data/skycraft", "data/minewitcher3").replace("skycraft.", "minewitcher3.")
    d, f = os.path.split(rel)
    stem, ext = os.path.splitext(f)
    for old, new in CLASSES:
        if stem == old:
            stem = new
    return os.path.join(d, stem + ext)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in (".gradle", "build", "run", ".idea")]
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            if rel in DROP:
                print("drop", rel)
                continue
            target = os.path.join(dst, out_path(rel))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if rel.startswith("gradle/") or rel in ("gradlew", "gradlew.bat", ".gitattributes"):
                shutil.copyfile(full, target)  # Gradle's own wrapper (Apache-2.0), unchanged
                continue
            if not rel.endswith((".java", ".json", ".gradle", ".properties")):
                shutil.copyfile(full, target)
                continue
            text = translate(open(full, encoding="utf-8").read())
            if rel.endswith(".java"):
                text = HEADER + text
            with open(target, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            print("port", rel, "->", out_path(rel))


if __name__ == "__main__":
    main()
