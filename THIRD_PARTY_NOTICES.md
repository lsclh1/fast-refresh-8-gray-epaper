# Third-party notices

The release panel driver is an independent implementation based on the [SSD1680 register specification](https://cdn-learn.adafruit.com/assets/assets/000/097/631/original/SSD1680_Datasheet.pdf) and the register setup for the [Waveshare 2.13-inch V4](https://github.com/waveshareteam/e-Paper/blob/master/Arduino/epd2in13_V4/epd2in13_V4.cpp). The short paired-mask waveform approach is informed by [epd-fast](https://github.com/TruElysia02/epd-fast). [InkBoy](https://github.com/kuslota/InkBoy) and [SmoothEPD](https://github.com/PinkiePie1/SmoothEPD) are earlier references for the experimental work.

Firmware builds use the installed [Arduino-ESP32](https://github.com/espressif/arduino-esp32) runtime, Espressif SPI APIs, and its `miniz` inflater. The host tool uses separately installed [Pillow](https://github.com/python-pillow/Pillow) and [pyserial](https://github.com/pyserial/pyserial) releases. FFmpeg is an optional system dependency for converting local video files. Each dependency retains its upstream license.

The packaged demo draws its own cards, bars, and labels. Video timing data contains measured counts and checksums. The source footage is separate from the release.
