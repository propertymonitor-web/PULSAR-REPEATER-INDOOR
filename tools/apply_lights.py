#!/usr/bin/env python3
"""
apply_lights.py - add the OPTIONAL jar lights to a MeshCore checkout that already has Lodestar applied.

  python tools/apply_lodestar.py --meshcore MeshCore
  python tools/apply_lights.py   --meshcore MeshCore --env Heltec_v3_repeater --pin 5

Copies lodestar/Lights.h into MeshCore/src/helpers, patches examples/simple_repeater (MyMesh.h / MyMesh.cpp) so
every packet heard and every packet sent is reported to the lights, and edits the chosen PlatformIO environment to
build with the NeoPixel library and the data pin you name (--pin takes a number or a pin name such as WB_IO1).

Every edit must match exactly once; if upstream MeshCore moved something the script stops and says which anchor it
could not find, instead of producing a half-patched tree.
"""
import argparse
import glob
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIGHTS_H = os.path.join(HERE, "..", "lodestar", "Lights.h")
NEOPIXEL_LIB = "adafruit/Adafruit NeoPixel@^1.10.0"       # the same library MeshCore already uses for some boards


def die(msg):
    print(f"\nERROR: {msg}\n", file=sys.stderr)
    sys.exit(1)


def patch_once(text, pattern, repl, what, flags=0):
    found = re.findall(pattern, text, flags)
    if len(found) != 1:
        die(f"Upstream MeshCore changed: expected exactly 1 match for '{what}', found {len(found)}. "
            f"tools/apply_lights.py anchor for this edit needs updating.")
    return re.sub(pattern, lambda m: repl(m) if callable(repl) else repl, text, count=1, flags=flags)


GLUE = r'''
// --- LODESTAR jar lights (optional; see PULSAR-JAR-LIGHTS.txt). Built only when the environment defines LODESTAR_LIGHTS_PIN. ---
#ifdef LODESTAR_LIGHTS_PIN
#include <Adafruit_NeoPixel.h>
#ifndef LODESTAR_LIGHTS_ORDER
#define LODESTAR_LIGHTS_ORDER NEO_GRB
#endif
static Adafruit_NeoPixel ls_strip(lodestar::Lights::TOTAL, LODESTAR_LIGHTS_PIN, LODESTAR_LIGHTS_ORDER + NEO_KHZ800);
static void lodestarLightsInit() { ls_strip.begin(); ls_strip.clear(); ls_strip.show(); }
static void lodestarLightsShow(const lodestar::Lights::RGB* p) {
  for (int i = 0; i < lodestar::Lights::TOTAL; i++) ls_strip.setPixelColor(i, p[i].r, p[i].g, p[i].b);
  ls_strip.show();
}
#else
static void lodestarLightsInit() {}
static void lodestarLightsShow(const lodestar::Lights::RGB*) {}
#endif
'''

CLI_BRANCH = r'''} else if (memcmp(command, "lights", 6) == 0 && (command[6] == 0 || command[6] == ' ')) {
#ifdef LODESTAR_LIGHTS_PIN
    lights.runCommand(command + 6, reply, 150);   // lights [on|off|bright 0-100]; not saved, back to the build default at reboot
#else
    strcpy(reply, "Err - this build has no lights");
#endif
  '''


