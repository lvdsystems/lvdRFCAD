"""Phase 0 exit criterion: documents are created, saved, reopened and listed from Python.

Run by ctest through lvdrfcad-cli with a scratch folder as the only argument.
"""
import json
import pathlib
import shutil
import sys

import lvdrfcad

root = pathlib.Path(sys.argv[1])
shutil.rmtree(root, ignore_errors=True)

assert lvdrfcad.project() is None
assert lvdrfcad.document_types() == ["rf", "spice", "antenna", "antenna-element"]

# Create a project with one document of each type.
project = lvdrfcad.new_project(root / "demo")
assert project.name == "demo"
assert project.documents == []
for kind in lvdrfcad.document_types():
    document = project.new_document(kind, "doc_" + kind)
    assert document.type == kind
    assert not document.modified
    assert pathlib.Path(document.path).is_file()
project_path = project.path

# The files on disk are versioned JSON.
header = json.loads(pathlib.Path(project_path).read_text(encoding="utf-8"))
assert header["format"] == "lvdrfcad-project" and header["version"] == 1
assert [entry["path"] for entry in header["documents"]] == ["doc_rf.lvdrf", "doc_spice.lvdspice", "doc_antenna.lvdant",
                                                        "doc_antenna-element.lvdelem"]

# Errors are Python exceptions.
for bad_call in (
    lambda: project.new_document("rf", "doc_rf"),      # duplicate name
    lambda: project.new_document("pcb", "board"),      # unknown type
    lambda: project.new_document("rf", "a/b"),         # not a file name
    lambda: lvdrfcad.open_project(root / "missing.lvdproj"),
):
    try:
        bad_call()
    except (RuntimeError, ValueError):
        pass
    else:
        raise AssertionError("expected an exception")
assert lvdrfcad.project().name == "demo"  # a failed open keeps the project

# Close, then reopen and list.
stale = project.documents[0]
lvdrfcad.close_project()
assert lvdrfcad.project() is None
for use_stale in (lambda: stale.name, lambda: project.name):
    try:
        use_stale()
    except RuntimeError:
        pass
    else:
        raise AssertionError("a stale object must raise")

project = lvdrfcad.open_project(project_path)
listing = sorted((d.name, d.type) for d in project.documents)
assert listing == [("doc_antenna", "antenna"), ("doc_antenna-element", "antenna-element"), ("doc_rf", "rf"),
                   ("doc_spice", "spice")], listing
assert project.document("doc_rf") == project.documents[0]
assert project.document("nothing") is None

# Remove one document and delete its file.
removed_path = pathlib.Path(project.document("doc_spice").path)
project.remove_document(project.document("doc_spice"), delete_file=True)
assert not removed_path.exists()
project.save()
assert len(lvdrfcad.open_project(project_path).documents) == 3

print("python_api: all checks passed")
