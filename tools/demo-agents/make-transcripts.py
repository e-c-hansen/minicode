#!/usr/bin/env python3
"""Writes made-up Claude Code transcripts for the agents demo scene.

    make-transcripts.py ROOT OUT

ROOT is the demo folder as the app sees it (the sessions' cwd) and OUT the
folder standing in for ~/.claude/projects (MUTIBLE_CLAUDE_PROJECTS). The
lines have the shape Claude Code 2.1 writes (see tests/fixtures/claude), with
only the fields mutible reads, and times under a few hours before now, so the
panel says "48 min ago" rather than a date.
"""
import datetime
import json
import os
import sys
import uuid

root, out = sys.argv[1], sys.argv[2]
now = datetime.datetime.now(datetime.timezone.utc)


def stamp(minutes_ago):
    t = now - datetime.timedelta(minutes=minutes_ago)
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (t.microsecond // 1000)


def session(sid, title, prompt, steps, reply, start):
    """steps: (tool, input, output, is_error) in order; a minute apart."""
    lines = []
    base = {"sessionId": sid, "cwd": root, "version": "2.1.318", "gitBranch": "main",
            "isSidechain": False, "userType": "external", "entrypoint": "cli"}
    t = start

    def add(kind, content, **extra):
        nonlocal t
        line = dict(base, type=kind, uuid=str(uuid.uuid4()), timestamp=stamp(t),
                    message={"role": kind, "content": content}, **extra)
        lines.append(line)
        t -= 0.4

    lines.append({"type": "ai-title", "aiTitle": title, "sessionId": sid})
    add("user", prompt, origin={"kind": "human"}, promptSource="typed")
    for i, (tool, args, output, error) in enumerate(steps):
        tid = "toolu_demo_%s_%d" % (sid[:8], i)
        add("assistant", [{"type": "tool_use", "id": tid, "name": tool, "input": args}])
        add("user", [{"tool_use_id": tid, "type": "tool_result", "content": output,
                      "is_error": error}])
    add("assistant", [{"type": "text", "text": reply}])
    folder = os.path.join(out, "-private-tmp-mutible-demo-demo")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, sid + ".jsonl")
    with open(path, "w") as f:
        for line in lines:
            f.write(json.dumps(line) + "\n")
    # The file's own date is the session's last activity to mutible (a
    # transcript written in the last minutes is a session still running).
    last = (now - datetime.timedelta(minutes=t)).timestamp()
    os.utime(path, (last, last))


hello = os.path.join(root, "hello.py")
test = os.path.join(root, "test_hello.py")
unittest_out = (
    "test_area (test_hello.CircleTest.test_area) ... ok\n"
    "test_perimeter (test_hello.CircleTest.test_perimeter) ... ok\n"
    "test_zero_radius (test_hello.CircleTest.test_zero_radius) ... ok\n"
    "\n"
    "----------------------------------------------------------------------\n"
    "Ran 3 tests in 0.001s\n"
    "\n"
    "OK")

session(
    "5d0c2a64-1b8e-4f43-9a51-2f0f5e7c9d10",
    "Add a perimeter method to Circle",
    "Add a perimeter method to Circle in hello.py, with tests.",
    [
        ("Read", {"file_path": hello}, "1\t# A sample Python file\n2\timport math\n", False),
        ("Edit", {"file_path": hello, "old_string": "    def area(self):",
                  "new_string": "    def perimeter(self):\n        return 2 * math.pi * self.radius\n\n    def area(self):"},
         "The file %s has been updated successfully." % hello, False),
        ("Write", {"file_path": test, "content": "import unittest\n"},
         "File created successfully at: %s" % test, False),
        ("Bash", {"command": "python3 -m unittest test_hello.py",
                  "description": "Run the new tests"},
         "F..\nFAIL: test_perimeter\nAssertionError: 21.99 != 21.991148575128552\n\n"
         "Ran 3 tests in 0.001s\n\nFAILED (failures=1)", True),
        ("Edit", {"file_path": test, "old_string": "assertEqual", "new_string": "assertAlmostEqual"},
         "The file %s has been updated successfully." % test, False),
        ("Bash", {"command": "python3 -m unittest -v test_hello.py",
                  "description": "Run the tests again"}, unittest_out, False),
    ],
    "Circle now has perimeter(), and test_hello.py covers area, perimeter, and a "
    "zero radius. All three tests pass.",
    start=48,
)

session(
    "a71e9b02-3c4d-4e5f-8a6b-7c8d9e0f1a2b",
    "Explain what vec/main.cpp returns",
    "What does vec/main.cpp return, and why?",
    [
        ("Read", {"file_path": os.path.join(root, "vec/main.cpp")}, "1\t#include \"vec.h\"\n", False),
        ("Read", {"file_path": os.path.join(root, "vec/vec.h")}, "1\t#pragma once\n", False),
        ("Bash", {"command": "c++ -std=c++17 vec/main.cpp -o /tmp/vec && /tmp/vec; echo $?",
                  "description": "Build and run it"}, "0", False),
    ],
    "It returns 0: q is (5, 6), whose squared length, 61, is over 50.",
    start=190,
)
