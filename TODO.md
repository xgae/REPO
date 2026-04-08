# Arabic Support Upgrade - ESP Attendance v4.2

## [x] Step 0: Planning ✅

## [x] Step 1: PlatformIO - Add U8g2 OLED Library ✅
## [x] Step 2: main.cpp - Replace LCD → OLED ✅  
## [x] Step 3: Port initCustomChars → u8g2Init() ✅
## [x] Step 4: Arabic Font + include/arabic.h ✅

## [ ] Step 5: Port lcdPad → u8g2_drawStr + Replace Calls
## [ ] Step 6: Port All Animations (showBootAnimation, showSuccess etc.)
## [ ] Step 7: Port OTA Lambdas + setup()/loop() LCD calls
## [ ] Step 8: Test Build → Upload → Arabic OLED Test

**Notes**: 
- Hardware upgrade: Replace LCD → SSD1306 128x64 OLED (same I2C)
- Wiring: VCC=3.3V, GND, SDA=D2/GPIO4, SCL=D1/GPIO5
- U8g2 handles Arabic fonts + basic RTL

