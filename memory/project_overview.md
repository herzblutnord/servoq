---
name: project-overview
description: ServoQ is an exact port of Ladybird's Qt UI using Servo as the browser engine instead of LibWeb. Fidelity contract: CHROME is an exact port, not an approximation. Servo embedding layer has no Ladybird equivalent — use Servo's real crate API exactly.
metadata:
  type: project
---

ServoQ = Ladybird Qt chrome (exact port) + Servo rendering engine (real crate API).

Reference: vendor/reference-ladybird/UI/Qt/ — DO NOT MODIFY vendor/.
Engine: servo crate (crates.io) — no stubbed/faked engine calls.

**Why:** milestone-based approach — get chrome right first, then integrate real engine per milestone.

**How to apply:** Before any chrome change, read the vendor reference file and cite the exact line. Before any engine API change, check doc.servo.org and use exact method names.