def add_env_flags(meshcore, env, pin):
    """Add the pin flag and the NeoPixel library to [env:<env>] in whichever platformio.ini defines it."""
    hits = [p for p in glob.glob(os.path.join(meshcore, "**", "platformio.ini"), recursive=True)
            if re.search(r"^\[env:" + re.escape(env) + r"\]\s*$", open(p, encoding="utf-8").read(), re.M)]
    if len(hits) != 1:
        die(f"Expected exactly one platformio.ini with [env:{env}], found {len(hits)}.")
    path = hits[0]
    lines = open(path, encoding="utf-8").read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.strip() == f"[env:{env}]")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    if any("LODESTAR_LIGHTS_PIN" in l for l in lines[start:end]):
        die("This environment already has the lights in it. Use a fresh clone.")
    for key, add in (("build_flags", f"  -D LODESTAR_LIGHTS_PIN={pin}"), ("lib_deps", f"  {NEOPIXEL_LIB}")):
        idx = [i for i in range(start, end) if re.match(rf"^{key}\s*=\s*$", lines[i])]
        if len(idx) != 1:
            die(f"[env:{env}] has no plain '{key} =' block to add to (found {len(idx)}); edit {path} by hand.")
        lines.insert(idx[0] + 1, add)
        end += 1
    open(path, "w", encoding="utf-8").write("\n".join(lines))
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meshcore", required=True, help="path to a MeshCore checkout (Lodestar already applied)")
    ap.add_argument("--env", required=True, help="PlatformIO environment, e.g. Heltec_v3_repeater")
    ap.add_argument("--pin", required=True, help="LED data pin: a number, or a pin name such as WB_IO1")
    args = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]{1,24}", args.pin):
        die("--pin must be a number or a pin name (letters, digits, underscore).")

    root = args.meshcore
    rep = os.path.join(root, "examples", "simple_repeater")
    h_path, c_path = os.path.join(rep, "MyMesh.h"), os.path.join(rep, "MyMesh.cpp")
    for p in (h_path, c_path, LIGHTS_H):
        if not os.path.isfile(p):
            die(f"Cannot find {p}. Is --meshcore pointing at a MeshCore checkout?")
    cpp, hdr = open(c_path).read(), open(h_path).read()
    if "lodestar::Lodestar lodestar;" not in hdr:
        die("Run tools/apply_lodestar.py first: the lights are an add-on to Lodestar.")
    if "lodestar::Lights" in hdr or "LODESTAR jar lights" in cpp:
        die("This MeshCore checkout already has the lights in it. Use a fresh clone.")

    env_path = add_env_flags(root, args.env, args.pin)
    shutil.copyfile(LIGHTS_H, os.path.join(root, "src", "helpers", "Lights.h"))

    hdr = patch_once(hdr, r"#include <helpers/Lodestar\.h>\n", "#include <helpers/Lodestar.h>\n#include <helpers/Lights.h>\n", "Lodestar include")
    hdr = patch_once(hdr, r"(  lodestar::Lodestar lodestar;[^\n]*\n)",
                     lambda m: m.group(1) + "  lodestar::Lights lights;       // jar lights (optional)\n", "Lodestar member")

    cpp = patch_once(cpp, r'(#include "MyMesh\.h"\n)', lambda m: m.group(1) + GLUE, "MyMesh.h include")
    cpp = patch_once(cpp, r"(void MyMesh::logRx\(mesh::Packet \*pkt, int len, float score\) \{\n)",
                     lambda m: m.group(1) + "  lights.onRx(millis());   // a packet was heard (receive bar)\n", "logRx")
    cpp = patch_once(cpp, r"(void MyMesh::logTx\(mesh::Packet \*pkt, int len\) \{\n)",
                     lambda m: m.group(1) + "  lights.onTx(millis());   // a packet was sent: forwarded, or one of our own (send bar)\n", "logTx")
    cpp = patch_once(cpp, r"(  _cli\.loadPrefs\(_fs\);\n)", lambda m: m.group(1) + "  lodestarLightsInit();\n", "loadPrefs call in begin")
    cpp = patch_once(cpp, r"(  lodestar\.loop\(millis\(\)\);\n)",
                     lambda m: m.group(1) + "  if (lights.update(millis())) lodestarLightsShow(lights.buf());\n", "lodestar.loop call")
    cpp = patch_once(cpp, r'\} else if \(memcmp\(command, "lodestar", 8\) == 0',
                     lambda m: CLI_BRANCH + m.group(0), "lodestar command branch")

    open(c_path, "w").write(cpp)
    open(h_path, "w").write(hdr)
    print("Jar lights applied:")
    print("  + src/helpers/Lights.h")
    print("  ~ examples/simple_repeater/MyMesh.h / MyMesh.cpp  (heard, sent, loop, `lights` command)")
    print(f"  ~ {os.path.relpath(env_path, root)}  ([env:{args.env}]: data pin {args.pin}, NeoPixel library)")


if __name__ == "__main__":
    main()
