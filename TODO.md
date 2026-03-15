# ESP Attendance Error Fixes - v3.0

## [x] Step 1: Fix Header Comment (CRITICAL BLOCKER)
- Replace em dash (—) → hyphen (-)
- Clean ASCII-only header before #includes

## [ ] Step 2: Rebuild & Analyze Next Errors
- pio run → identify compilation issues

## [ ] Step 3: Fix Common ESP8266 Issues
- [ ] va_list scope (bug in debugPrintF)
- [ ] Large stack arrays → static/global  
- [ ] String → char[] (heap safety)
- [ ] Add #include <stdarg.h> 
- [ ] WDT: Add more yield() in loops

## [ ] Step 4: Runtime Stability
- [ ] Test upload + serial monitor
- [ ] Heap/WiFi/sensor verification

## [ ] Step 5: Complete
- Clean build + working system

