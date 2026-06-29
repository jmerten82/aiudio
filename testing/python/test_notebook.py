"""Execute every project notebook end to end — each must run with zero cell errors.

Notebooks are living documentation/acceptance walkthroughs; this keeps them honest (they
can't silently rot). Two are covered:
  * notebooks/aiudio_pipeline_tour.ipynb            — the teaching tour
  * testing/notebooks/aiudio_acceptance_walkthrough.ipynb — every feature + its shortcomings

Marked `slow` (each spins a Jupyter kernel) and skips cleanly if the execution stack or a
`python3` kernel isn't available.

    pytest testing/python/test_notebook.py        # runs if deps present
    pytest -m "not slow"                          # skip notebook execution
"""
from __future__ import annotations

import pathlib

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
NOTEBOOKS = sorted(
    [*(REPO / "notebooks").glob("*.ipynb"), *(REPO / "testing" / "notebooks").glob("*.ipynb")]
)


@pytest.mark.slow
@pytest.mark.parametrize("nb_path", NOTEBOOKS, ids=lambda p: p.name)
def test_notebook_executes_cleanly(nb_path):
    nbformat = pytest.importorskip("nbformat")
    pytest.importorskip("nbclient")
    from jupyter_client.kernelspec import NoSuchKernel
    from nbclient import NotebookClient

    nb = nbformat.read(str(nb_path), as_version=4)
    client = NotebookClient(
        nb, timeout=300, kernel_name="python3",
        resources={"metadata": {"path": str(nb_path.parent)}},
    )
    try:
        client.execute()
    except NoSuchKernel:
        pytest.skip("no 'python3' Jupyter kernel registered (run: python -m ipykernel install --user)")

    errors = [o for c in nb.cells for o in c.get("outputs", []) if o.get("output_type") == "error"]
    assert not errors, f"{nb_path.name}: {len(errors)} cell error(s)"
