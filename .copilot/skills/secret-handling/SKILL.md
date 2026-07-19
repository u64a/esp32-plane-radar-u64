---
name: "secret-handling"
description: "Protect credentials and provisioning secrets"
domain: "security"
confidence: "high"
source: "team-decision"
---

## Rules

- Never read live environment-secret files; use documented examples or ask the user.
- Never persist Wi-Fi passwords, provisioning passwords, tokens, private keys, or full
  connection strings in source, logs, Squad state, test fixtures, or commit messages.
- Never print the generated provisioning password to serial output.
- Use placeholders when documenting required configuration.
- Scan staged content before every commit and block the commit if credential-shaped
  content is found.

## Firmware-Specific Handling

- Provisioning credentials exist only for the active temporary SoftAP session.
- Tests use injected deterministic fake values that are clearly non-production.
- Factory reset and credential replacement must be explicit, transactional operations.
- Error diagnostics may identify the failing subsystem but must not include secrets or
  full untrusted API payloads.

## If Exposure Is Suspected

Stop the commit or release, identify only the affected file and credential category,
and ask the user to rotate the credential. Do not copy the secret into a report while
describing the incident.
