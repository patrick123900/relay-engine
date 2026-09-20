# Agent instructions

Work in the background and keep the repository suitable for public distribution.

- Do not move or control the user's mouse, send keyboard input, change focus, or open, show, move,
  resize, or activate windows without the user's specific advance approval for those desktop actions
  in the current task. Coding or test requests, old approvals, saved permissions, and documented
  commands do not grant desktop access.
- Complete relevant background verification autonomously: builds, headless tests, protocol/API
  checks, and file inspection need no permission. Prefer meaningful headless regression coverage.
- Before running any editor, browser, GPU, screenshot, or automation test, confirm it cannot affect
  the user's desktop. An isolated editor layout is not an isolated desktop. The
  `tests/editor_*_smoke.py` suites use the real desktop and require explicit approval.
- Ask about desktop verification only after background checks are complete and a material gap cannot
  be covered safely. Continue all independent work while approval is pending.
- Preserve unrelated working-tree changes. Do not put personal activity, private user context, or
  credentials in tracked files, commits, or repository comments.

Use [`HANDOFF.md`](HANDOFF.md) for current engineering state and [`docs/protocol.md`](docs/protocol.md)
for the generated control-protocol reference.
