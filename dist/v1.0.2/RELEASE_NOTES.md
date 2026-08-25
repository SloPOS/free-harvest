# Free Harvest v1.0.2

**Four routes were silently dead in 1.0.0 and 1.0.1**, including the captive
portal endpoints a phone uses to find the setup hotspot.

`max_uri_handlers` was 32 against 37 registered routes. Handlers past the limit
fail to register, and whichever happen to be last simply 404 forever:

    /img/*                 dashboard images
    /generate_204          Android captive-portal probe
    /hotspot-detect.html   Apple captive-portal probe
    /connecttest.txt       Windows captive-portal probe

**If first-time Wi-Fi setup would not open the sign-in page on your phone, this
is the fix.**

This is the second time this limit has been overrun, and both times the only
evidence was a single warning buried in the boot log. So the limit is now 64,
and more importantly `reg()` checks the result and logs `ROUTE LOST` at ERROR
with the route's name. A route that fails to register is a broken build, not
something to mention in passing.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
