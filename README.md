# Cqeta1564's update
Hi, I added a few new things to code from [@Yourigh](https://github.com/Yourigh). If you want more info look at [Jurajs' original repository](https://github.com/Yourigh/maker_badge_fw). Feel free to use and make it even more great.
I used the code as a starting point, because it already has some great features built in. Originaly this project was based on older version of the code, but I redo it to the newer version to be able to use it with all versions of the badge. 
## Quick start guide
You can find full guide in the original repo. 
1. Clone the repo
2. rename "config_template.h" to "config.h" and fill it in
3. upload it to the badge

## Add support for pictures
Add "bitmaps.h" with bitmaps for pictures.
### Lines in "main.cpp" with this feature
9, 146, 232 - 233

## Add support for more fonts
My used fonts are from the same library/family. More info on https://learn.adafruit.com/adafruit-gfx-graphics-library/using-fonts.
### Lines in "main.cpp" with this feature
16, 241

## Add support for ESP-Now
Communicate between a group of badges and sync color of the leds. This is not point-to-point communication!
### Lines in "main.cpp" with this feature
18 - 19, 40, 57, 119 - 121, 154, 174 - 176, 298 - 495
