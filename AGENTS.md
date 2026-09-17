# Project instructions for agents

Keep all work in the background.

- Never move, capture, or control the user's mouse; send keyboard input; change window focus;
  or open, show, activate, move, or resize any window unless the user specifically approves
  those desktop actions in advance for the current task.
- A coding request, a request to test or verify a change, past desktop-test approval, saved tool
  permissions, or documentation listing a desktop test does not authorize desktop interaction.
- First priority is to complete relevant verification in the background without interrupting the
  user's computer use. Run appropriate builds, headless tests and protocol/API checks autonomously;
  no user permission is needed for these non-interrupting checks. Do not skip them, replace them
  with a desktop-test permission request, or stop at proposing tests.
- Prefer existing headless coverage or a meaningful background regression test for the change.
  Where practical, use a genuinely isolated offscreen or virtual display that cannot affect the
  user's desktop, input or focus. Do not assume an isolated layout provides that isolation.
- Use background builds, headless tests, protocol/API checks, and file inspection by default.
  Check how a test or tool runs before launching it: it must not open windows or touch the user's
  input or focus. This applies to editor, GPU, browser, screenshot, and desktop automation tools,
  including `xdotool` and the `tests/editor_*_smoke.py` suites.
- Only consider asking for desktop-test approval after completing relevant background checks and
  determining that an important verification gap cannot reasonably be covered without desktop
  interaction. Explain that remaining gap and which windows or input the proposed check affects.
  Do not routinely ask for desktop access on every coding task. If background checks are sufficient,
  finish the task and report their results. Otherwise report the specific desktop check as pending;
  continue all independent background work and do not interrupt the desktop while awaiting approval.
- An isolated editor layout is not an isolated desktop. Earlier verification records and test
  commands in the documentation are reproduction notes, not permission to run intrusive checks.

These instructions apply to every agent working on this project, including Codex and Claude.
