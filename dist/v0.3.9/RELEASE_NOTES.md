# Free Harvest v0.3.9

UI corrections found by photographing the app for the README, plus new
documentation.

### Fixed: unthemed inputs

The batch name and notes fields rendered as white boxes on the dark theme. Form
styling was scoped to `.field`, and the recipe fields use `.rcpfield`, so they
inherited the browser default and nothing else. Only obvious once someone
looked at a screenshot.

### Fixed: two strings that contradicted the app

With control enabled, the idle screen still said *"Press START on the dryer when
you're ready"* while offering a Start button, and the guidance card tagged every
action `on dryer` directly beneath a Controls card offering the same button.
Both now reflect whether control is switched on.

### Added: screenshot tooling

`tools/shotserver.py` serves the real `main/www/index.html` against canned API
responses, and `tools/shots.sh` captures the set headlessly. The Candy editor
needs STAT type 43 and the Custom editor type 31, so photographing them
otherwise means driving a real freeze dryer between screens for documentation -
a poor reason to press buttons on someone's machine.

The UI itself is unmodified, so the screenshots are photographs of the shipped
interface rather than mock-ups of it.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
