"""Execute the guided tour notebook end to end — it must run with zero cell errors.

The notebook is living documentation of the whole Python-controllable pipeline; this
keeps it honest (it can't silently rot). Marked `slow` (~10 s + a Jupyter kernel) and
skips cleanly if the Jupyter execution stack or a `python3` kernel isn't available.

    pytest testing/python/test_notebook.py            # runs if deps present
    pytest -m "not slow"                              # skip it
"""
from __future__ import annotations

import pathlib

import pytest

NB = pathlib.Path(__file__).resolve().parents[2] / "notebooks" / "aiudio_pipeline_tour.ipynb"


@pytest.mark.slow
def test_tour_notebook_executes_cleanly():
    nbformat = pytest.importorskip("nbformat")
    pytest.importorskip("nbclient")
    from jupyter_client.kernelspec import NoSuchKernel
    from nbclient import NotebookClient

    assert NB.exists(), f"notebook not found: {NB}"
    nb = nbformat.read(str(NB), as_version=4)
    client = NotebookClient(nb, timeout=180, kernel_name="python3",
                            resources={"metadata": {"path": str(NB.parent)}})
    try:
        client.execute()
    except NoSuchKernel:
        pytest.skip("no 'python3' Jupyter kernel registered (run: python -m ipykernel install --user)")

    errors = [o for c in nb.cells for o in c.get("outputs", []) if o.get("output_type") == "error"]
    assert not errors, f"notebook produced {len(errors)} cell error(s)"
