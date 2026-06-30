"""Execute every project notebook end to end — each must run with zero cell errors.

Notebooks are living documentation/acceptance walkthroughs; this keeps them honest (they
can't silently rot). Two are covered:
  * notebooks/aiudio_pipeline_tour.ipynb            — the teaching tour
  * testing/notebooks/aiudio_acceptance_walkthrough.ipynb — every feature + its shortcomings

The notebook is executed with the **same interpreter running pytest** (where `aiudio` is
installed) — via a throwaway kernelspec pointing at `sys.executable` — rather than a
globally-registered "python3" kernelspec, which may point at a different Python and yield
`ModuleNotFoundError: No module named 'aiudio'`.

Marked `slow` (each spins a Jupyter kernel) and skips cleanly if the Jupyter execution stack
isn't installed.

    pytest testing/python/test_notebook.py        # runs if deps present
    pytest -m "not slow"                          # skip notebook execution
"""
from __future__ import annotations

import json
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
NOTEBOOKS = sorted(
    [*(REPO / "notebooks").glob("*.ipynb"), *(REPO / "testing" / "notebooks").glob("*.ipynb")]
)


@pytest.fixture(scope="session")
def current_interpreter_kernel(tmp_path_factory):
    """A throwaway kernelspec whose kernel is the interpreter running the tests.

    Returns (kernel_spec_manager, kernel_name). Independent of any globally-registered
    'python3' kernel, so the notebook runs in the venv where `aiudio` is installed.
    """
    pytest.importorskip("ipykernel")
    from jupyter_client.kernelspec import KernelSpecManager

    root = tmp_path_factory.mktemp("kernels")
    name = "aiudio-current"
    spec_dir = root / name
    spec_dir.mkdir()
    (spec_dir / "kernel.json").write_text(
        json.dumps(
            {
                "argv": [sys.executable, "-m", "ipykernel_launcher", "-f", "{connection_file}"],
                "display_name": "aiudio (test interpreter)",
                "language": "python",
            }
        )
    )
    ksm = KernelSpecManager()
    ksm.kernel_dirs.insert(0, str(root))
    return ksm, name


@pytest.mark.slow
@pytest.mark.parametrize("nb_path", NOTEBOOKS, ids=lambda p: p.name)
def test_notebook_executes_cleanly(nb_path, current_interpreter_kernel):
    nbformat = pytest.importorskip("nbformat")
    pytest.importorskip("nbclient")
    from jupyter_client.manager import KernelManager
    from nbclient import NotebookClient

    ksm, kernel_name = current_interpreter_kernel
    nb = nbformat.read(str(nb_path), as_version=4)
    km = KernelManager(kernel_name=kernel_name, kernel_spec_manager=ksm)
    client = NotebookClient(
        nb, km=km, timeout=300,
        resources={"metadata": {"path": str(nb_path.parent)}},
    )
    client.execute()

    errors = [o for c in nb.cells for o in c.get("outputs", []) if o.get("output_type") == "error"]
    assert not errors, f"{nb_path.name}: {len(errors)} cell error(s)"
