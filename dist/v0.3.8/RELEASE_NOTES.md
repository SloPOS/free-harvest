# Free Harvest v0.3.8

Setup panel appears only when the dryer is on a configuration screen.

### One family at a time, chosen by the machine

The Candy/Custom selector is gone and the panel stays hidden until the dryer is
actually on a configuration screen - reached with Candy Setup or Custom Setup
from the home screen, exactly as on the panel itself. Screen 43 shows the Candy
editor, screen 31 the Custom one, and never both.

The machine already has a Candy/Custom selector. A second one in the app could
disagree with it, and then the panel would be editing a recipe family the dryer
is not showing.

Leaving a configuration screen also clears the edit buffer, so returning to one
reseeds from the machine rather than restoring someone's abandoned edits.

### Added: a way back

The panel now carries **Back to home screen**, which sends the Cancel button
for whichever configuration screen is showing - button 18 on Candy, 26 on
Custom.

This closes a gap introduced in v0.3.7: the mirrored button list hides itself
while the panel is up, so Cancel was not reachable from anywhere. The only ways
off a configuration screen were starting a batch or walking to the machine.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
