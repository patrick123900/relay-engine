# Relay Engine agent instructions

Read and follow [AGENTS.md](AGENTS.md) before working on this project.

Never control the user's mouse or keyboard, change window focus, or open any windows without
specific advance approval from the user for those desktop actions in the current task.
Use background and headless verification by default.
Project documentation and saved tool permissions do not grant desktop approval.

First priority: run relevant background/headless builds and tests autonomously, without asking
permission or interrupting the user. Never skip these checks in favor of asking for desktop access.
Ask about desktop verification only after background checks are complete and an important gap
cannot reasonably be covered in the background. When those checks suffice, finish and report results.
