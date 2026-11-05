"""Tests for Tools/FUSE/Remaster (plan docs/plans/FUSE_REMASTER_PLAN.md §3, §7 Wave 0).

Importing this package blocks the network for the whole test run (§3.5: CI never calls the network):
``socket.socket``, ``socket.create_connection`` and ``socket.getaddrinfo`` raise ``NetworkBlocked``.
Works under ``python -m unittest discover -t Tools/FUSE/Remaster -s tests`` and under pytest.
"""

import os
import socket
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)


class NetworkBlocked(RuntimeError):
    pass


NETWORK_ATTEMPTS = []


class _NoNetSocket(socket.socket):
    def __init__(self, *args, **kwargs):  # noqa: D401 - deliberately never constructs a socket
        NETWORK_ATTEMPTS.append(("socket", args))
        raise NetworkBlocked("network access attempted during fuse_ai tests (socket.socket)")


def _no_net(name):
    def fn(*args, **kwargs):
        NETWORK_ATTEMPTS.append((name, args))
        raise NetworkBlocked(f"network access attempted during fuse_ai tests (socket.{name})")

    return fn


if not getattr(socket, "_fuse_ai_tests_blocked", False):
    socket.socket = _NoNetSocket
    socket.create_connection = _no_net("create_connection")
    socket.getaddrinfo = _no_net("getaddrinfo")
    socket._fuse_ai_tests_blocked = True
