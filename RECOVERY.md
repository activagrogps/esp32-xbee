# Recovery from commit 1c8cb36 (golden, 22/09/2025)

This tree = the EXACT working state that produced esp32-xbee.bin
(commit 1c8cb36, "Update wifi.c 22.09 line 255", built Sep 22 2025
06:55 UTC, ESP-IDF v4.4, classic ESP32) — recovered from git history,
NOT reconstructed — plus only:

- S3 compile fixes: status_led.c (#ifdef LEDC low-speed + valid S3
  GPIOs, keeping 17/18 free for UM980), web_server.c (esp_rom_crc.h)
- sdkconfig.defaults (shared) + sdkconfig.defaults.esp32s3 (S3-only)
- .github/workflows/build.yml — CI on espressif/idf:release-v4.4,
  matrix build esp32 + esp32s3, flashable artifacts
- components/button vendored (submodules arrive empty in zip downloads)
- .gitmodules removed (button is vendored)

The golden state itself differs from nebkat upstream in ONLY 2 files:
- main/main.c:  mdns_init_service() — hostname "esp32-rtk",
                instance "ESP32-RTK-Config", called after wifi_init()
- main/wifi.c:  mdns_register_http_service() — _http._tcp port 80,
                called from handle_sta_got_ip (line 255)
Verified against binary strings at offsets 0x2ec4/0x2ed0.

# Fixing the GitHub repo (activagrogps/esp32-xbee)

Contamination map:
- master:    1c8cb36 + merged PR #1 (S3-gemeni) = BROKEN
- s3-imu:    branched from contaminated master = BROKEN
- S3-gemeni: the contamination source

Commands (from any clone of the repo):

    git fetch origin
    # golden branch, never touch it again
    git branch golden 1c8cb36
    git push origin golden
    # replace s3-imu with this tree's content
    git checkout -B s3-imu golden
    # ...copy the S3 changes from this zip over the working dir...
    git add -A && git commit -m "S3 port on golden base (1c8cb36)"
    git push --force-with-lease origin s3-imu

Leave master and S3-gemeni as-is for now (history/evidence);
work happens on s3-imu, releases can later fast-forward master
to it once S3 is field-proven.

# Build

CI: push -> Actions builds both targets automatically.
Local: IDF v4.4.8, `idf.py set-target esp32|esp32s3`, `idf.py build`.
