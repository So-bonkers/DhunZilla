"""Backend adapter packages discovered by :mod:`ai_sidecar.registry`.

Each subpackage (``mockgen``, ``mocksep``, later ``demucs`` …) must expose a module-level
``BACKEND_CLASS`` attribute: the :class:`ai_sidecar.base.BackendBase` subclass to register.

Usage example (``backends/mockgen/__init__.py``)::

    from .impl import MockGenBackend
    BACKEND_CLASS = MockGenBackend
"""
