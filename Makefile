.PHONY: build upload monitor clean

build:
	pio run

upload:
	@if [ -z "$(PORT)" ]; then echo "Usage: make upload PORT=/dev/ttyUSB0"; exit 1; fi
	pio run -t upload --upload-port $(PORT)

monitor:
	@if [ -z "$(PORT)" ]; then echo "Usage: make monitor PORT=/dev/ttyUSB0"; exit 1; fi
	pio device monitor --port $(PORT) --baud 115200

clean:
	pio run -t clean
