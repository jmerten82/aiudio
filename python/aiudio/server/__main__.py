"""``python -m aiudio.server`` — run the localhost workbench server."""
from __future__ import annotations

import argparse

from .app import serve


def main() -> None:
    parser = argparse.ArgumentParser(prog="aiudio.server", description="aiudio workbench server")
    parser.add_argument("--host", default="127.0.0.1", help="bind host (default: localhost)")
    parser.add_argument("--port", type=int, default=8765, help="bind port (default: 8765)")
    parser.add_argument("--static", default=None,
                        help="serve a built frontend from this dir (e.g. web/dist); "
                             "omit in dev (use the Vite dev server)")
    args = parser.parse_args()
    serve(args.host, args.port, static_dir=args.static)


if __name__ == "__main__":
    main()
