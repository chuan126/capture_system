from __future__ import annotations

import os
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_STATIC_DIR = PROJECT_ROOT / "frontend" / "out"


def resolve_static_dir() -> Path:
    configured_path = os.getenv("CAPTURE_STATIC_DIR")
    if configured_path:
        return Path(configured_path).expanduser().resolve()
    return DEFAULT_STATIC_DIR


def create_app(static_dir: Path | None = None) -> FastAPI:
    site_directory = (static_dir or resolve_static_dir()).resolve()

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        index_file = site_directory / "index.html"
        if not index_file.is_file():
            raise RuntimeError(
                "Frontend static export is missing. "
                f"Expected {index_file}; run scripts/build/build_web.sh first."
            )
        yield

    application = FastAPI(
        title="Capture System Web API",
        version="0.1.0",
        lifespan=lifespan,
    )

    @application.get("/api/health", tags=["system"])
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    application.mount(
        "/",
        StaticFiles(directory=site_directory, html=True, check_dir=False),
        name="frontend",
    )
    return application


app = create_app()
