---
description: "{Use when... trigger phrases for subagent discovery}"
name: "Agent Name"
tools: [{minimal set of tool aliases}]
model: "Claude Sonnet 4.5"
argument-hint: "Task..."
agents: [agent1, agent2]
user-invocable: true
disable-model-invocation: false
handoffs: []
hooks:
  PreToolUse:
    - type: command
      command: "./scripts/validate.sh"
  PostToolUse:
    - type: command
      command: "./scripts/format.sh"
---

You are a specialist at {specific task}. Your job is to {clear purpose}.

## Constraints
- DO NOT {thing this agent should never do}
- DO NOT {another restriction}
- ONLY {the one thing this agent does}

## Approach
1. {Step one of how this agent works}
2. {Step two}
3. {Step three}

## Output Format
{Exactly what this agent should return}
