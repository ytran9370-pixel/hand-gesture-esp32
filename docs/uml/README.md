# UML diagrams

Each diagram is written as text in [PlantUML](https://plantuml.com) (`.puml`) and exported to `.svg` for viewing on GitHub.
Edit the `.puml` file, then regenerate the `.svg` — never edit the `.svg` by hand.

| Diagram | Type | Shows |
|---|---|---|
| [architecture](architecture.svg) | Component | Blocks, FreeRTOS tasks, interfaces |
| [use_case](use_case.svg) | Use case | What the user can do with the system |
| [sequence_gesture](sequence_gesture.svg) | Sequence | One image from webcam to a light turning on |
| [sequence_rfid_unlock](sequence_rfid_unlock.svg) | Sequence | Unlocking with an RFID card |
| [state_system](state_system.svg) | State machine | LOCKED / UNLOCKED / LOCKOUT |
| [state_gesture_filter](state_gesture_filter.svg) | State machine | Gesture debounce (3 frames, cooldown) |
| [state_frame_parser](state_frame_parser.svg) | State machine | Byte-by-byte UART frame parser |

The class diagram of the PC application is added in Phase 3, from the real code.

## Regenerate the SVG files

Option 1 — online, nothing to install: paste the `.puml` text (and the content of `_style.iuml` instead of the `!include` line)
into the editor at plantuml.com, then download the SVG.

Option 2 — locally (needs Java):

```bash
java -jar plantuml.jar -tsvg docs/uml/*.puml
```
