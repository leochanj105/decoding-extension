"""PLDI's checker, used as ground truth for "is this token legal here?".

No new logic lives here. The paper's parser is driven as-is, so that whatever our
implementation produces can be compared against the system it is meant to match.

What "legal" means, which is easy to get wrong
----------------------------------------------
Legality is about *continuation*, not immediate type agreement. At

    let s: string = |

the token `x` is legal even when `x` is a number, because the expression can go on
to be `x.toString()` or `x + "a"`, both strings. But `x;` is illegal: the semicolon
commits it as a bare number where a string is required.

So a mask admits every token that leaves at least one well-typed completion open.
This is exactly why type *reachability* is needed and a plain "symbols whose type
is string" mask would be wrong.

Cost
----
One query reparses a candidate on top of the current state, so it is far too slow
to sweep a 151k vocabulary per position. Callers pass the candidate ids they care
about; maskgen/check.py decides which those are.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from typing import Any, Iterable, Optional

_REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "type-constrained-code-generation")
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from typesafe_llm.parser.parser_ts import (  # noqa: E402
    custom_end_initial_state,
    incremental_ts_parse,
)

# The benchmark wraps generated code in a fenced block, and the paper's parser is
# set up to stop at the closing fence.
END_MARKER = "```"


@dataclass(frozen=True)
class Oracle:
    """A position in a program, and what may legally follow it.

    Immutable: `advance` returns a new Oracle and `allows` does not disturb this
    one. The paper's parser states are frozen dataclasses, so trialling a
    candidate cannot corrupt the committed state -- which is what makes querying
    many candidates from one position safe.
    """

    state: Any

    @classmethod
    def at_start(cls, identifiers: Optional[dict] = None) -> "Oracle":
        return cls(state=custom_end_initial_state(END_MARKER, identifiers or {}))

    @property
    def readings(self) -> int:
        """How many interpretations of the code so far are still alive.

        Zero means the text has been rejected. The checker keeps every reading it
        cannot yet rule out, which is where its cost comes from.
        """
        return len(self.state.active_states)

    @property
    def alive(self) -> bool:
        return self.readings > 0

    def advance(self, text: str) -> "Oracle":
        """Commit `text`. The result may be dead, which means `text` was illegal."""
        return Oracle(state=incremental_ts_parse(self.state, text))

    def allows(self, text: str) -> bool:
        """Could the program continue with `text`? See the note on legality above."""
        if not text:
            return True
        return self.advance(text).alive

    def allowed_among(self, candidates: Iterable[tuple[int, str]]) -> set[int]:
        """Of these (token id, text) pairs, which are legal here.

        One reparse per candidate, so keep the list short and deliberate.
        """
        return {tid for tid, text in candidates if self.allows(text)}
