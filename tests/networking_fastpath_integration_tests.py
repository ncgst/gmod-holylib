"""Compile production CheckTransmit/PVS bodies with a deterministic engine fixture."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--msvc", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source", type=Path, help="Networking source override for regression controls")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    net = (args.source or root / "source/modules/networking.cpp").read_text(encoding="utf-8")
    prop = (root / "source/sourcesdk/ccservernetworkproperty.h").read_text(encoding="utf-8")
    fixture = (root / "tests/networking_fastpath_fixture.cpp").read_text(encoding="utf-8")
    bodies = {
        "GLOBAL_CACHE": between(net, "using TransmitPVSCache =", "\n#if 0 // Would be needed"),
        "IS_IN_PVS": between(net, "static inline bool IsInPVS(", "\nstatic vec_t g_nTransmitRange"),
        "DO_TRANSMIT": between(net, "static inline void DoTransmitPVSCheck(", "\nstatic ConVar networking_fastpath("),
        "CHECK_TRANSMIT": between(net, "bool New_CServerGameEnts_CheckTransmit(", "\nvoid SV_FillHLTVData("),
        "CHARACTER_TRANSMIT": between(net, "static void hook_CBaseCombatCharacter_SetTransmit(", "\nstatic inline bool IsInPVS("),
        "ATTACHMENT_EXCLUSION": between(net, "\t\t// First we build data based off all players", "\n\t\tfor (int i=0; i < nEdicts; ++i)"),
        "PVS": prop[prop.index("template <typename HeadnodeQuery>\ninline bool CCServerNetworkProperty::IsInPVS("):],
    }
    for name, body in bodies.items():
        marker = f"// PRODUCTION_{name}"
        assert fixture.count(marker) == 1
        fixture = fixture.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix="networking-fastpath-") as temp:
        cpp = Path(temp) / "fixture.cpp"
        binary = Path(temp) / ("fixture.exe" if args.msvc else "fixture")
        cpp.write_text(fixture, encoding="utf-8")
        if args.msvc:
            command = [os.environ.get("CXX", "cl"), "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/wd4100",
                       f"/I{root / 'source'}", str(cpp), f"/Fe{binary}", f"/Fo{Path(temp) / 'fixture.obj'}"]
            if args.sanitize:
                command += ["/fsanitize=address", "/Zi", f"/Fd{Path(temp) / 'fixture.pdb'}"]
        else:
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", f"-I{root / 'source'}", str(cpp), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=temp, check=True)
        subprocess.run([str(binary)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
