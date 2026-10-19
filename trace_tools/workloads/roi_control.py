"""Small ctypes interface to Callgrind instrumentation client requests."""

from __future__ import annotations

import ctypes
from pathlib import Path


class ROIControl:
    def __init__(self, library: str | Path) -> None:
        self._library = ctypes.CDLL(str(Path(library).resolve()))
        self._library.cachetrace_roi_start.argtypes = []
        self._library.cachetrace_roi_start.restype = None
        self._library.cachetrace_roi_stop.argtypes = []
        self._library.cachetrace_roi_stop.restype = None

    def start(self) -> None:
        self._library.cachetrace_roi_start()

    def stop(self) -> None:
        self._library.cachetrace_roi_stop()

