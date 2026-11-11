"""Test isolation for the optional dashboard access token.

The operator's local ``.env`` may contain a real ``DASHBOARD_ACCESS_TOKEN`` for
tunnel use. Tests must not depend on it, so the token is forced empty in the
pytest process. Access-control tests monkeypatch the setting explicitly.
"""
import os

os.environ["DASHBOARD_ACCESS_TOKEN"] = ""
