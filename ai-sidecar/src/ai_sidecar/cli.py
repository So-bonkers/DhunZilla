"""``opsidecar`` CLI — serve the sidecar, or submit/status jobs against one.

Usage examples::

    uv run opsidecar serve --port 0        # prints "SIDECAR_PORT=<n>" on stdout
    python -m opsidecar serve              # same via the module alias
    uv run opsidecar jobs submit --backend mockgen --task generate \\
        --params '{"prompt": "chill", "duration_s": 1.0, "seed": 42}' --wait --port 8765
    uv run opsidecar jobs status <job_id> --port 8765
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path

import httpx


def _serve(args: argparse.Namespace) -> int:
    """Start uvicorn on a pre-bound socket; announce ``SIDECAR_PORT=<n>`` on stdout.

    The C++ app spawns us with ``--port 0`` and reads that line (spec lifecycle).
    Binding here and handing the socket to uvicorn avoids a bind-then-race pattern.
    """
    import uvicorn

    from ai_sidecar.app import create_app

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    actual_port = int(sock.getsockname()[1])
    sock.listen(128)  # kernel accepts into backlog before uvicorn takes over — no connect race
    print(f"SIDECAR_PORT={actual_port}", flush=True)

    app = create_app(jobs_dir=Path(args.jobs_dir), workers=args.workers)
    server = uvicorn.Server(
        uvicorn.Config(app, host=args.host, port=actual_port, log_level=args.log_level)
    )
    try:
        server.run(sockets=[sock])
    finally:
        sock.close()
    return 0


def _base_url(args: argparse.Namespace) -> str:
    return f"http://{args.host}:{args.port}"


def _jobs_submit(args: argparse.Namespace) -> int:
    """POST /jobs; prints the job_id (stdout). With --wait, polls to terminal state."""
    try:
        params = json.loads(args.params) if args.params else {}
    except json.JSONDecodeError as exc:
        print(f"--params is not valid JSON: {exc}", file=sys.stderr)
        return 2
    if not isinstance(params, dict):
        print("--params must be a JSON object", file=sys.stderr)
        return 2
    with httpx.Client(base_url=_base_url(args), timeout=30.0) as client:
        resp = client.post(
            "/jobs", json={"backend": args.backend, "task": args.task, "params": params}
        )
        if resp.status_code >= 400:
            print(json.dumps(resp.json(), indent=2), file=sys.stderr)
            return 1
        job_id = resp.json()["job_id"]
        print(job_id)
        if not args.wait:
            return 0
        while True:
            state = client.get(f"/jobs/{job_id}").json()
            if state["state"] in {"done", "failed", "cancelled"}:
                print(json.dumps(state, indent=2), file=sys.stderr)
                return 0 if state["state"] == "done" else 1
            time.sleep(0.2)


def _jobs_status(args: argparse.Namespace) -> int:
    """GET /jobs/{id}; pretty-prints the snapshot."""
    with httpx.Client(base_url=_base_url(args), timeout=30.0) as client:
        resp = client.get(f"/jobs/{args.job_id}")
        print(json.dumps(resp.json(), indent=2))
        return 0 if resp.status_code < 400 else 1


def build_parser() -> argparse.ArgumentParser:
    """Argparse tree for the ``opsidecar`` entry point."""
    parser = argparse.ArgumentParser(prog="opsidecar", description="DhunZilla AI sidecar")
    parser.add_argument(
        "--version", action="version", version=f"opsidecar {_version_string()}"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    serve = sub.add_parser("serve", help="run the sidecar HTTP server")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=0, help="0 = pick a free port")
    serve.add_argument("--jobs-dir", default="out/jobs")
    serve.add_argument("--workers", type=int, default=None)
    serve.add_argument("--log-level", default="info")
    serve.set_defaults(func=_serve)

    jobs = sub.add_parser("jobs", help="convenience client for a running sidecar")
    jobs_sub = jobs.add_subparsers(dest="jobs_command", required=True)

    submit_p = jobs_sub.add_parser("submit", help="submit a job")
    submit_p.add_argument("--host", default="127.0.0.1")
    submit_p.add_argument("--port", type=int, required=True)
    submit_p.add_argument("--backend", required=True)
    submit_p.add_argument("--task", required=True)
    submit_p.add_argument("--params", default="{}", help="JSON object of task params")
    submit_p.add_argument("--wait", action="store_true", help="poll until terminal state")
    submit_p.set_defaults(func=_jobs_submit)

    status_p = jobs_sub.add_parser("status", help="get a job's state")
    status_p.add_argument("--host", default="127.0.0.1")
    status_p.add_argument("--port", type=int, required=True)
    status_p.add_argument("job_id")
    status_p.set_defaults(func=_jobs_status)

    return parser


def _version_string() -> str:
    import ai_sidecar

    return ai_sidecar.__version__


def main(argv: list[str] | None = None) -> int:
    """Entry point (console script + ``python -m opsidecar``). Returns exit code."""
    parser = build_parser()
    args = parser.parse_args(argv)
    func = getattr(args, "func", None)
    if func is None:  # argparse required-subparsers makes this unreachable in practice
        parser.error("no command handler")
    return int(func(args))
