# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0

from gitlint.rules import CommitRule, RuleViolation


class BodyHasText(CommitRule):
    """gitlint counts a lone Signed-off-by as a body"""

    name = "body-has-text"
    id = "UC1"

    def validate(self, commit):
        lines = [line.strip() for line in commit.message.body]
        if not any(line and not line.lower().startswith("signed-off-by:") for line in lines):
            return [RuleViolation(self.id, "Body has no text besides its Signed-off-by")]
        return []
