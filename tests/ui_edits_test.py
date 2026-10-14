"""Builds tests/ui_edits_test.c for the host, runs azimuth_ui.c's edits on the game's own
files (read from the game folder, written to a temporary folder) and checks the results
are well-formed XML.
Usage: python tests/ui_edits_test.py <zig> <ForzaHorizon2 Mount folder>"""
import pathlib, subprocess, sys, tempfile, xml.dom.minidom, zipfile

hook = pathlib.Path(__file__).resolve().parent.parent
zig, mount = sys.argv[1], pathlib.Path(sys.argv[2])
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    ui = zipfile.ZipFile(mount / "media" / "UI.zip")
    flows = zipfile.ZipFile(mount / "media" / "stateflow.zip")
    files = {
        "options.xaml": ui.read("Scenes/Anthem/A_PauseOptions.xaml"),
        "pause.xaml": ui.read("Scenes/Anthem/A_PauseMenu.xaml"),
        "ui_screens.xml": flows.read("ui_screens.xml"),
        "pause_flow.xml": flows.read("pause.xml"),
    }
    for name, data in files.items():
        (tmp / name).write_bytes(data)
    exe = tmp / "ui_edits_test.exe"
    subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-I", str(hook), str(hook / "tests" / "ui_edits_test.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)] + [str(tmp / n) for n in files], check=True)
    for out in sorted(tmp.glob("*.*.*")):
        if out.suffix in (".options", ".infinity", ".pause", ".flow"):
            xml.dom.minidom.parseString(out.read_bytes())
            print("well-formed:", out.name)
